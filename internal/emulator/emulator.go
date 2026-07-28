// Package emulator provides the Spettrum ZX Spectrum emulator integration layer.
// It ties together the Z80 CPU, ULA renderer, keyboard, beeper, TAP loader,
// and snapshot loader into a runnable emulator.
package emulator

import (
	"fmt"
	"os"
	"os/signal"
	"sync/atomic"
	"time"

	"github.com/defik74/spettrum/internal/emulator/romdata"
	"github.com/defik74/spettrum/pkg/beeper"
	"github.com/defik74/spettrum/pkg/keyboard"
	"github.com/defik74/spettrum/pkg/snapshot"
	"github.com/defik74/spettrum/pkg/tap"
	"github.com/defik74/spettrum/pkg/ula"
	"github.com/defik74/spettrum/pkg/z80"
)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const (
	ROMStart  = 0x0000
	ROMSize   = 16 * 1024
	VRAMStart = 0x4000
	RAMStart  = 0x4000
	TotalRAM  = 48 * 1024
	TotalMem  = 64 * 1024
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Config holds emulator configuration.
type Config struct {
	ROMFile       string
	SnapshotFile  string
	TAPFile       string
	Instructions  int // 0 = unlimited
	DisasmFile    string
	RenderMode    ula.RenderMode
	SimKey        string
	Audio         bool
	Volume        int // 0-100
	QuickLoad     bool
}

// ---------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------

// Emulator ties all modules together.
type Emulator struct {
	cfg    Config
	mem    [TotalMem]uint8
	running atomic.Bool

	cpu     *z80.CPU
	display *ula.State
	kbd     *keyboard.State
	beeper  *beeper.Player

	tapPlayer *tap.Player

	// Disassembly
	disasmFile *os.File

	// Timing
	frameCycleCount uint64
	intAsserted     bool
	intAssertedAt   uint64

	// Debug tracking
	lastPC     [10]uint16
	lastOpcode [10]uint8
	histIdx    int
	totalInst  uint64

	// Anomaly tracking
	warnPCinVRAM int
	warnSPinVRAM int

	// Simulated keys
	simKeys      string
	simKeyIdx    int
	simKeyTimer  time.Time
	simKeyActive bool
}

// New creates a new emulator with the given configuration.
func New(cfg Config) *Emulator {
	e := &Emulator{
		cfg:    cfg,
		simKeys: cfg.SimKey,
	}
	return e
}

// Init initializes all emulator modules.
func (e *Emulator) Init() error {
	// Load ROM
	if e.cfg.ROMFile != "" {
		data, err := os.ReadFile(e.cfg.ROMFile)
		if err != nil {
			return fmt.Errorf("read ROM: %w", err)
		}
		copy(e.mem[ROMStart:], data)
	} else {
		// Use embedded ROM
		copy(e.mem[ROMStart:], romdata.DefaultROM)
	}

	// Load snapshot (if provided)
	if e.cfg.SnapshotFile != "" {
		cpuState, _, err := snapshot.Load(e.cfg.SnapshotFile, e.mem[:])
		if err != nil {
			return fmt.Errorf("load snapshot: %w", err)
		}
		_ = cpuState // Will apply after CPU creation
	}

	// Create CPU
	e.cpu = z80.NewCPU(e, e)

	// Load snapshot state if available
	if e.cfg.SnapshotFile != "" {
		cpuState, _, _ := snapshot.Load(e.cfg.SnapshotFile, e.mem[:])
		if cpuState != nil {
			e.applyCPUState(cpuState)
		}
	}

	// Set up I/O port handlers for keyboard
	for _, port := range []uint16{0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F} {
		port := port
		e.cpu.RegisterPortHandler(port,
			func(p uint16) uint8 { return e.kbd.ReadPort(p) },
			nil,
		)
	}

	// Create ULA
	mode := e.cfg.RenderMode
	if mode == 0 {
		mode = ula.RenderOCR
	}
	e.display = ula.New(e.mem[VRAMStart:VRAMStart+ula.TotalVRAM], mode)

	// Create keyboard
	e.kbd = keyboard.New()

	// Create beeper
	if e.cfg.Audio {
		vol := float64(e.cfg.Volume) / 100.0
		if vol > 1.0 { vol = 1.0 }
		if vol < 0.0 { vol = 0.0 }
		e.beeper = beeper.NewPlayer(vol)
		e.beeper.Start()
	}

	// Load TAP file
	if e.cfg.TAPFile != "" {
		if e.cfg.QuickLoad {
			_, _, err := tap.LoadToMemory(e.cfg.TAPFile, e.mem[:], RAMStart)
			if err != nil {
				return fmt.Errorf("quick-load TAP: %w", err)
			}
		} else {
			tp, err := tap.NewPlayer(e.cfg.TAPFile)
			if err != nil {
				return fmt.Errorf("open TAP player: %w", err)
			}
			e.tapPlayer = tp
		}
	}

	// Open disassembly file
	if e.cfg.DisasmFile != "" {
		f, err := os.Create(e.cfg.DisasmFile)
		if err != nil {
			return fmt.Errorf("create disasm file: %w", err)
		}
		e.disasmFile = f
	}

	return nil
}

// applyCPUState applies snapshot CPU state.
func (e *Emulator) applyCPUState(s *snapshot.CPUState) {
	r := &e.cpu.Regs
	r.A = s.A
	r.F = s.F
	r.B = s.B
	r.C = s.C
	r.D = s.D
	r.E = s.E
	r.H = s.H
	r.L = s.L
	r.IXh = s.IXh
	r.IXl = s.IXl
	r.IYh = s.IYh
	r.IYl = s.IYl
	r.A1 = s.A1
	r.F1 = s.F1
	r.B1 = s.B1
	r.C1 = s.C1
	r.D1 = s.D1
	r.E1 = s.E1
	r.H1 = s.H1
	r.L1 = s.L1
	r.PC = s.PC
	r.SP = s.SP
	r.I = s.I
	r.R = s.R
	r.IFF1 = s.IFF1
	r.IFF2 = s.IFF2
	r.IM = s.IM
}

// Run starts the emulation loop. Blocks until emulation stops.
func (e *Emulator) Run() error {
	// Set up signal handling
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)

	// Initialize terminal
	origTerm, err := ula.TermInit()
	if err != nil {
		return fmt.Errorf("term init: %w", err)
	}
	defer ula.TermCleanup(origTerm)

	e.running.Store(true)
	e.simKeyTimer = time.Now()

	// Render loop in a goroutine
	renderDone := make(chan struct{})
	go func() {
		defer close(renderDone)
		for e.running.Load() {
			frameStart := time.Now()
			frame := e.display.RenderFrame()
			os.Stdout.WriteString(frame)
			ula.WaitFrame(frameStart)
		}
	}()

	// Main CPU loop
	for e.running.Load() {
		select {
		case <-sigCh:
			e.running.Store(false)
		default:
		}

		// Check instruction limit
		if e.cfg.Instructions > 0 && e.totalInst >= uint64(e.cfg.Instructions) {
			e.running.Store(false)
			break
		}

		// Simulate key injection
		e.simulateKeys()

		// Execute one instruction
		pcBefore := e.cpu.Regs.PC
		opcode := e.mem[pcBefore]
		cycles := e.cpu.Step()

		// Record history
		e.lastPC[e.histIdx%10] = pcBefore
		e.lastOpcode[e.histIdx%10] = opcode
		e.histIdx++
		e.totalInst++

		// Accumulate cycles for interrupt timing
		e.frameCycleCount += uint64(cycles)

		// 50Hz ULA interrupt (every 70908 T-states)
		if e.frameCycleCount >= z80.FrameCycles {
			e.frameCycleCount -= z80.FrameCycles
			e.cpu.GenInt(0xFF)
		}

		// Check for anomalies
		if pcBefore >= VRAMStart && pcBefore < VRAMStart+ula.VRAMSize {
			e.warnPCinVRAM++
		}
		if e.cpu.Regs.SP >= VRAMStart && e.cpu.Regs.SP < VRAMStart+ula.VRAMSize {
			e.warnSPinVRAM++
		}

		// Update beeper from port 0xFE writes (handled via IO interface)
		// TAP player EAR bit
		if e.tapPlayer != nil && !e.tapPlayer.IsFinished() {
			earBit := e.tapPlayer.ReadEAR(int(e.frameCycleCount))
			// The EAR bit is injected into port 0xFE bit 6 via keyboard read
			_ = earBit
		}

		// Speed throttle (optional)
		// usleep equivalent would go here if needed
	}

	// Wait for render goroutine
	e.running.Store(false)
	<-renderDone

	// Show anomalies
	if e.warnPCinVRAM > 0 {
		fmt.Printf("\nWARNING: PC in VRAM %d times\n", e.warnPCinVRAM)
	}
	if e.warnSPinVRAM > 0 {
		fmt.Printf("WARNING: SP in VRAM %d times\n", e.warnSPinVRAM)
	}
	fmt.Printf("Total instructions: %d\n", e.totalInst)

	return nil
}

// simulateKeys injects simulated key presses if configured.
func (e *Emulator) simulateKeys() {
	if e.simKeys == "" {
		return
	}

	if !e.simKeyActive {
		// First key after initial delay
		if time.Since(e.simKeyTimer) > 3*time.Second {
			e.simKeyActive = true
			e.simKeyTimer = time.Now()
			e.kbd.InjectKey(e.simKeys[0])
			e.simKeyIdx = 1
		}
		return
	}

	// Subsequent keys every 500ms
	if time.Since(e.simKeyTimer) > 500*time.Millisecond {
		e.simKeyTimer = time.Now()
		if e.simKeyIdx < len(e.simKeys) {
			e.kbd.InjectKey(e.simKeys[e.simKeyIdx])
			e.simKeyIdx++
		}
	}
}

// Close cleans up emulator resources.
func (e *Emulator) Close() {
	if e.disasmFile != nil {
		fmt.Fprintf(e.disasmFile, "Total instructions: %d\n", e.totalInst)
		e.disasmFile.Close()
	}
	if e.beeper != nil {
		e.beeper.Close()
	}
	if e.tapPlayer != nil {
		e.tapPlayer.Close()
	}
	signal.Stop(make(chan os.Signal, 1))
}

// ---------------------------------------------------------------------------
// Memory and I/O interface implementations (for z80.CPU)
// ---------------------------------------------------------------------------

// ReadMemory implements z80.MemoryHandler.
func (e *Emulator) ReadMemory(addr uint16) uint8 {
	return e.mem[addr]
}

// WriteMemory implements z80.MemoryHandler. Blocks writes to ROM.
func (e *Emulator) WriteMemory(addr uint16, val uint8) {
	if addr < ROMSize {
		return // ROM is read-only
	}
	e.mem[addr] = val
}

// ReadIO implements z80.IOHandler.
func (e *Emulator) ReadIO(port uint16) uint8 {
	// Generic I/O read — port-specific handlers take precedence
	pl := port & 0xFF
	if pl == 0xFE {
		// Keyboard
		result := e.kbd.ReadPort(port)
		// Tape EAR bit (bit 6)
		if e.tapPlayer != nil && !e.tapPlayer.IsFinished() {
			if e.tapPlayer.ReadEAR(int(e.frameCycleCount)) != 0 {
				result &^= 0x40 // set bit 6 HIGH for EAR
			}
		}
		return result
	}
	return 0xFF
}

// WriteIO implements z80.IOHandler.
func (e *Emulator) WriteIO(port uint16, val uint8) {
	pl := port & 0xFF
	if pl == 0xFE {
		// Border color (bits 0-2)
		e.display.SetBorderColor(val & 0x07)
		// Beeper (bit 4) and MIC (bit 3)
		if e.beeper != nil {
			e.beeper.Update(e.frameCycleCount, val&0x10 != 0)
		}
		// Keyboard row select (bits 5-7 of upper byte are row)
		e.kbd.SetRowSelector(uint8(port >> 8))
	}
}

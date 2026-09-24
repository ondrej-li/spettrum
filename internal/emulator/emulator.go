// Package emulator provides the Spettrum ZX Spectrum emulator integration layer.
// It ties together the Z80 CPU, ULA renderer, keyboard, beeper, TAP loader,
// and snapshot loader into a runnable emulator.
package emulator

import (
	"errors"
	"fmt"
	"io"
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

	// Simulated key timing, expressed in 50Hz frames.
	simKeyStartFrames = 150 // ~3s before the first simulated key
	simKeyGapFrames   = 25  // ~500ms between simulated keys
)

// frameDuration is how long one emulated frame lasts in real time. It is
// derived from the CPU clock rather than assumed, because the main loop is paced
// with it while the audio is generated from the cycles a frame contains: the two
// have to come from the same clock or the sound card slowly starves.
const frameDuration = time.Duration(z80.FrameCycles) * time.Second / time.Duration(z80.ClockFreq)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Config holds emulator configuration.
type Config struct {
	ROMFile      string
	SnapshotFile string
	TAPFile      string
	Instructions int // 0 = unlimited
	DisasmFile   string
	RenderMode   ula.RenderMode
	SimKey       string
	Audio        bool
	Volume       int // 0-100
	// AudioFile, when set, captures the emulated audio to a 16-bit stereo WAV
	// file. Unlike the live output this also works on headless and unpaced runs.
	AudioFile string
	// AudioRate is the audio sample rate in Hz; 0 means beeper.DefaultSampleRate.
	AudioRate int
	// AudioLatency is how much audio the output device buffers; 0 means
	// beeper.DefaultLatency.
	AudioLatency time.Duration
	QuickLoad    bool

	// Headless runs the CPU only: no terminal, no rendering, no pacing.
	Headless bool
	// NoTerminal renders frames without putting the terminal into raw mode or
	// switching to the alternate screen. Used when output is piped, redirected
	// or captured by a test.
	NoTerminal bool
	// Unpaced skips the 50Hz frame pacing so the emulator runs as fast as the
	// host allows. Intended for tests and fast-forwarding.
	Unpaced bool
	// Output receives rendered frames. A nil Output means os.Stdout.
	Output io.Writer
}

// ---------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------

// Emulator ties all modules together.
type Emulator struct {
	cfg     Config
	mem     [TotalMem]uint8
	running atomic.Bool
	out     io.Writer
	// detectSize is set when Output is the terminal, in which case the frame
	// geometry is fitted to the window before every frame.
	detectSize bool

	cpu     *z80.CPU
	display *ula.State
	kbd     *keyboard.State
	beeper  *beeper.Player
	// audioDevice is the sound card, when one was opened, so the run can report
	// audio it had to drop.
	audioDevice *beeper.DeviceSink

	tapPlayer *tap.Player

	// Disassembly
	disasmFile *os.File

	// Timing
	frameCycleCount uint64
	frameCount      uint64
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
	simKeys         string
	simKeyIdx       int
	simKeyNextFrame uint64

	// sizeWarning is reported after the terminal has been restored: while the
	// emulator owns the alternate screen anything written there is painted over
	// by the first frame and gone when the screen is switched back.
	sizeWarning string
}

// New creates a new emulator with the given configuration.
func New(cfg Config) *Emulator {
	out := cfg.Output
	detectSize := false
	if out == nil {
		out = os.Stdout
		detectSize = true
	}
	e := &Emulator{
		cfg:        cfg,
		simKeys:    cfg.SimKey,
		out:        out,
		detectSize: detectSize,
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

	// Create CPU
	e.cpu = z80.NewCPU(e, e)

	// Load snapshot state (if provided). This must happen after the CPU
	// exists, since it restores registers as well as memory.
	if e.cfg.SnapshotFile != "" {
		cpuState, _, err := snapshot.Load(e.cfg.SnapshotFile, e.mem[:])
		if err != nil {
			return fmt.Errorf("load snapshot: %w", err)
		}
		if cpuState != nil {
			e.applyCPUState(cpuState)
		}
	}

	// Port 0xFE is deliberately left to ReadIO/WriteIO rather than being served by a
	// registered port handler: ReadIO combines the keyboard matrix with the tape EAR
	// bit, so a handler for it would silently disable tape input. Registering the
	// other ULA addresses was worse still, since handlers are keyed on the low byte
	// of the port and would have answered the keyboard on ports like 0xFDFE.

	// Create ULA
	mode := e.cfg.RenderMode
	if mode == 0 {
		mode = ula.RenderOCR
	}
	e.display = ula.New(e.mem[VRAMStart:VRAMStart+ula.TotalVRAM], mode)

	// Create keyboard
	e.kbd = keyboard.New()

	// Create beeper. Audio is an optional extra: a machine with no working sound
	// card still runs the emulator, so failing to open the output device is
	// reported and the run continues in silence.
	sink, device, err := openAudioSink(e.cfg)
	if err != nil {
		return fmt.Errorf("audio: %w", err)
	}
	e.audioDevice = device
	if sink != nil {
		e.beeper = beeper.NewPlayer(beeper.Config{
			ClockFreq:  z80.ClockFreq,
			SampleRate: audioSampleRate(e.cfg),
			Volume:     float64(e.cfg.Volume) / 100.0,
			Sink:       sink,
		})
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
	defer signal.Stop(sigCh)

	e.running.Store(true)
	e.frameCount = 0
	e.simKeyNextFrame = simKeyStartFrames

	// Deferred before TermInit so that it runs after the terminal has been
	// restored: until then stdout is on the alternate screen and anything
	// written to it is thrown away when the emulator exits.
	defer func() {
		if e.sizeWarning != "" {
			fmt.Fprint(os.Stderr, e.sizeWarning)
		}
		if e.warnPCinVRAM > 0 {
			fmt.Fprintf(os.Stderr, "\nWARNING: PC in VRAM %d times\n", e.warnPCinVRAM)
		}
		if e.warnSPinVRAM > 0 {
			fmt.Fprintf(os.Stderr, "WARNING: SP in VRAM %d times\n", e.warnSPinVRAM)
		}
		if d := e.audioDevice; d != nil {
			if n := d.Dropped(); n > 0 {
				fmt.Fprintf(os.Stderr, "WARNING: %d bytes of audio were dropped; the sound card could not keep up\n", n)
			}
		}
		fmt.Printf("Total instructions: %d\n", e.totalInst)
	}()

	if !e.cfg.Headless && !e.cfg.NoTerminal {
		// Initialize terminal
		origTerm, err := ula.TermInit()
		if err != nil {
			return fmt.Errorf("term init: %w", err)
		}
		defer ula.TermCleanup(origTerm)

		// Host keyboard input is read on its own goroutine: the CPU must never
		// block on stdin (see keyboard.State.StartInput).
		e.kbd.StartInput()

		// The window geometry has to be known before the first frame is drawn:
		// rendering at the wrong size wraps or scrolls the terminal and smears
		// successive frames together.
		e.syncTerminalSize()
		e.warnIfTerminalTooSmall()
	}

	// Main loop. The CPU runs exactly one frame of T-states, then the display is
	// rendered and the pair is paced to 50Hz. Rendering happens between frames
	// while the CPU is idle, so the renderer can never observe VRAM mid-update
	// and no locking is needed on the emulation path.
	for e.running.Load() {
		frameStart := time.Now()

		for e.frameCycleCount = 0; e.frameCycleCount < z80.FrameCycles; {
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

			// Execute one instruction
			pcBefore := e.cpu.Regs.PC
			opcode := e.mem[pcBefore]
			cycles := e.cpu.Step()

			// Record history
			e.lastPC[e.histIdx%10] = pcBefore
			e.lastOpcode[e.histIdx%10] = opcode
			e.histIdx++
			e.totalInst++
			e.frameCycleCount += uint64(cycles)

			// Check for anomalies
			if pcBefore >= VRAMStart && pcBefore < VRAMStart+ula.TotalVRAM {
				e.warnPCinVRAM++
			}
			if e.cpu.Regs.SP >= VRAMStart && e.cpu.Regs.SP < VRAMStart+ula.TotalVRAM {
				e.warnSPinVRAM++
			}
		}

		if !e.running.Load() {
			break
		}

		// End of frame: raise the 50Hz interrupt, advance the emulated clock the
		// keyboard timers run on, and service simulated key presses.
		e.frameCount++
		e.cpu.GenInt(0xFF)
		e.kbd.Tick(e.cpu.Cycles)
		e.simulateKeys()
		if e.kbd.QuitRequested() {
			e.running.Store(false)
			break
		}

		// Turn this frame's speaker writes into samples now, so they are queued
		// while the emulator sleeps rather than after it wakes up.
		if e.beeper != nil {
			if err := e.beeper.EndFrame(e.cpu.Cycles); err != nil {
				return fmt.Errorf("audio: %w", err)
			}
		}

		if e.cfg.Headless {
			continue
		}

		// Keep frames fitted to the window; this also picks up resizes.
		e.syncTerminalSize()
		if _, err := io.WriteString(e.out, e.display.RenderFrame()); err != nil {
			return fmt.Errorf("write frame: %w", err)
		}
		if !e.cfg.Unpaced && !e.pacedByAudio() {
			ula.WaitFrame(frameStart, frameDuration)
		}
	}

	// Flush the tail of the last frame: a run stopped by an instruction limit
	// ends part way through one, and its audio would otherwise be lost.
	if e.beeper != nil {
		if err := e.beeper.EndFrame(e.cpu.Cycles); err != nil {
			return fmt.Errorf("audio: %w", err)
		}
	}

	return nil
}

// pacedByAudio reports whether the sound card is keeping time for the main loop.
//
// While audio is playing, waiting for the device to consume the frame's samples
// is what sets the frame rate. Sleeping as well would run the emulator slower
// than the sound it is producing, and a queue that drains is one that stutters:
// a millisecond of sleep overshoot per frame is enough for the device to eat the
// whole buffer within a couple of seconds. The wall clock is the clock to fall
// back on when there is no device, or when the one there is has stopped
// consuming.
func (e *Emulator) pacedByAudio() bool {
	return e.audioDevice != nil && e.audioDevice.Paced()
}

// syncTerminalSize refreshes the frame geometry from the window the emulator is
// attached to. It is called before the first frame is drawn and again between
// frames, which is also how a resize is picked up.
func (e *Emulator) syncTerminalSize() {
	if !e.detectSize {
		return
	}
	if cols, rows, ok := ula.TerminalSize(); ok {
		e.display.SetTerminalSize(cols, rows)
	}
}

// warnIfTerminalTooSmall records a message for when the window cannot show the
// whole display, because the image is then cropped. Frames are always fitted to
// the window, since writing a larger frame makes the terminal scroll and smear
// successive frames together. The message is reported at exit rather than
// immediately: the first frame would paint over it.
func (e *Emulator) warnIfTerminalTooSmall() {
	if !e.detectSize {
		return
	}
	cols, rows, ok := ula.TerminalSize()
	if !ok {
		return
	}
	w, h := e.display.BodySize()
	if cols >= w && rows >= h {
		return
	}
	e.sizeWarning = fmt.Sprintf(
		"warning: terminal is %dx%d but the %s display is %dx%d, so the image is cropped.\n"+
			"         use --render-mode ocr (32x24) or enlarge the window.\n",
		cols, rows, e.display.RenderMode, w, h)
}

// simulateKeys injects simulated key presses if configured.
//
// It is driven by emulated frames rather than wall-clock time so that repeated
// runs behave identically regardless of how fast the host is.
func (e *Emulator) simulateKeys() {
	if e.simKeys == "" || e.simKeyIdx >= len(e.simKeys) {
		return
	}
	if e.frameCount < e.simKeyNextFrame {
		return
	}
	e.kbd.InjectKey(e.simKeys[e.simKeyIdx])
	e.simKeyIdx++
	e.simKeyNextFrame = e.frameCount + simKeyGapFrames
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
		// Tape EAR bit (bit 6): high while the tape signal is high.
		if e.tapPlayer != nil && !e.tapPlayer.IsFinished() &&
			e.tapPlayer.ReadEAR(int(e.cpu.Cycles)) != 0 {
			result |= 0x40
		} else {
			result &^= 0x40
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
			e.beeper.Update(e.cpu.Cycles, val&0x10 != 0)
		}
	}
}

// ---------------------------------------------------------------------------
// Audio output
// ---------------------------------------------------------------------------

// openAudioSink decides where the emulated audio goes. It returns the sink and,
// when a sound card was opened, the device itself so a run can report audio that
// had to be dropped.
//
// Live output is only useful while the emulator runs in real time: when frames
// are not paced the run is either a batch job or a fast-forward, and blocking on
// a sound card would slow it down for nothing. A capture file is written either
// way, which is what makes the audio checkable from a headless run.
func openAudioSink(cfg Config) (beeper.Sink, *beeper.DeviceSink, error) {
	var sinks []beeper.Sink
	if cfg.AudioFile != "" {
		w, err := beeper.NewWavSink(cfg.AudioFile, audioSampleRate(cfg))
		if err != nil {
			return nil, nil, err
		}
		sinks = append(sinks, w)
	}
	var device *beeper.DeviceSink
	if cfg.Audio && !cfg.Headless && !cfg.Unpaced {
		d, err := beeper.NewDeviceSink(audioSampleRate(cfg), cfg.AudioLatency)
		if err != nil {
			fmt.Fprintf(os.Stderr, "warning: no audio output: %v\n", err)
		} else {
			device = d
			sinks = append(sinks, d)
		}
	}
	switch len(sinks) {
	case 0:
		return nil, device, nil
	case 1:
		return sinks[0], device, nil
	default:
		return beeper.MultiSink(sinks), device, nil
	}
}

// audioSampleRate returns the configured sample rate, or the default.
func audioSampleRate(cfg Config) int {
	if cfg.AudioRate > 0 {
		return cfg.AudioRate
	}
	return beeper.DefaultSampleRate
}

// AudioSelfTest plays a short tune through the configured audio output.
//
// It exercises the whole audio path - the beeper pin, the synthesiser and the
// sound card - without needing a ROM or a working emulation, so it is the
// quickest way to check that sound works on a particular machine:
// spettrum --audio-selftest.
func AudioSelfTest(cfg Config) error {
	sink, _, err := openAudioSink(cfg)
	if err != nil {
		return err
	}
	if sink == nil {
		return errors.New("audio is disabled: pass --audio or --audio-file")
	}

	p := beeper.NewPlayer(beeper.Config{
		ClockFreq:  z80.ClockFreq,
		SampleRate: audioSampleRate(cfg),
		Volume:     float64(cfg.Volume) / 100.0,
		Sink:       sink,
	})
	defer p.Close()

	// A rising arpeggio: A4, C#5, E5.
	cycle := uint64(0)
	for _, freq := range []float64{440, 554.37, 659.26} {
		for frame := 0; frame < 15; frame++ { // ~300ms per note
			cycle = beepTone(p, cycle, freq, z80.FrameCycles)
			if err := p.EndFrame(cycle); err != nil {
				return err
			}
			// Sleep so the tone is played at the rate it was emulated at, which
			// is what makes it audible through a sound card.
			time.Sleep(frameDuration)
		}
	}
	return nil
}

// beepTone pushes the pin transitions of a square wave of the given frequency
// into the player for one frame, starting from startCycle, and returns the cycle
// the frame ends on.
func beepTone(p *beeper.Player, startCycle uint64, freq float64, cycles uint64) uint64 {
	half := float64(z80.ClockFreq) / (2 * freq)
	high := false
	end := float64(startCycle + cycles)
	for at := float64(startCycle) + half; at < end; at += half {
		high = !high
		p.Update(uint64(at), high)
	}
	return startCycle + cycles
}

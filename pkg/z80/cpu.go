// Package z80 provides a Z80 CPU emulator implementing the full instruction set,
// interrupts, and memory/I/O callbacks.
package z80

import "sync"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const (
	ClockFreq  = 3_500_000 // 3.5 MHz Z80 clock
	MaxMemory  = 65536     // 64 KB address space
	IOPorts    = 256       // Number of I/O ports
)

// Spectrum frame timing
const (
	FrameCycles   = 70908 // T-states per 50Hz frame
	IntPulseCycles = 32   // How long INT stays asserted
)

// ---------------------------------------------------------------------------
// Flag bit positions in the F register
// ---------------------------------------------------------------------------

const (
	FlagC uint8 = 1 << 0 // Carry
	FlagN uint8 = 1 << 1 // Add/Subtract
	FlagP uint8 = 1 << 2 // Parity/Overflow
	FlagX uint8 = 1 << 3 // Undocumented XF (bit 3 of result)
	FlagH uint8 = 1 << 4 // Half-carry
	FlagY uint8 = 1 << 5 // Undocumented YF (bit 5 of result)
	FlagZ uint8 = 1 << 6 // Zero
	FlagS uint8 = 1 << 7 // Sign
)

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

// Registers holds all Z80 CPU registers including the alternate set,
// index registers, and interrupt state.
type Registers struct {
	// Main register set
	A, F   uint8
	B, C   uint8
	D, E   uint8
	H, L   uint8
	IXh, IXl uint8
	IYh, IYl uint8

	// Alternate register set
	A1, F1 uint8
	B1, C1 uint8
	D1, E1 uint8
	H1, L1 uint8

	// Special registers
	PC uint16
	SP uint16
	I  uint8 // Interrupt vector base
	R  uint8 // Memory refresh counter

	// Internal WZ register (mem_ptr) — used by LD A,(nn) and block instructions
	WZ uint16

	// Interrupt state
	IFF1 bool    // Interrupt flip-flop 1 (actual enable)
	IFF2 bool    // Interrupt flip-flop 2 (temporary save during NMI)
	IM   uint8   // Interrupt mode (0, 1, or 2)
	IFFDelay int  // EI delay counter (EI takes effect after next instruction)

	// Halted flag
	Halted bool
}

// AF returns the 16-bit AF register pair value.
func (r *Registers) AF() uint16  { return (uint16(r.A) << 8) | uint16(r.F) }
// BC returns the 16-bit BC register pair value.
func (r *Registers) BC() uint16  { return (uint16(r.B) << 8) | uint16(r.C) }
// DE returns the 16-bit DE register pair value.
func (r *Registers) DE() uint16  { return (uint16(r.D) << 8) | uint16(r.E) }
// HL returns the 16-bit HL register pair value.
func (r *Registers) HL() uint16  { return (uint16(r.H) << 8) | uint16(r.L) }
// IX returns the 16-bit IX index register value.
func (r *Registers) IX() uint16  { return (uint16(r.IXh) << 8) | uint16(r.IXl) }
// IY returns the 16-bit IY index register value.
func (r *Registers) IY() uint16  { return (uint16(r.IYh) << 8) | uint16(r.IYl) }

// SetAF sets the 16-bit AF register pair.
func (r *Registers) SetAF(v uint16) { r.A = uint8(v >> 8); r.F = uint8(v & 0xFF) }
// SetBC sets the 16-bit BC register pair.
func (r *Registers) SetBC(v uint16) { r.B = uint8(v >> 8); r.C = uint8(v & 0xFF) }
// SetDE sets the 16-bit DE register pair.
func (r *Registers) SetDE(v uint16) { r.D = uint8(v >> 8); r.E = uint8(v & 0xFF) }
// SetHL sets the 16-bit HL register pair.
func (r *Registers) SetHL(v uint16) { r.H = uint8(v >> 8); r.L = uint8(v & 0xFF) }
// SetIX sets the 16-bit IX index register.
func (r *Registers) SetIX(v uint16) { r.IXh = uint8(v >> 8); r.IXl = uint8(v & 0xFF) }
// SetIY sets the 16-bit IY index register.
func (r *Registers) SetIY(v uint16) { r.IYh = uint8(v >> 8); r.IYl = uint8(v & 0xFF) }

// SetAF1 sets the alternate 16-bit AF register pair.
func (r *Registers) AF1Val() uint16 { return (uint16(r.A1) << 8) | uint16(r.F1) }
// SetAF1 sets the alternate AF pair.
func (r *Registers) SetAF1(v uint16) { r.A1 = uint8(v >> 8); r.F1 = uint8(v & 0xFF) }
// BC1 returns the alternate BC pair.
func (r *Registers) BC1Val() uint16  { return (uint16(r.B1) << 8) | uint16(r.C1) }
// SetBC1 sets the alternate BC pair.
func (r *Registers) SetBC1(v uint16)  { r.B1 = uint8(v >> 8); r.C1 = uint8(v & 0xFF) }
// DE1 returns the alternate DE pair.
func (r *Registers) DE1Val() uint16  { return (uint16(r.D1) << 8) | uint16(r.E1) }
// SetDE1 sets the alternate DE pair.
func (r *Registers) SetDE1(v uint16)  { r.D1 = uint8(v >> 8); r.E1 = uint8(v & 0xFF) }
// HL1 returns the alternate HL pair.
func (r *Registers) HL1Val() uint16  { return (uint16(r.H1) << 8) | uint16(r.L1) }
// SetHL1 sets the alternate HL pair.
func (r *Registers) SetHL1(v uint16)  { r.H1 = uint8(v >> 8); r.L1 = uint8(v & 0xFF) }

// ---------------------------------------------------------------------------
// Memory and I/O interfaces
// ---------------------------------------------------------------------------

// MemoryHandler provides read/write access to the address space.
type MemoryHandler interface {
	ReadMemory(addr uint16) uint8
	WriteMemory(addr uint16, val uint8)
}

// IOHandler provides read/write access to I/O ports.
type IOHandler interface {
	ReadIO(port uint16) uint8
	WriteIO(port uint16, val uint8)
}

// PortHandler is a per-port read/write callback.
type PortHandler struct {
	Read  func(port uint16) uint8
	Write func(port uint16, val uint8)
}

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------

// CPU represents a Z80 processor instance.
type CPU struct {
	Regs Registers

	// Memory and I/O
	Mem  MemoryHandler
	IO   IOHandler

	// Per-port I/O handlers (for port-specific keyboard/tape handling)
	PortHandlers [IOPorts]PortHandler

	// Cycle counter
	Cycles uint64

	// Interrupt pending
	IntPending bool
	NmiPending bool
	IntData    uint8

	// Mutex for thread safety
	mu sync.Mutex
}

// NewCPU creates a new Z80 CPU with default register values.
func NewCPU(mem MemoryHandler, io IOHandler) *CPU {
	cpu := &CPU{
		Mem: mem,
		IO:  io,
	}
	cpu.Reset()
	return cpu
}

// Reset initializes the CPU to its power-on state.
func (c *CPU) Reset() {
	c.Regs = Registers{
		PC: 0x0000,
		SP: 0xFFFF,
		I:  0x00,
		R:  0x00,
	}
	c.Cycles = 0
	c.IntPending = false
	c.NmiPending = false
	c.IntData = 0
}

// RegisterPortHandler registers a handler for a specific I/O port.
func (c *CPU) RegisterPortHandler(port uint16, read func(uint16) uint8, write func(uint16, uint8)) {
	c.PortHandlers[port&0xFF].Read = read
	c.PortHandlers[port&0xFF].Write = write
}

// GenInt generates a maskable interrupt with the given data byte.
func (c *CPU) GenInt(data uint8) {
	if c.Regs.IFF1 && !c.Regs.IFFDelayActive() {
		c.IntPending = true
		c.IntData = data
	}
}

// GenNMI generates a non-maskable interrupt.
func (c *CPU) GenNMI() {
	c.NmiPending = true
}

// IFFDelayActive returns true if the EI delay is still active
// (IFF1 was just enabled and hasn't taken effect yet).
func (r *Registers) IFFDelayActive() bool {
	return r.IFFDelay > 0
}

// ---------------------------------------------------------------------------
// Flag helpers
// ---------------------------------------------------------------------------

// Step executes one instruction. Returns the number of T-states consumed.
// Full implementation in instructions.go.


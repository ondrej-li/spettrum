package z80

import "math/bits"

// ==========================================================================
// CPU — memory/IO helpers (inline-style)
// ==========================================================================

func (c *CPU) rb(addr uint16) uint8 { return c.Mem.ReadMemory(addr) }
func (c *CPU) wb(addr uint16, val uint8) { c.Mem.WriteMemory(addr, val) }

func (c *CPU) rw(addr uint16) uint16 {
	return (uint16(c.Mem.ReadMemory(addr+1)) << 8) | uint16(c.Mem.ReadMemory(addr))
}

func (c *CPU) ww(addr, val uint16) {
	c.Mem.WriteMemory(addr, uint8(val&0xFF))
	c.Mem.WriteMemory(addr+1, uint8(val>>8))
}

func (c *CPU) pushw(val uint16) {
	c.Regs.SP -= 2
	c.ww(c.Regs.SP, val)
}

func (c *CPU) popw() uint16 {
	c.Regs.SP += 2
	return c.rw(c.Regs.SP - 2)
}

func (c *CPU) nextb() uint8 {
	b := c.rb(c.Regs.PC)
	c.Regs.PC++
	return b
}

func (c *CPU) nextw() uint16 {
	c.Regs.PC += 2
	return c.rw(c.Regs.PC - 2)
}

// ==========================================================================
// Flag helpers
// ==========================================================================

// parity returns true if the number of set bits in val is even.
func parityByte(val uint8) bool { return bits.OnesCount8(val)&1 == 0 }

// getF packs the boolean flag fields into an 8-bit F register value.
func (r *Registers) getF() uint8 {
	var f uint8
	if r.F&FlagC != 0 { f |= FlagC }
	if r.F&FlagN != 0 { f |= FlagN }
	if r.F&FlagP != 0 { f |= FlagP }
	if r.F&FlagX != 0 { f |= FlagX }
	if r.F&FlagH != 0 { f |= FlagH }
	if r.F&FlagY != 0 { f |= FlagY }
	if r.F&FlagZ != 0 { f |= FlagZ }
	if r.F&FlagS != 0 { f |= FlagS }
	return f
}

// setF unpacks an 8-bit F register value into boolean flag fields.
func (r *Registers) setF(val uint8) {
	r.F = 0
	if val&FlagC != 0 { r.F |= FlagC }
	if val&FlagN != 0 { r.F |= FlagN }
	if val&FlagP != 0 { r.F |= FlagP }
	if val&FlagX != 0 { r.F |= FlagX }
	if val&FlagH != 0 { r.F |= FlagH }
	if val&FlagY != 0 { r.F |= FlagY }
	if val&FlagZ != 0 { r.F |= FlagZ }
	if val&FlagS != 0 { r.F |= FlagS }
}

// incR increments the R register, keeping bit 7 intact.
func (r *Registers) incR() {
	r.R = (r.R & 0x80) | ((r.R + 1) & 0x7F)
}

// sz53 sets bits 3,5,6,7 of F from val (X, Y, Z, S flags).
func sz53(r *Registers, val uint8) {
	r.F &^= FlagS | FlagY | FlagX | FlagZ
	r.F |= (val & (FlagS | FlagY | FlagX)) | (FlagZ & -(val & FlagZ))
	// Simpler:
	r.F = (r.F & ^uint8(FlagS|FlagY|FlagX|FlagZ)) | (val & (FlagS | FlagY | FlagX | FlagZ))
}

// ==========================================================================
// Arithmetic helpers
// ==========================================================================

// carry returns true if there's a carry from bit position bitNo when
// adding a + b + cy.
func carry(bitNo int, a, b uint16, cy bool) bool {
	result := uint32(a) + uint32(b)
	if cy { result++ }
	carryBits := result ^ uint32(a) ^ uint32(b)
	return carryBits&(1<<bitNo) != 0
}

// addb adds two 8-bit values (with carry) and sets flags. Returns result.
func addb(r *Registers, a, b uint8, cy bool) uint8 {
	result16 := uint16(a) + uint16(b)
	if cy { result16++ }
	result := uint8(result16)

	r.F = 0
	if result&0x80 != 0 { r.F |= FlagS }
	if result == 0 { r.F |= FlagZ }
	r.F |= (result & (FlagY | FlagX)) // undocumented XF/YF from result bits 3/5
	if carry(4, uint16(a), uint16(b), cy) { r.F |= FlagH } // half-carry: bit 3→4
	if carry(7, uint16(a), uint16(b), cy) != carry(8, uint16(a), uint16(b), cy) { r.F |= FlagP }
	if carry(8, uint16(a), uint16(b), cy) { r.F |= FlagC }

	return result
}

// subb subtracts b from a (with carry) and sets flags.
func subb(r *Registers, a, b uint8, cy bool) uint8 {
	val := addb(r, a, ^b, !cy)
	// Invert carry and half-carry for subtraction
	if r.F&FlagC != 0 { r.F &^= FlagC } else { r.F |= FlagC }
	if r.F&FlagH != 0 { r.F &^= FlagH } else { r.F |= FlagH }
	r.F |= FlagN
	return val
}

// addw adds two 16-bit values (with carry). Returns result, sets carry/half-carry from MSB addition.
func addw(r *Registers, a, b uint16, cy bool) uint16 {
	lsb := addb(r, uint8(a&0xFF), uint8(b&0xFF), cy)
	hadCarry := r.F&FlagC != 0
	msb := addb(r, uint8(a>>8), uint8(b>>8), hadCarry)

	result := (uint16(msb) << 8) | uint16(lsb)
	if result == 0 { r.F |= FlagZ } else { r.F &^= FlagZ }
	return result
}

// subw subtracts b from a (16-bit, with carry). Returns result.
func subw(r *Registers, a, b uint16, cy bool) uint16 {
	lsb := subb(r, uint8(a&0xFF), uint8(b&0xFF), cy)
	hadCarry := r.F&FlagC != 0
	msb := subb(r, uint8(a>>8), uint8(b>>8), hadCarry)

	result := (uint16(msb) << 8) | uint16(lsb)
	if result == 0 { r.F |= FlagZ } else { r.F &^= FlagZ }
	return result
}

// addhl adds val to HL, keeping S/Z/P flags from before the addition.
func (c *CPU) addhl(val uint16) {
	r := &c.Regs
	sf := r.F & FlagS
	zf := r.F & FlagZ
	pf := r.F & FlagP
	hl := r.HL()
	result := addw(r, hl, val, false)
	r.SetHL(result)
	r.F = (r.F &^ (FlagS | FlagZ | FlagP)) | sf | zf | pf
}

// addiz adds val to *reg (IX or IY), keeping S/Z/P flags.
func (c *CPU) addiz(reg *uint16, val uint16) {
	r := &c.Regs
	sf := r.F & FlagS
	zf := r.F & FlagZ
	pf := r.F & FlagP
	result := addw(r, *reg, val, false)
	*reg = result
	r.F = (r.F &^ (FlagS | FlagZ | FlagP)) | sf | zf | pf
}

// addizFromBytes is like addiz but takes the high/low bytes of IZ separately,
// for use in the DDFD handler where we don't have a single uint16 pointer.
func addizFromBytes(r *Registers, h, l *uint8, val uint16) {
	regVal := (uint16(*h) << 8) | uint16(*l)
	sf := r.F & FlagS
	zf := r.F & FlagZ
	pf := r.F & FlagP
	result := addw(r, regVal, val, false)
	*h = uint8(result >> 8)
	*l = uint8(result & 0xFF)
	r.F = (r.F &^ (FlagS | FlagZ | FlagP)) | sf | zf | pf
}

// adchl adds val+carry to HL.
func (c *CPU) adchl(val uint16) {
	r := &c.Regs
	result := addw(r, r.HL(), val, r.F&FlagC != 0)
	r.SetHL(result)
	if result&0x8000 != 0 { r.F |= FlagS } else { r.F &^= FlagS }
	if result == 0 { r.F |= FlagZ } else { r.F &^= FlagZ }
}

// sbchl subtracts val+carry from HL.
func (c *CPU) sbchl(val uint16) {
	r := &c.Regs
	result := subw(r, r.HL(), val, r.F&FlagC != 0)
	r.SetHL(result)
	if result&0x8000 != 0 { r.F |= FlagS } else { r.F &^= FlagS }
	if result == 0 { r.F |= FlagZ } else { r.F &^= FlagZ }
}

// inc8 increments an 8-bit value, preserving carry flag.
func inc8(r *Registers, a uint8) uint8 {
	cf := r.F & FlagC
	result := addb(r, a, 1, false)
	r.F = (r.F &^ FlagC) | cf
	return result
}

// dec8 decrements an 8-bit value, preserving carry flag.
func dec8(r *Registers, a uint8) uint8 {
	cf := r.F & FlagC
	result := subb(r, a, 1, false)
	r.F = (r.F &^ FlagC) | cf
	return result
}

// ==========================================================================
// Logic helpers
// ==========================================================================

func land(r *Registers, val uint8) {
	result := r.A & val
	r.F = 0
	if result&0x80 != 0 { r.F |= FlagS }
	if result == 0 { r.F |= FlagZ }
	r.F |= FlagH
	if parityByte(result) { r.F |= FlagP }
	r.F |= result & (FlagY | FlagX)
	r.A = result
}

func lxor(r *Registers, val uint8) {
	result := r.A ^ val
	r.F = 0
	if result&0x80 != 0 { r.F |= FlagS }
	if result == 0 { r.F |= FlagZ }
	if parityByte(result) { r.F |= FlagP }
	r.F |= result & (FlagY | FlagX)
	r.A = result
}

func lor(r *Registers, val uint8) {
	result := r.A | val
	r.F = 0
	if result&0x80 != 0 { r.F |= FlagS }
	if result == 0 { r.F |= FlagZ }
	if parityByte(result) { r.F |= FlagP }
	r.F |= result & (FlagY | FlagX)
	r.A = result
}

// cp compares val with A (same as sub but doesn't modify A).
func cp(r *Registers, val uint8) {
	subb(r, r.A, val, false)
	// XF/YF from the operand, not the result
	r.F &^= FlagY | FlagX
	r.F |= val & (FlagY | FlagX)
}

// ==========================================================================
// Rotate & shift helpers (CB-prefix)
// ==========================================================================

func cb_rlc(r *Registers, val uint8) uint8 {
	old := val >> 7
	val = (val << 1) | old
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_rrc(r *Registers, val uint8) uint8 {
	old := val & 1
	val = (val >> 1) | (old << 7)
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_rl(r *Registers, val uint8) uint8 {
	cf := r.F & FlagC
	old := val >> 7
	val = (val << 1)
	if cf != 0 { val |= 1 }
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_rr(r *Registers, val uint8) uint8 {
	cf := r.F & FlagC
	old := val & 1
	val = (val >> 1)
	if cf != 0 { val |= 0x80 }
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_sla(r *Registers, val uint8) uint8 {
	old := val >> 7
	val <<= 1
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_sll(r *Registers, val uint8) uint8 {
	old := val >> 7
	val = (val << 1) | 1
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_sra(r *Registers, val uint8) uint8 {
	old := val & 1
	val = (val >> 1) | (val & 0x80)
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

func cb_srl(r *Registers, val uint8) uint8 {
	old := val & 1
	val >>= 1
	r.F = 0
	if val&0x80 != 0 { r.F |= FlagS }
	if val == 0 { r.F |= FlagZ }
	if parityByte(val) { r.F |= FlagP }
	if old != 0 { r.F |= FlagC }
	r.F |= val & (FlagY | FlagX)
	return val
}

// cb_bit tests bit n of val, sets flags. Returns val unchanged for BIT but
// the result is used by the DDCB/FDCB path.
func cb_bit(r *Registers, val, n uint8) {
	result := val & (1 << n)
	r.F &^= FlagS | FlagZ | FlagY | FlagH | FlagX | FlagP | FlagN
	if n == 7 && result != 0 { r.F |= FlagS }
	if result == 0 { r.F |= FlagZ | FlagP } else { r.F &^= FlagP }
	r.F |= FlagH
	r.F |= val & (FlagY | FlagX)
}

// ==========================================================================
// Block instruction helpers
// ==========================================================================

func (c *CPU) ldi() {
	r := &c.Regs
	de := r.DE()
	hl := r.HL()
	val := c.rb(hl)
	c.wb(de, val)

	r.SetHL(hl + 1)
	r.SetDE(de + 1)
	r.SetBC(r.BC() - 1)

	// XF/YF from (A + copied byte) bits 3 and 1
	result := val + r.A
	r.F &^= FlagN | FlagH | FlagP | FlagY | FlagX
	r.F |= result & FlagX
	if result&0x02 != 0 { r.F |= FlagY }
	if r.BC() != 0 { r.F |= FlagP }
}

func (c *CPU) ldd() {
	c.ldi()
	r := &c.Regs
	r.SetHL(r.HL() - 2)
	r.SetDE(r.DE() - 2)
}

func (c *CPU) cpi() {
	r := &c.Regs
	cf := r.F & FlagC
	result := subb(r, r.A, c.rb(r.HL()), false)
	r.SetHL(r.HL() + 1)
	r.SetBC(r.BC() - 1)
	// XF/YF from (result - half_carry)
	adj := result
	if r.F&FlagH != 0 { adj-- }
	r.F &^= FlagY | FlagX
	r.F |= adj & (FlagY | FlagX)
	if r.BC() != 0 { r.F |= FlagP } else { r.F &^= FlagP }
	r.F = (r.F &^ FlagC) | cf
	r.WZ++
}

func (c *CPU) cpd() {
	c.cpi()
	r := &c.Regs
	r.SetHL(r.HL() - 2)
	r.WZ -= 2
}

func (c *CPU) ini() {
	r := &c.Regs
	port := (uint16(r.B) << 8) | uint16(r.C)
	val := c.readIOPort(port)
	c.wb(r.HL(), val)
	r.SetHL(r.HL() + 1)
	r.B--
	r.F &^= FlagZ | FlagN
	if r.B == 0 { r.F |= FlagZ }
	r.F |= FlagN
	r.WZ = r.BC() + 1
}

func (c *CPU) ind() {
	c.ini()
	r := &c.Regs
	r.SetHL(r.HL() - 2)
	r.WZ = r.BC() - 2
}

func (c *CPU) outi() {
	r := &c.Regs
	port := (uint16(r.B) << 8) | uint16(r.C)
	c.writeIOPort(port, c.rb(r.HL()))
	r.SetHL(r.HL() + 1)
	r.B--
	r.F &^= FlagZ | FlagN
	if r.B == 0 { r.F |= FlagZ }
	r.F |= FlagN
	r.WZ = r.BC() + 1
}

func (c *CPU) outd() {
	c.outi()
	r := &c.Regs
	r.SetHL(r.HL() - 2)
	r.WZ = r.BC() - 2
}

// ==========================================================================
// I/O port access
// ==========================================================================

func (c *CPU) readIOPort(port uint16) uint8 {
	pl := port & 0xFF
	if c.PortHandlers[pl].Read != nil {
		return c.PortHandlers[pl].Read(port)
	}
	if c.IO != nil {
		return c.IO.ReadIO(port)
	}
	return 0xFF
}

func (c *CPU) writeIOPort(port uint16, val uint8) {
	pl := port & 0xFF
	if c.PortHandlers[pl].Write != nil {
		c.PortHandlers[pl].Write(port, val)
		return
	}
	if c.IO != nil {
		c.IO.WriteIO(port, val)
	}
}

// ==========================================================================
// DAA — decimal adjust accumulator
// ==========================================================================

func daa(r *Registers) {
	var correction uint8
	if (r.A&0x0F) > 0x09 || r.F&FlagH != 0 {
		correction += 0x06
	}
	if r.A > 0x99 || r.F&FlagC != 0 {
		correction += 0x60
		r.F |= FlagC
	} else {
		r.F &^= FlagC
	}

	if r.F&FlagN != 0 {
		// Subtraction
		r.F &^= FlagH
		if (r.A & 0x0F) < 0x06 { r.F &^= FlagH } else { r.F |= FlagH }
		r.A -= correction
	} else {
		if (r.A&0x0F) > 0x09 { r.F |= FlagH } else { r.F &^= FlagH }
		r.A += correction
	}

	r.F &^= FlagS | FlagZ | FlagP | FlagY | FlagX
	if r.A&0x80 != 0 { r.F |= FlagS }
	if r.A == 0 { r.F |= FlagZ }
	if parityByte(r.A) { r.F |= FlagP }
	r.F |= r.A & (FlagY | FlagX)
}

// ==========================================================================
// CB-prefix handler (bit/rotate/shift operations)
// ==========================================================================

// cbFunc is a function that operates on a register value and returns the result.
type cbFunc func(r *Registers, val uint8) uint8

// cbBitFunc is a function that tests a bit (BIT n,r).
type cbBitFunc func(r *Registers, val, n uint8)

// cbRotTable maps the x field (bits 7-6 of CB opcode) to the rotate/shift function.
var cbRotTable = [8]cbFunc{
	cb_rlc, cb_rrc, cb_rl, cb_rr, cb_sla, cb_sra, cb_sll, cb_srl,
}

// setCBResult writes val to the register indicated by z (0-7).
func (c *CPU) setCBResult(z uint8, val uint8) {
	r := &c.Regs
	switch z {
	case 0: r.B = val
	case 1: r.C = val
	case 2: r.D = val
	case 3: r.E = val
	case 4: r.H = val
	case 5: r.L = val
	case 6: c.wb(r.HL(), val) // (HL)
	case 7: r.A = val
	}
}

// getCBResult reads the value from register indicated by z.
func (c *CPU) getCBResult(z uint8) uint8 {
	r := &c.Regs
	switch z {
	case 0: return r.B
	case 1: return r.C
	case 2: return r.D
	case 3: return r.E
	case 4: return r.H
	case 5: return r.L
	case 6: return c.rb(r.HL()) // (HL)
	case 7: return r.A
	}
	return 0
}

func (c *CPU) execOpcodeCB(opcode uint8) int {
	r := &c.Regs
	x := (opcode >> 6) & 3  // operation class
	y := (opcode >> 3) & 7  // bit number
	z := opcode & 7          // register

	switch x {
	case 0: // Rotate/shift
		val := c.getCBResult(z)
		val = cbRotTable[y](r, val)
		c.setCBResult(z, val)
	case 1: // BIT y, r
		val := c.getCBResult(z)
		cb_bit(r, val, y)
	case 2: // RES y, r
		val := c.getCBResult(z) & ^(uint8(1) << y)
		c.setCBResult(z, val)
	case 3: // SET y, r
		val := c.getCBResult(z) | (uint8(1) << y)
		c.setCBResult(z, val)
	}
	return 8 // CB-prefix base T-states (simplified)
}

// ==========================================================================
// DDCB/FDCB handler (displaced bit operations)
// ==========================================================================

func (c *CPU) execOpcodeDCB(opcode uint8, addr uint16) int {
	r := &c.Regs
	x := (opcode >> 6) & 3
	y := (opcode >> 3) & 7
	z := opcode & 7
	val := c.rb(addr)

	switch x {
	case 0: // Rotate/shift on (IZ+d), also write back to register z
		result := cbRotTable[y](r, val)
		c.wb(addr, result)
		if z != 6 { // Don't write back to (HL) for the displaced version? Actually, per Z80 spec, for rotate/shift with (IZ+d), the result IS written also to the register.
			c.setCBResult(z, result)
		}
	case 1: // BIT y, (IZ+d)
		cb_bit(r, val, y)
	case 2: // RES y, (IZ+d)
		val &^= 1 << y
		c.wb(addr, val)
		if z != 6 {
			c.setCBResult(z, val)
		}
	case 3: // SET y, (IZ+d)
		val |= 1 << y
		c.wb(addr, val)
		if z != 6 {
			c.setCBResult(z, val)
		}
	}
	return 23 // DDCB/FDCB T-states (simplified)
}

// ==========================================================================
// ED-prefix handler
// ==========================================================================

func (c *CPU) execOpcodeED(opcode uint8) int {
	r := &c.Regs

	switch {
	// --- Block transfers ---
	case opcode == 0xA0: c.ldi(); return 16
	case opcode == 0xB0: // LDIR
		c.ldi()
		if r.BC() != 0 { r.PC -= 2; return 21 }
		return 16
	case opcode == 0xA8: c.ldd(); return 16
	case opcode == 0xB8:
		c.ldd()
		if r.BC() != 0 { r.PC -= 2; return 21 }
		return 16

	// --- Block compare ---
	case opcode == 0xA1: c.cpi(); return 16
	case opcode == 0xB1:
		c.cpi()
		if r.BC() != 0 && r.F&FlagZ == 0 { r.PC -= 2; return 21 }
		return 16
	case opcode == 0xA9: c.cpd(); return 16
	case opcode == 0xB9:
		c.cpd()
		if r.BC() != 0 && r.F&FlagZ == 0 { r.PC -= 2; return 21 }
		return 16

	// --- Block I/O ---
	case opcode == 0xA2: c.ini(); return 16
	case opcode == 0xB2:
		c.ini()
		if r.B != 0 { r.PC -= 2; return 21 }
		return 16
	case opcode == 0xAA: c.ind(); return 16
	case opcode == 0xBA:
		c.ind()
		if r.B != 0 { r.PC -= 2; return 21 }
		return 16
	case opcode == 0xA3: c.outi(); return 16
	case opcode == 0xB3:
		c.outi()
		if r.B != 0 { r.PC -= 2; return 21 }
		return 16
	case opcode == 0xAB: c.outd(); return 16
	case opcode == 0xBB:
		c.outd()
		if r.B != 0 { r.PC -= 2; return 21 }
		return 16

	// --- 16-bit arithmetic ---
	case opcode == 0x4A: c.adchl(r.BC()); return 15
	case opcode == 0x5A: c.adchl(r.DE()); return 15
	case opcode == 0x6A: c.adchl(r.HL()); return 15
	case opcode == 0x7A: c.adchl(r.SP); return 15
	case opcode == 0x42: c.sbchl(r.BC()); return 15
	case opcode == 0x52: c.sbchl(r.DE()); return 15
	case opcode == 0x62: c.sbchl(r.HL()); return 15
	case opcode == 0x72: c.sbchl(r.SP); return 15

	// --- LD (nn), dd / LD dd, (nn) ---
	case opcode == 0x43: addr := c.nextw(); c.ww(addr, r.BC()); r.WZ = addr + 1; return 20
	case opcode == 0x53: addr := c.nextw(); c.ww(addr, r.DE()); r.WZ = addr + 1; return 20
	case opcode == 0x63: addr := c.nextw(); c.ww(addr, r.HL()); r.WZ = addr + 1; return 20
	case opcode == 0x73: addr := c.nextw(); c.ww(addr, r.SP); r.WZ = addr + 1; return 20
	case opcode == 0x4B: addr := c.nextw(); r.SetBC(c.rw(addr)); r.WZ = addr + 1; return 20
	case opcode == 0x5B: addr := c.nextw(); r.SetDE(c.rw(addr)); r.WZ = addr + 1; return 20
	case opcode == 0x6B: addr := c.nextw(); r.SetHL(c.rw(addr)); r.WZ = addr + 1; return 20
	case opcode == 0x7B: addr := c.nextw(); r.SP = c.rw(addr); r.WZ = addr + 1; return 20

	// --- Specific opcodes (MUST be before range checks below) ---
	// RETN / RETI
	case opcode == 0x45, opcode == 0x4D:
		r.PC = c.popw(); r.WZ = r.PC
		r.IFF1 = r.IFF2; r.IFFDelay = 0; return 14

	// IM 0/1/2
	case opcode == 0x46: r.IM = 0; return 8
	case opcode == 0x56: r.IM = 1; return 8
	case opcode == 0x5E: r.IM = 2; return 8

	// LD I,A / LD A,I / LD R,A / LD A,R
	case opcode == 0x47: r.I = r.A; return 9
	case opcode == 0x57: // LD A,I
		r.A = r.I
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if r.A&0x80 != 0 { r.F |= FlagS }
		if r.A == 0 { r.F |= FlagZ }
		if r.IFF2 { r.F |= FlagP }
		r.F |= r.A & (FlagY | FlagX)
		return 9
	case opcode == 0x4F: r.R = r.A; return 9
	case opcode == 0x5F: // LD A,R
		r.A = r.R
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if r.A&0x80 != 0 { r.F |= FlagS }
		if r.A == 0 { r.F |= FlagZ }
		if r.IFF2 { r.F |= FlagP }
		r.F |= r.A & (FlagY | FlagX)
		return 9

	// RRD / RLD
	case opcode == 0x67: // RRD
		hl := r.HL(); val := c.rb(hl); oldA := r.A
		r.A = (oldA & 0xF0) | (val & 0x0F)
		val = (val >> 4) | ((oldA & 0x0F) << 4)
		c.wb(hl, val)
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if r.A&0x80 != 0 { r.F |= FlagS }
		if r.A == 0 { r.F |= FlagZ }
		if parityByte(r.A) { r.F |= FlagP }
		r.F |= r.A & (FlagY | FlagX)
		r.WZ = hl + 1
		return 18
	case opcode == 0x6F: // RLD
		hl := r.HL(); val := c.rb(hl); oldA := r.A
		r.A = (oldA & 0xF0) | (val >> 4)
		val = (val << 4) | (oldA & 0x0F)
		c.wb(hl, val)
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if r.A&0x80 != 0 { r.F |= FlagS }
		if r.A == 0 { r.F |= FlagZ }
		if parityByte(r.A) { r.F |= FlagP }
		r.F |= r.A & (FlagY | FlagX)
		r.WZ = hl + 1
		return 18

	// NEG
	case opcode == 0x44, opcode == 0x54, opcode == 0x64, opcode == 0x74,
		opcode == 0x4C, opcode == 0x5C, opcode == 0x6C, opcode == 0x7C:
		oldA := r.A
		r.A = subb(r, 0, oldA, false)
		return 8

	// --- IN r, (C) (range: 0x40-0x78 except 0x46,0x56,0x66→reg=6 excluded) ---
	case opcode >= 0x40 && opcode <= 0x78 && (opcode&7) != 6:
		// But skip the specific opcodes already handled above
		if opcode == 0x44 || opcode == 0x54 || opcode == 0x64 || opcode == 0x74 ||
			opcode == 0x4C || opcode == 0x5C || opcode == 0x6C || opcode == 0x7C ||
			opcode == 0x45 || opcode == 0x4D || opcode == 0x46 || opcode == 0x56 ||
			opcode == 0x5E || opcode == 0x47 || opcode == 0x4F || opcode == 0x57 ||
			opcode == 0x5F || opcode == 0x67 || opcode == 0x6F {
			return 8
		}
		port := (uint16(r.B) << 8) | uint16(r.C)
		val := c.readIOPort(port)
		z := opcode & 7
		c.setCBResult(z, val)
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if val&0x80 != 0 { r.F |= FlagS }
		if val == 0 { r.F |= FlagZ }
		if parityByte(val) { r.F |= FlagP }
		r.F |= val & (FlagY | FlagX)
		return 12
	case opcode == 0x70: // IN (C) dummy
		port := (uint16(r.B) << 8) | uint16(r.C)
		val := c.readIOPort(port)
		r.F &^= FlagS | FlagZ | FlagY | FlagX | FlagH | FlagP | FlagN
		if val&0x80 != 0 { r.F |= FlagS }
		if val == 0 { r.F |= FlagZ }
		if parityByte(val) { r.F |= FlagP }
		r.F |= val & (FlagY | FlagX)
		return 12

	// --- OUT (C), r (range: 0x41-0x79 except 0x71=OUT (C),0) ---
	case opcode >= 0x41 && opcode <= 0x79 && (opcode&7) != 6 && opcode != 0x71:
		port := (uint16(r.B) << 8) | uint16(r.C)
		c.writeIOPort(port, c.getCBResult(opcode&7))
		return 12
	case opcode == 0x71: // OUT (C), 0
		port := (uint16(r.B) << 8) | uint16(r.C)
		c.writeIOPort(port, 0)
		return 12
	}

	// Unknown ED opcode
	return 8
}

// ==========================================================================
// DD/FD-prefix handler (index register instructions)
// ==========================================================================

func (c *CPU) execOpcodeDDFD(opcode uint8, izh, izl *uint8) int {
	r := &c.Regs

	// izAddr computes IX+d or IY+d
	izAddr := func() uint16 {
		disp := int8(c.nextb())
		return uint16(int32((uint16(*izh)<<8)|uint16(*izl)) + int32(disp))
	}

	// izVal returns the 16-bit IX or IY value
	izVal := func() uint16 { return (uint16(*izh) << 8) | uint16(*izl) }
	setIZ := func(v uint16) { *izh = uint8(v >> 8); *izl = uint8(v & 0xFF) }

	switch opcode {
	// --- Fall-through to base opcode for most instructions ---
	// DD/FD 0xCB — displaced bit operations
	case 0xCB:
		disp := int8(c.nextb())
		addr := uint16(int32(izVal()) + int32(disp))
		r.WZ = addr
		cbOp := c.nextb()
		return c.execOpcodeDCB(cbOp, addr)

	// --- 16-bit loads ---
	case 0x21: // LD IZ, nn
		setIZ(c.nextw())
		return 14
	case 0x22: // LD (nn), IZ
		addr := c.nextw()
		c.ww(addr, izVal())
		r.WZ = addr + 1
		return 20
	case 0x2A: // LD IZ, (nn)
		addr := c.nextw()
		setIZ(c.rw(addr))
		r.WZ = addr + 1
		return 20
	case 0x36: // LD (IZ+d), n
		addr := izAddr()
		c.wb(addr, c.nextb())
		return 19

	// --- Increment/Decrement ---
	case 0x23: setIZ(izVal() + 1); return 10 // INC IZ
	case 0x2B: setIZ(izVal() - 1); return 10 // DEC IZ
	case 0x34: // INC (IZ+d)
		addr := izAddr()
		val := inc8(r, c.rb(addr))
		c.wb(addr, val)
		return 23
	case 0x35: // DEC (IZ+d)
		addr := izAddr()
		val := dec8(r, c.rb(addr))
		c.wb(addr, val)
		return 23

	// --- 8-bit loads to/from (IZ+d) ---
	case 0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77:
		addr := izAddr()
		regIdx := (opcode >> 3) & 7
		isStore := (opcode & 0x07) >= 0x06

		// Load (IZ+d), r
		if isStore && opcode != 0x7E {
			// 0x70-0x77: LD (IZ+d), r
			c.wb(addr, c.getCBResult(opcode&7))
			return 19
		}

		// Load r, (IZ+d)
		// 0x46,0x4E,0x56,0x5E,0x66,0x6E: LD r, (IZ+d)
		// 0x7E: LD A, (IZ+d)
		val := c.rb(addr)
		if opcode == 0x7E {
			r.A = val
		} else {
			c.setCBResult(regIdx, val)
		}
		return 19

	// --- 8-bit arithmetic on (IZ+d) ---
	case 0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE:
		addr := izAddr()
		val := c.rb(addr)
		switch (opcode >> 3) & 7 {
		case 0: addb(r, r.A, val, false); r.A = r.A + val - val + addb(r, r.A, val, false) // handled below
		}
		// Delegate to base opcode with (HL) semantics
		return c.execOpcodeBaseWithHL(opcode, addr)

	// --- POP/PUSH IZ ---
	case 0xE1: setIZ(c.popw()); return 14 // POP IZ
	case 0xE5: c.pushw(izVal()); return 15 // PUSH IZ
	case 0xE3: // EX (SP), IZ
		spVal := c.rw(r.SP)
		c.ww(r.SP, izVal())
		setIZ(spVal)
		r.WZ = spVal
		return 23
	case 0xE9: r.PC = izVal(); return 8 // JP (IZ)
	case 0xF9: r.SP = izVal(); return 10 // LD SP, IZ

	// --- ADD IZ, ss ---
	case 0x09:
		addizFromBytes(r, izh, izl, r.BC())
		return 15
	case 0x19:
		addizFromBytes(r, izh, izl, r.DE())
		return 15
	case 0x29:
		addizFromBytes(r, izh, izl, izVal())
		return 15
	case 0x39:
		addizFromBytes(r, izh, izl, r.SP)
		return 15

	// --- LD IZh/IZl, r ---
	case 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6F:
		// DD 0x60 = LD IXh, B, DD 0x68 = LD IXl, B, etc.
		reg := opcode & 7
		isHigh := (opcode & 0x08) == 0
		val := c.getCBResult(reg)
		if isHigh { *izh = val } else { *izl = val }
		return 8
	}

	// Unknown DD/FD opcode — fall through to base opcode
	return c.execOpcodeBase(opcode)
}

// execOpcodeBaseWithHL handles base opcodes that use (HL) but need a different address.
func (c *CPU) execOpcodeBaseWithHL(opcode uint8, addr uint16) int {
	r := &c.Regs
	val := c.rb(addr)

	switch opcode {
	case 0x86: // ADD A, (HL)
		r.A = addb(r, r.A, val, false)
	case 0x8E: // ADC A, (HL)
		r.A = addb(r, r.A, val, r.F&FlagC != 0)
	case 0x96: // SUB (HL)
		r.A = subb(r, r.A, val, false)
	case 0x9E: // SBC A, (HL)
		r.A = subb(r, r.A, val, r.F&FlagC != 0)
	case 0xA6: // AND (HL)
		land(r, val)
	case 0xAE: // XOR (HL)
		lxor(r, val)
	case 0xB6: // OR (HL)
		lor(r, val)
	case 0xBE: // CP (HL)
		cp(r, val)
	}
	return 11 // simplified timing for now
}

// ==========================================================================
// Base opcode dispatch (0x00–0xFF excluding prefixes)
// ==========================================================================

func (c *CPU) execOpcodeBase(opcode uint8) int {
	r := &c.Regs

	// Opcode groups:
	// 0x00–0x3F: control, 8-bit loads, 16-bit loads, inc/dec, rotates
	// 0x40–0x7F: LD r,r' (with HALT at 0x76)
	// 0x80–0xBF: ALU ops on A
	// 0xC0–0xFF: control flow, stack, prefix escapes

	switch {
	// === 0x00–0x3F ===
	case opcode == 0x00: // NOP
		return 4
	case opcode == 0x08: // EX AF, AF'
		r.A, r.A1 = r.A1, r.A
		r.F, r.F1 = r.F1, r.F
		return 4
	case opcode == 0x10: // DJNZ e
		disp := int8(c.nextb())
		r.B--
		if r.B != 0 {
			r.PC = uint16(int32(r.PC) + int32(disp))
			r.WZ = r.PC
			return 13
		}
		return 8
	case opcode == 0x18: // JR e
		disp := int8(c.nextb())
		r.PC = uint16(int32(r.PC) + int32(disp))
		r.WZ = r.PC
		return 12
	case opcode == 0x20: // JR NZ, e
		disp := int8(c.nextb())
		if r.F&FlagZ == 0 {
			r.PC = uint16(int32(r.PC) + int32(disp))
			r.WZ = r.PC
			return 12
		}
		return 7
	case opcode == 0x28: // JR Z, e
		disp := int8(c.nextb())
		if r.F&FlagZ != 0 {
			r.PC = uint16(int32(r.PC) + int32(disp))
			r.WZ = r.PC
			return 12
		}
		return 7
	case opcode == 0x30: // JR NC, e
		disp := int8(c.nextb())
		if r.F&FlagC == 0 {
			r.PC = uint16(int32(r.PC) + int32(disp))
			r.WZ = r.PC
			return 12
		}
		return 7
	case opcode == 0x38: // JR C, e
		disp := int8(c.nextb())
		if r.F&FlagC != 0 {
			r.PC = uint16(int32(r.PC) + int32(disp))
			r.WZ = r.PC
			return 12
		}
		return 7
	case opcode == 0x27: // DAA
		daa(r)
		return 4
	case opcode == 0x2F: // CPL
		r.A = ^r.A
		r.F |= FlagH | FlagN
		r.F |= r.A & (FlagY | FlagX)
		return 4
	case opcode == 0x37: // SCF
		r.F = (r.F &^ (FlagH | FlagN | FlagY | FlagX)) | FlagC | (r.A & (FlagY | FlagX))
		return 4
	case opcode == 0x3F: // CCF
		r.F = (r.F &^ (FlagY | FlagX)) | (r.A & (FlagY | FlagX))
		if r.F&FlagC != 0 { r.F &^= FlagC } else { r.F |= FlagC }
		r.F |= r.F&FlagC << 4 // HF = old CF
		if r.F&FlagH != 0 { r.F &^= FlagN } else { r.F &^= FlagN } // actually, N = 0 always
		r.F &^= FlagN
		r.F |= FlagH // HF set
		return 4
	case opcode == 0xD9: // EXX
		r.B, r.B1 = r.B1, r.B
		r.C, r.C1 = r.C1, r.C
		r.D, r.D1 = r.D1, r.D
		r.E, r.E1 = r.E1, r.E
		r.H, r.H1 = r.H1, r.H
		r.L, r.L1 = r.L1, r.L
		return 4

	// === 0x01–0x3F: 16-bit load, inc/dec ===
	case opcode == 0x01: r.SetBC(c.nextw()); return 10
	case opcode == 0x11: r.SetDE(c.nextw()); return 10
	case opcode == 0x21: r.SetHL(c.nextw()); return 10
	case opcode == 0x31: r.SP = c.nextw(); return 10
	case opcode == 0x02: c.wb(r.BC(), r.A); r.WZ = (r.BC()&0xFF00) | ((r.BC()+1)&0xFF); return 7
	case opcode == 0x12: c.wb(r.DE(), r.A); r.WZ = (r.DE()&0xFF00) | ((r.DE()+1)&0xFF); return 7
	case opcode == 0x0A: r.A = c.rb(r.BC()); r.WZ = r.BC() + 1; return 7
	case opcode == 0x1A: r.A = c.rb(r.DE()); r.WZ = r.DE() + 1; return 7
	case opcode == 0x22: // LD (nn), HL
		addr := c.nextw()
		c.ww(addr, r.HL()); r.WZ = addr + 1; return 16
	case opcode == 0x2A: // LD HL, (nn)
		addr := c.nextw()
		r.SetHL(c.rw(addr)); r.WZ = addr + 1; return 16
	case opcode == 0x32: // LD (nn), A
		addr := c.nextw()
		c.wb(addr, r.A); r.WZ = (uint16(r.A) << 8) | ((addr + 1) & 0xFF); return 13
	case opcode == 0x3A: // LD A, (nn)
		addr := c.nextw()
		r.A = c.rb(addr); r.WZ = addr + 1; return 13
	case opcode == 0xF9: r.SP = r.HL(); return 6 // LD SP, HL

	// === Rotates on A ===
	case opcode == 0x07: r.A = cb_rlc(r, r.A); r.F &^= FlagY | FlagX; r.F |= r.A & (FlagY | FlagX); return 4 // RLCA
	case opcode == 0x0F: r.A = cb_rrc(r, r.A); r.F &^= FlagY | FlagX; r.F |= r.A & (FlagY | FlagX); return 4 // RRCA
	case opcode == 0x17: r.A = cb_rl(r, r.A); r.F &^= FlagY | FlagX; r.F |= r.A & (FlagY | FlagX); return 4  // RLA
	case opcode == 0x1F: r.A = cb_rr(r, r.A); r.F &^= FlagY | FlagX; r.F |= r.A & (FlagY | FlagX); return 4  // RRA

	// === 0x03–0x3B: 16-bit inc/dec ===
	case opcode == 0x03: r.SetBC(r.BC() + 1); return 6
	case opcode == 0x13: r.SetDE(r.DE() + 1); return 6
	case opcode == 0x23: r.SetHL(r.HL() + 1); return 6
	case opcode == 0x33: r.SP++; return 6
	case opcode == 0x0B: r.SetBC(r.BC() + 0xFFFF); return 6 // DEC BC
	case opcode == 0x1B: r.SetDE(r.DE() + 0xFFFF); return 6
	case opcode == 0x2B: r.SetHL(r.HL() + 0xFFFF); return 6
	case opcode == 0x3B: r.SP += 0xFFFF; return 6

	// === 0x04–0x3D: 8-bit inc/dec ===
	case opcode == 0x04: r.B = inc8(r, r.B); return 4
	case opcode == 0x0C: r.C = inc8(r, r.C); return 4
	case opcode == 0x14: r.D = inc8(r, r.D); return 4
	case opcode == 0x1C: r.E = inc8(r, r.E); return 4
	case opcode == 0x24: r.H = inc8(r, r.H); return 4
	case opcode == 0x2C: r.L = inc8(r, r.L); return 4
	case opcode == 0x34: // INC (HL)
		hl := r.HL()
		val := inc8(r, c.rb(hl))
		c.wb(hl, val); return 11
	case opcode == 0x3C: r.A = inc8(r, r.A); return 4
	case opcode == 0x05: r.B = dec8(r, r.B); return 4
	case opcode == 0x0D: r.C = dec8(r, r.C); return 4
	case opcode == 0x15: r.D = dec8(r, r.D); return 4
	case opcode == 0x1D: r.E = dec8(r, r.E); return 4
	case opcode == 0x25: r.H = dec8(r, r.H); return 4
	case opcode == 0x2D: r.L = dec8(r, r.L); return 4
	case opcode == 0x35: // DEC (HL)
		hl := r.HL()
		val := dec8(r, c.rb(hl))
		c.wb(hl, val); return 11
	case opcode == 0x3D: r.A = dec8(r, r.A); return 4

	// === 0x06–0x3E: 8-bit load immediate ===
	case opcode == 0x06: r.B = c.nextb(); return 7
	case opcode == 0x0E: r.C = c.nextb(); return 7
	case opcode == 0x16: r.D = c.nextb(); return 7
	case opcode == 0x1E: r.E = c.nextb(); return 7
	case opcode == 0x26: r.H = c.nextb(); return 7
	case opcode == 0x2E: r.L = c.nextb(); return 7
	case opcode == 0x36: c.wb(r.HL(), c.nextb()); return 10 // LD (HL), n
	case opcode == 0x3E: r.A = c.nextb(); return 7

	// === 0x09–0x39: ADD HL, ss ===
	case opcode == 0x09: c.addhl(r.BC()); return 11
	case opcode == 0x19: c.addhl(r.DE()); return 11
	case opcode == 0x29: c.addhl(r.HL()); return 11
	case opcode == 0x39: c.addhl(r.SP); return 11

	// === 0x40–0x7F: LD r, r' (with HALT at 0x76) ===
	case opcode == 0x76: // HALT
		r.Halted = true
		return 4
	case opcode >= 0x40 && opcode <= 0x75 || opcode >= 0x77 && opcode <= 0x7F:
		src := opcode & 7
		dst := (opcode >> 3) & 7
		val := c.getCBResult(src)
		c.setCBResult(dst, val)
		return 4

	// === 0x80–0xBF: ALU ops (ADD/ADC/SUB/SBC/AND/XOR/OR/CP) A, r ===
	case opcode >= 0x80 && opcode <= 0x87: // ADD A, r
		r.A = addb(r, r.A, c.getCBResult(opcode&7), false); return 4
	case opcode == 0xC6: r.A = addb(r, r.A, c.nextb(), false); return 7 // ADD A, n
	case opcode >= 0x88 && opcode <= 0x8F: // ADC A, r
		r.A = addb(r, r.A, c.getCBResult(opcode&7), r.F&FlagC != 0); return 4
	case opcode == 0xCE: r.A = addb(r, r.A, c.nextb(), r.F&FlagC != 0); return 7 // ADC A, n
	case opcode >= 0x90 && opcode <= 0x97: // SUB r
		r.A = subb(r, r.A, c.getCBResult(opcode&7), false); return 4
	case opcode == 0xD6: r.A = subb(r, r.A, c.nextb(), false); return 7 // SUB n
	case opcode >= 0x98 && opcode <= 0x9F: // SBC A, r
		r.A = subb(r, r.A, c.getCBResult(opcode&7), r.F&FlagC != 0); return 4
	case opcode == 0xDE: r.A = subb(r, r.A, c.nextb(), r.F&FlagC != 0); return 7 // SBC A, n
	case opcode >= 0xA0 && opcode <= 0xA7: // AND r
		land(r, c.getCBResult(opcode&7)); return 4
	case opcode == 0xE6: land(r, c.nextb()); return 7 // AND n
	case opcode >= 0xA8 && opcode <= 0xAF: // XOR r
		lxor(r, c.getCBResult(opcode&7)); return 4
	case opcode == 0xEE: lxor(r, c.nextb()); return 7 // XOR n
	case opcode >= 0xB0 && opcode <= 0xB7: // OR r
		lor(r, c.getCBResult(opcode&7)); return 4
	case opcode == 0xF6: lor(r, c.nextb()); return 7 // OR n
	case opcode >= 0xB8 && opcode <= 0xBF: // CP r
		cp(r, c.getCBResult(opcode&7)); return 4
	case opcode == 0xFE: cp(r, c.nextb()); return 7 // CP n

	// === 0xC0–0xFF: Control flow, stack, I/O ===
	case opcode == 0xC0: // RET NZ
		if r.F&FlagZ == 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xC8: // RET Z
		if r.F&FlagZ != 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xD0: // RET NC
		if r.F&FlagC == 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xD8: // RET C
		if r.F&FlagC != 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xE0: // RET PO
		if r.F&FlagP == 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xE8: // RET PE
		if r.F&FlagP != 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xF0: // RET P
		if r.F&FlagS == 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xF8: // RET M
		if r.F&FlagS != 0 { r.PC = c.popw(); r.WZ = r.PC; return 11 }
		return 5
	case opcode == 0xC9: // RET
		r.PC = c.popw(); r.WZ = r.PC; return 10

	case opcode == 0xC1: r.SetBC(c.popw()); return 10 // POP BC
	case opcode == 0xD1: r.SetDE(c.popw()); return 10 // POP DE
	case opcode == 0xE1: r.SetHL(c.popw()); return 10 // POP HL
	case opcode == 0xF1: // POP AF
		val := c.popw()
		r.SetAF(val); return 10
	case opcode == 0xC5: c.pushw(r.BC()); return 11 // PUSH BC
	case opcode == 0xD5: c.pushw(r.DE()); return 11 // PUSH DE
	case opcode == 0xE5: c.pushw(r.HL()); return 11 // PUSH HL
	case opcode == 0xF5: c.pushw(r.AF()); return 11 // PUSH AF

	case opcode == 0xC3: // JP nn
		addr := c.nextw()
		r.PC = addr; r.WZ = addr; return 10
	case opcode == 0xC2: // JP NZ, nn
		addr := c.nextw()
		if r.F&FlagZ == 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xCA: // JP Z, nn
		addr := c.nextw()
		if r.F&FlagZ != 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xD2: // JP NC, nn
		addr := c.nextw()
		if r.F&FlagC == 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xDA: // JP C, nn
		addr := c.nextw()
		if r.F&FlagC != 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xE2: // JP PO, nn
		addr := c.nextw()
		if r.F&FlagP == 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xEA: // JP PE, nn
		addr := c.nextw()
		if r.F&FlagP != 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xF2: // JP P, nn
		addr := c.nextw()
		if r.F&FlagS == 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xFA: // JP M, nn
		addr := c.nextw()
		if r.F&FlagS != 0 { r.PC = addr; r.WZ = addr }; return 10
	case opcode == 0xE9: // JP (HL)
		r.PC = r.HL(); return 4

	case opcode == 0xCD: // CALL nn
		addr := c.nextw()
		c.pushw(r.PC); r.PC = addr; r.WZ = addr; return 17
	case opcode == 0xC4: // CALL NZ, nn
		addr := c.nextw()
		if r.F&FlagZ == 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xCC: // CALL Z, nn
		addr := c.nextw()
		if r.F&FlagZ != 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xD4: // CALL NC, nn
		addr := c.nextw()
		if r.F&FlagC == 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xDC: // CALL C, nn
		addr := c.nextw()
		if r.F&FlagC != 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xE4: // CALL PO, nn
		addr := c.nextw()
		if r.F&FlagP == 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xEC: // CALL PE, nn
		addr := c.nextw()
		if r.F&FlagP != 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xF4: // CALL P, nn
		addr := c.nextw()
		if r.F&FlagS == 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17
	case opcode == 0xFC: // CALL M, nn
		addr := c.nextw()
		if r.F&FlagS != 0 { c.pushw(r.PC); r.PC = addr }; r.WZ = addr; return 17

	case opcode == 0xC7: c.pushw(r.PC); r.PC = 0x00; r.WZ = 0x00; return 11 // RST 00
	case opcode == 0xCF: c.pushw(r.PC); r.PC = 0x08; r.WZ = 0x08; return 11 // RST 08
	case opcode == 0xD7: c.pushw(r.PC); r.PC = 0x10; r.WZ = 0x10; return 11 // RST 10
	case opcode == 0xDF: c.pushw(r.PC); r.PC = 0x18; r.WZ = 0x18; return 11 // RST 18
	case opcode == 0xE7: c.pushw(r.PC); r.PC = 0x20; r.WZ = 0x20; return 11 // RST 20
	case opcode == 0xEF: c.pushw(r.PC); r.PC = 0x28; r.WZ = 0x28; return 11 // RST 28
	case opcode == 0xF7: c.pushw(r.PC); r.PC = 0x30; r.WZ = 0x30; return 11 // RST 30
	case opcode == 0xFF: c.pushw(r.PC); r.PC = 0x38; r.WZ = 0x38; return 11 // RST 38

	case opcode == 0xD3: // OUT (n), A
		port := (uint16(r.A) << 8) | uint16(c.nextb())
		c.writeIOPort(port, r.A)
		r.WZ = ((port + 1) & 0xFF) | (uint16(r.A) << 8)
		return 11
	case opcode == 0xDB: // IN A, (n)
		port := (uint16(r.A) << 8) | uint16(c.nextb())
		r.A = c.readIOPort(port)
		r.WZ = port + 1
		return 11

	case opcode == 0xE3: // EX (SP), HL
		spVal := c.rw(r.SP)
		c.ww(r.SP, r.HL())
		r.SetHL(spVal)
		r.WZ = spVal
		return 19
	case opcode == 0xEB: // EX DE, HL
		r.D, r.H = r.H, r.D
		r.E, r.L = r.L, r.E
		return 4

	case opcode == 0xF3: // DI
		r.IFF1 = false
		r.IFF2 = false
		r.IFFDelay = 0
		return 4
	case opcode == 0xFB: // EI
		r.IFF1 = true
		r.IFF2 = true
		r.IFFDelay = 1
		return 4

	// === Prefix escapes (0xCB, 0xED, 0xDD, 0xFD) — handled by Step() ===
	case opcode == 0xCB:
		return c.execOpcodeCB(c.nextb())
	case opcode == 0xED:
		return c.execOpcodeED(c.nextb())
	case opcode == 0xDD:
		return c.execOpcodeDDFD(c.nextb(), &r.IXh, &r.IXl)
	case opcode == 0xFD:
		return c.execOpcodeDDFD(c.nextb(), &r.IYh, &r.IYl)
	}

	// Unknown opcode — NOP
	return 4
}

// ==========================================================================
// Step — main instruction execution entry point
// ==========================================================================

// Step executes one instruction and returns the number of T-states consumed.
func (c *CPU) Step() int {
	r := &c.Regs

	// NMI handling
	if c.NmiPending {
		c.NmiPending = false
		r.IFF2 = r.IFF1
		r.IFF1 = false
		c.pushw(r.PC)
		r.PC = 0x0066
		r.WZ = r.PC
		r.Halted = false
		c.Cycles += 11
		return 11
	}

	// Handle HALT state
	if r.Halted {
		// In HALT, CPU executes NOPs until an interrupt occurs
		if c.IntPending {
			r.Halted = false
		}
		c.Cycles += 4
		return 4
	}

	// EI delay processing
	if r.IFFDelay > 0 {
		r.IFFDelay--
	}

	// Fetch opcode
	opcode := c.nextb()
	r.incR()

	// Dispatch
	cycles := c.execOpcodeBase(opcode)

	// Maskable interrupt handling (after instruction completes)
	if c.IntPending && r.IFF1 && r.IFFDelay == 0 {
		c.IntPending = false
		r.IFF1 = false
		r.IFF2 = false

		switch r.IM {
		case 0:
			// IM 0: execute the opcode provided on the data bus
			c.execOpcodeBase(c.IntData)
			cycles += 13
		case 1:
			// IM 1: RST 38h
			c.pushw(r.PC)
			r.PC = 0x0038
			r.WZ = 0x0038
			cycles += 13
		case 2:
			// IM 2: call to vector table
			addr := (uint16(r.I) << 8) | uint16(c.IntData)
			low := c.rb(addr)
			high := c.rb(addr + 1)
			vector := (uint16(high) << 8) | uint16(low)
			c.pushw(r.PC)
			r.PC = vector
			r.WZ = vector
			cycles += 19
		}
	}

	c.Cycles += uint64(cycles)
	return cycles
}

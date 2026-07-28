package z80

import (
	"testing"
)

// mockMem is a simple 64KB memory for testing.
type mockMem struct {
	mem [65536]uint8
}

func (m *mockMem) ReadMemory(addr uint16) uint8  { return m.mem[addr] }
func (m *mockMem) WriteMemory(addr uint16, val uint8) { m.mem[addr] = val }

// mockIO is a simple I/O port array for testing.
type mockIO struct {
	ports      [256]uint8
	readCount  int
	writeCount int
}

func (m *mockIO) ReadIO(port uint16) uint8 {
	m.readCount++
	return m.ports[port&0xFF]
}

func (m *mockIO) WriteIO(port uint16, val uint8) {
	m.writeCount++
	m.ports[port&0xFF] = val
}

func newTestCPU() (*CPU, *mockMem, *mockIO) {
	mem := &mockMem{}
	io := &mockIO{}
	cpu := NewCPU(mem, io)
	return cpu, mem, io
}

// loadOpcodes loads the given bytes at address 0 and sets PC=0.
func loadOpcodes(cpu *CPU, mem *mockMem, opcodes ...uint8) {
	for i, b := range opcodes {
		mem.mem[i] = b
	}
	cpu.Regs.PC = 0
}

func TestNOP(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x00) // NOP
	cpu.Step()
	if cpu.Regs.PC != 1 {
		t.Errorf("NOP: expected PC=1, got %d", cpu.Regs.PC)
	}
}

func TestLDBn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x06, 0x42) // LD B, $42
	cpu.Step()
	if cpu.Regs.B != 0x42 {
		t.Errorf("LD B,n: expected B=$42, got $%02X", cpu.Regs.B)
	}
	if cpu.Regs.PC != 2 {
		t.Errorf("LD B,n: expected PC=2, got %d", cpu.Regs.PC)
	}
}

func TestLDBCnn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x01, 0x34, 0x12) // LD BC, $1234
	cpu.Step()
	if cpu.Regs.BC() != 0x1234 {
		t.Errorf("LD BC,nn: expected BC=$1234, got $%04X", cpu.Regs.BC())
	}
}

func TestINCB(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.B = 0x41
	loadOpcodes(cpu, mem, 0x04) // INC B
	cpu.Step()
	if cpu.Regs.B != 0x42 {
		t.Errorf("INC B: expected B=$42, got $%02X", cpu.Regs.B)
	}
	// Carry should not be affected
	if cpu.Regs.F&FlagC != 0 {
		t.Error("INC B: carry flag should not be affected")
	}
}

func TestDECB(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.B = 0x42
	loadOpcodes(cpu, mem, 0x05) // DEC B
	cpu.Step()
	if cpu.Regs.B != 0x41 {
		t.Errorf("DEC B: expected B=$41, got $%02X", cpu.Regs.B)
	}
}

func TestLDA_n(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x3E, 0xAA) // LD A, $AA
	cpu.Step()
	if cpu.Regs.A != 0xAA {
		t.Errorf("LD A,n: expected A=$AA, got $%02X", cpu.Regs.A)
	}
}

func TestADDAB(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x10
	cpu.Regs.B = 0x20
	loadOpcodes(cpu, mem, 0x80) // ADD A, B
	cpu.Step()
	if cpu.Regs.A != 0x30 {
		t.Errorf("ADD A,B: expected A=$30, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagZ != 0 {
		t.Error("ADD A,B: zero flag should not be set")
	}
}

func TestADDABOverflow(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0xFF
	cpu.Regs.B = 0x01
	loadOpcodes(cpu, mem, 0x80) // ADD A, B
	cpu.Step()
	if cpu.Regs.A != 0x00 {
		t.Errorf("ADD A,B overflow: expected A=0, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagZ == 0 {
		t.Error("ADD A,B overflow: zero flag should be set")
	}
	if cpu.Regs.F&FlagC == 0 {
		t.Error("ADD A,B overflow: carry flag should be set")
	}
}

func TestSUBB(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x30
	cpu.Regs.B = 0x10
	loadOpcodes(cpu, mem, 0x90) // SUB B
	cpu.Step()
	if cpu.Regs.A != 0x20 {
		t.Errorf("SUB B: expected A=$20, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagN == 0 {
		t.Error("SUB B: subtract flag should be set")
	}
}

func TestCPB(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x42
	cpu.Regs.B = 0x42
	loadOpcodes(cpu, mem, 0xB8) // CP B
	cpu.Step()
	if cpu.Regs.A != 0x42 {
		t.Error("CP B: A should be unchanged")
	}
	if cpu.Regs.F&FlagZ == 0 {
		t.Error("CP B: zero flag should be set for equal values")
	}
}

func TestLDHL_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0xAB
	cpu.Regs.SetHL(0x1000)
	loadOpcodes(cpu, mem, 0x77) // LD (HL), A
	cpu.Step()
	if mem.mem[0x1000] != 0xAB {
		t.Errorf("LD (HL),A: expected mem[0x1000]=$AB, got $%02X", mem.mem[0x1000])
	}
}

func TestLDA_HL(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetHL(0x1000)
	mem.mem[0x1000] = 0xCD
	loadOpcodes(cpu, mem, 0x7E) // LD A, (HL)
	cpu.Step()
	if cpu.Regs.A != 0xCD {
		t.Errorf("LD A,(HL): expected A=$CD, got $%02X", cpu.Regs.A)
	}
}

func TestLDBC_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0xEF
	cpu.Regs.SetBC(0x2000)
	loadOpcodes(cpu, mem, 0x02) // LD (BC), A
	cpu.Step()
	if mem.mem[0x2000] != 0xEF {
		t.Errorf("LD (BC),A: expected mem[0x2000]=$EF, got $%02X", mem.mem[0x2000])
	}
}

func TestINCBC(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetBC(0xFFFF)
	loadOpcodes(cpu, mem, 0x03) // INC BC
	cpu.Step()
	if cpu.Regs.BC() != 0x0000 {
		t.Errorf("INC BC: expected BC=0, got $%04X", cpu.Regs.BC())
	}
}

func TestRLCA(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x81 // 10000001
	loadOpcodes(cpu, mem, 0x07) // RLCA
	cpu.Step()
	if cpu.Regs.A != 0x03 { // 00000011
		t.Errorf("RLCA: expected A=$03, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagC == 0 {
		t.Error("RLCA: carry should be set (bit 7 was 1)")
	}
}

func TestJPnn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0xC3, 0x00, 0x10) // JP $1000
	cpu.Step()
	if cpu.Regs.PC != 0x1000 {
		t.Errorf("JP nn: expected PC=$1000, got $%04X", cpu.Regs.PC)
	}
}

func TestINAn(t *testing.T) {
	cpu, mem, io := newTestCPU()
	cpu.Regs.A = 0x01
	io.ports[0x01] = 0xBB
	loadOpcodes(cpu, mem, 0xDB, 0x01) // IN A, (1)
	cpu.Step()
	if cpu.Regs.A != 0xBB {
		t.Errorf("IN A,(n): expected A=$BB, got $%02X", cpu.Regs.A)
	}
	if io.readCount != 1 {
		t.Errorf("IN A,(n): expected 1 read, got %d", io.readCount)
	}
}

func TestOUTnA(t *testing.T) {
	cpu, mem, io := newTestCPU()
	cpu.Regs.A = 0xCC
	loadOpcodes(cpu, mem, 0xD3, 0x02) // OUT (2), A
	cpu.Step()
	if io.ports[0x02] != 0xCC {
		t.Errorf("OUT (n),A: expected port[2]=$CC, got $%02X", io.ports[0x02])
	}
	if io.writeCount != 1 {
		t.Errorf("OUT (n),A: expected 1 write, got %d", io.writeCount)
	}
}

func TestJRn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x18, 0x05) // JR $0007 (PC=2 + 5 = 7)
	cpu.Step()
	if cpu.Regs.PC != 7 {
		t.Errorf("JR n forward: expected PC=7, got %d", cpu.Regs.PC)
	}
}

func TestJRNZ_taken(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F &^= FlagZ // clear Z flag
	loadOpcodes(cpu, mem, 0x20, 0x03) // JR NZ, $0005
	cpu.Step()
	if cpu.Regs.PC != 5 {
		t.Errorf("JR NZ (taken): expected PC=5, got %d", cpu.Regs.PC)
	}
}

func TestJRNZ_notTaken(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F |= FlagZ // set Z flag
	loadOpcodes(cpu, mem, 0x20, 0x03) // JR NZ, $0005
	cpu.Step()
	if cpu.Regs.PC != 2 {
		t.Errorf("JR NZ (not taken): expected PC=2, got %d", cpu.Regs.PC)
	}
}

func TestJRNZ_negative(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F &^= FlagZ
	loadOpcodes(cpu, mem, 0x20, 0xFE) // JR NZ, $-2 (back to 0)
	cpu.Step()
	if cpu.Regs.PC != 0 {
		t.Errorf("JR NZ backward: expected PC=0, got %d", cpu.Regs.PC)
	}
}

func TestCALL_RET(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SP = 0xFFFF
	loadOpcodes(cpu, mem, 0xCD, 0x00, 0x20) // CALL $2000
	cpu.Step()
	if cpu.Regs.PC != 0x2000 {
		t.Errorf("CALL: expected PC=$2000, got $%04X", cpu.Regs.PC)
	}
	if cpu.Regs.SP != 0xFFFD {
		t.Errorf("CALL: expected SP=FFFD, got $%04X", cpu.Regs.SP)
	}
	// Verify return address on stack
	retAddr := (uint16(mem.mem[0xFFFE]) << 8) | uint16(mem.mem[0xFFFD])
	if retAddr != 0x0003 {
		t.Errorf("CALL: expected return addr=$0003 on stack, got $%04X", retAddr)
	}

	// Now test RET
	mem.mem[0x2000] = 0xC9 // RET
	cpu.Step()
	if cpu.Regs.PC != 0x0003 {
		t.Errorf("RET: expected PC=$0003, got $%04X", cpu.Regs.PC)
	}
	if cpu.Regs.SP != 0xFFFF {
		t.Errorf("RET: expected SP=FFFF, got $%04X", cpu.Regs.SP)
	}
}

func TestDI_EI(t *testing.T) {
	cpu, mem, _ := newTestCPU()

	// DI
	loadOpcodes(cpu, mem, 0xF3) // DI
	cpu.Step()
	if cpu.Regs.IFF1 {
		t.Error("DI: IFF1 should be false")
	}
	if cpu.Regs.IFF2 {
		t.Error("DI: IFF2 should be false")
	}

	// EI
	cpu.Regs.PC = 0
	mem.mem[0] = 0xFB // EI
	mem.mem[1] = 0x00 // NOP (EI takes effect after next instruction)
	cpu.Step() // EI
	cpu.Step() // NOP — EI now takes effect
	if !cpu.Regs.IFF1 {
		t.Error("EI: IFF1 should be true after EI + next instruction")
	}
}

func TestLDA_B(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.B = 0x55
	loadOpcodes(cpu, mem, 0x78) // LD A, B
	cpu.Step()
	if cpu.Regs.A != 0x55 {
		t.Errorf("LD A,B: expected A=$55, got $%02X", cpu.Regs.A)
	}
}

func TestLDB_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x77
	loadOpcodes(cpu, mem, 0x47) // LD B, A
	cpu.Step()
	if cpu.Regs.B != 0x77 {
		t.Errorf("LD B,A: expected B=$77, got $%02X", cpu.Regs.B)
	}
}

func TestLDHL_nn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x21, 0x00, 0x80) // LD HL, $8000
	cpu.Step()
	if cpu.Regs.HL() != 0x8000 {
		t.Errorf("LD HL,nn: expected HL=$8000, got $%04X", cpu.Regs.HL())
	}
}

func TestLDDEnn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x11, 0xEF, 0xBE) // LD DE, $BEEF
	cpu.Step()
	if cpu.Regs.DE() != 0xBEEF {
		t.Errorf("LD DE,nn: expected DE=$BEEF, got $%04X", cpu.Regs.DE())
	}
}

func TestLDCn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x0E, 0x99) // LD C, $99
	cpu.Step()
	if cpu.Regs.C != 0x99 {
		t.Errorf("LD C,n: expected C=$99, got $%02X", cpu.Regs.C)
	}
}

func TestLDDn(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x16, 0x88) // LD D, $88
	cpu.Step()
	if cpu.Regs.D != 0x88 {
		t.Errorf("LD D,n: expected D=$88, got $%02X", cpu.Regs.D)
	}
}

func TestJPNZ_taken(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F &^= FlagZ // Z=0
	loadOpcodes(cpu, mem, 0xC2, 0x00, 0x30) // JP NZ, $3000
	cpu.Step()
	if cpu.Regs.PC != 0x3000 {
		t.Errorf("JP NZ (taken): expected PC=$3000, got $%04X", cpu.Regs.PC)
	}
}

func TestJPNZ_notTaken(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F |= FlagZ // Z=1
	loadOpcodes(cpu, mem, 0xC2, 0x00, 0x30) // JP NZ, $3000
	cpu.Step()
	if cpu.Regs.PC != 3 {
		t.Errorf("JP NZ (not taken): expected PC=3, got %d", cpu.Regs.PC)
	}
}

func TestCBRLC_B(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.B = 0x81 // 10000001
	loadOpcodes(cpu, mem, 0xCB, 0x00) // RLC B
	cpu.Step()
	if cpu.Regs.B != 0x03 {
		t.Errorf("RLC B: expected B=$03, got $%02X", cpu.Regs.B)
	}
	if cpu.Regs.F&FlagC == 0 {
		t.Error("RLC B: carry should be set")
	}
}

func TestCBBIT3_D(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.D = 0x08 // bit 3 is set
	loadOpcodes(cpu, mem, 0xCB, 0x5A) // BIT 3, D
	cpu.Step()
	if cpu.Regs.D != 0x08 {
		t.Error("BIT 3,D: D should be unchanged")
	}
	if cpu.Regs.F&FlagZ != 0 {
		t.Error("BIT 3,D: Z should be 0 (bit is set)")
	}
}

func TestCBIT3_D_zero(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.D = 0x00 // bit 3 is clear
	loadOpcodes(cpu, mem, 0xCB, 0x5A) // BIT 3, D
	cpu.Step()
	if cpu.Regs.F&FlagZ == 0 {
		t.Error("BIT 3,D: Z should be 1 (bit is clear)")
	}
}

func TestCBRES2_E(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.E = 0xFF
	loadOpcodes(cpu, mem, 0xCB, 0x93) // RES 2, E
	cpu.Step()
	if cpu.Regs.E != 0xFB { // 11111011
		t.Errorf("RES 2,E: expected E=$FB, got $%02X", cpu.Regs.E)
	}
}

func TestCBSET5_L(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.L = 0x00
	loadOpcodes(cpu, mem, 0xCB, 0xED) // SET 5, L
	cpu.Step()
	if cpu.Regs.L != 0x20 {
		t.Errorf("SET 5,L: expected L=$20, got $%02X", cpu.Regs.L)
	}
}

func TestEDINB_C(t *testing.T) {
	cpu, mem, io := newTestCPU()
	cpu.Regs.B = 0x00
	cpu.Regs.C = 0x10
	io.ports[0x10] = 0xDA
	loadOpcodes(cpu, mem, 0xED, 0x40) // IN B, (C)
	cpu.Step()
	if cpu.Regs.B != 0xDA {
		t.Errorf("IN B,(C): expected B=$DA, got $%02X", cpu.Regs.B)
	}
}

func TestRRD(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x84
	cpu.Regs.SetHL(0x1000)
	mem.mem[0x1000] = 0x20
	loadOpcodes(cpu, mem, 0xED, 0x67) // RRD
	cpu.Step()

	// RRD: A = (A & 0xF0) | (mem[HL] & 0x0F) = 0x80 | 0x00 = 0x80
	// mem[HL] = (mem[HL] >> 4) | ((A_old & 0x0F) << 4) = 0x02 | 0x40 = 0x42
	if cpu.Regs.A != 0x80 {
		t.Errorf("RRD: expected A=$80, got $%02X", cpu.Regs.A)
	}
	if mem.mem[0x1000] != 0x42 {
		t.Errorf("RRD: expected mem[HL]=$42, got $%02X", mem.mem[0x1000])
	}
}

func TestRLD(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x84
	cpu.Regs.SetHL(0x1000)
	mem.mem[0x1000] = 0x20
	loadOpcodes(cpu, mem, 0xED, 0x6F) // RLD
	cpu.Step()

	if cpu.Regs.A != 0x82 {
		t.Errorf("RLD: expected A=$82, got $%02X", cpu.Regs.A)
	}
	if mem.mem[0x1000] != 0x04 {
		t.Errorf("RLD: expected mem[HL]=$04, got $%02X", mem.mem[0x1000])
	}
}

func TestXOR_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0xFF
	loadOpcodes(cpu, mem, 0xAF) // XOR A
	cpu.Step()
	if cpu.Regs.A != 0x00 {
		t.Errorf("XOR A: expected A=0, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagZ == 0 {
		t.Error("XOR A: Z should be set")
	}
	if cpu.Regs.F&FlagC != 0 {
		t.Error("XOR A: C should be clear")
	}
}

func TestOR_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x0F
	cpu.Regs.B = 0xF0
	loadOpcodes(cpu, mem, 0xB0) // OR B
	cpu.Step()
	if cpu.Regs.A != 0xFF {
		t.Errorf("OR B: expected A=$FF, got $%02X", cpu.Regs.A)
	}
}

func TestAND_A(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0xFF
	cpu.Regs.B = 0x0F
	loadOpcodes(cpu, mem, 0xA0) // AND B
	cpu.Step()
	if cpu.Regs.A != 0x0F {
		t.Errorf("AND B: expected A=$0F, got $%02X", cpu.Regs.A)
	}
}

func TestDAA(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x09
	cpu.Regs.B = 0x08
	// ADD A,B gives 0x11, then DAA corrects to 0x17 (BCD 17)
	loadOpcodes(cpu, mem, 0x80, 0x27) // ADD A, B; DAA
	cpu.Step() // ADD
	if cpu.Regs.A != 0x11 {
		t.Fatalf("ADD: expected A=$11, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagH == 0 {
		t.Fatal("ADD: H flag should be set for half-carry")
	}
	cpu.Step() // DAA
	if cpu.Regs.A != 0x17 {
		t.Errorf("DAA after add: expected A=$17, got $%02X (F=$%02X)", cpu.Regs.A, cpu.Regs.F)
	}
}

func TestSBC(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x30
	cpu.Regs.B = 0x10
	cpu.Regs.F |= FlagC // carry set
	loadOpcodes(cpu, mem, 0x98) // SBC A, B
	cpu.Step()
	// 0x30 - 0x10 - 1 = 0x1F
	if cpu.Regs.A != 0x1F {
		t.Errorf("SBC A,B: expected A=$1F, got $%02X", cpu.Regs.A)
	}
}

func TestADC(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x10
	cpu.Regs.B = 0x20
	cpu.Regs.F |= FlagC
	loadOpcodes(cpu, mem, 0x88) // ADC A, B
	cpu.Step()
	if cpu.Regs.A != 0x31 {
		t.Errorf("ADC A,B: expected A=$31, got $%02X", cpu.Regs.A)
	}
}

func TestEXDE_HL(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetDE(0x1234)
	cpu.Regs.SetHL(0x5678)
	loadOpcodes(cpu, mem, 0xEB) // EX DE, HL
	cpu.Step()
	if cpu.Regs.DE() != 0x5678 {
		t.Errorf("EX DE,HL: expected DE=$5678, got $%04X", cpu.Regs.DE())
	}
	if cpu.Regs.HL() != 0x1234 {
		t.Errorf("EX DE,HL: expected HL=$1234, got $%04X", cpu.Regs.HL())
	}
}

func TestPUSH_POP_AF(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SP = 0xFFFF
	cpu.Regs.A = 0x12
	cpu.Regs.setF(0x34)
	loadOpcodes(cpu, mem, 0xF5) // PUSH AF
	cpu.Step()
	if cpu.Regs.SP != 0xFFFD {
		t.Errorf("PUSH AF: expected SP=FFFD, got $%04X", cpu.Regs.SP)
	}

	// Now POP
	cpu.Regs.A = 0
	cpu.Regs.F = 0
	mem.mem[1] = 0xF1 // POP AF
	cpu.Regs.PC = 1
	cpu.Step()
	if cpu.Regs.A != 0x12 {
		t.Errorf("POP AF: expected A=$12, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.getF() != 0x34 {
		t.Errorf("POP AF: expected F=$34, got $%02X", cpu.Regs.getF())
	}
	if cpu.Regs.SP != 0xFFFF {
		t.Errorf("POP AF: expected SP=FFFF, got $%04X", cpu.Regs.SP)
	}
}

func TestEXX(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetBC(0x1111)
	cpu.Regs.SetDE(0x2222)
	cpu.Regs.SetHL(0x3333)
	cpu.Regs.SetBC1(0xAAAA)
	cpu.Regs.SetDE1(0xBBBB)
	cpu.Regs.SetHL1(0xCCCC)
	loadOpcodes(cpu, mem, 0xD9) // EXX
	cpu.Step()
	if cpu.Regs.BC() != 0xAAAA || cpu.Regs.DE() != 0xBBBB || cpu.Regs.HL() != 0xCCCC {
		t.Error("EXX: primary registers not swapped correctly")
	}
}

func TestDAA_sub(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x10
	cpu.Regs.B = 0x01
	// 0x10 - 0x01 = 0x0F, DAA corrects to 0x09
	loadOpcodes(cpu, mem, 0x90, 0x27) // SUB B; DAA
	cpu.Step()
	cpu.Step()
	if cpu.Regs.A != 0x09 {
		t.Errorf("DAA after sub: expected A=$09, got $%02X", cpu.Regs.A)
	}
}

func TestADD_HL_BC(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetHL(0x1000)
	cpu.Regs.SetBC(0x0234)
	loadOpcodes(cpu, mem, 0x09) // ADD HL, BC
	cpu.Step()
	if cpu.Regs.HL() != 0x1234 {
		t.Errorf("ADD HL,BC: expected HL=$1234, got $%04X", cpu.Regs.HL())
	}
}

func TestHALT(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	loadOpcodes(cpu, mem, 0x76) // HALT
	cpu.Step()
	if !cpu.Regs.Halted {
		t.Error("HALT: CPU should be halted")
	}
	// HALT should be exited on interrupt
	cpu.IntPending = true
	cpu.Step()
	if cpu.Regs.Halted {
		t.Error("HALT: CPU should exit halt on interrupt")
	}
}

func TestLDIR(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetHL(0x5000)
	cpu.Regs.SetDE(0x6000)
	cpu.Regs.SetBC(3)
	mem.mem[0x5000] = 0x11
	mem.mem[0x5001] = 0x22
	mem.mem[0x5002] = 0x33
	loadOpcodes(cpu, mem, 0xED, 0xB0) // LDIR
	// LDIR decrements BC each step; for BC=3 we need 3 Step() calls
	for i := 0; i < 3; i++ {
		cpu.Step()
	}
	if cpu.Regs.BC() != 0 {
		t.Errorf("LDIR: BC should be 0 after transfer, got %d", cpu.Regs.BC())
	}
	if mem.mem[0x6000] != 0x11 || mem.mem[0x6001] != 0x22 || mem.mem[0x6002] != 0x33 {
		t.Error("LDIR: data not copied correctly")
	}
	if cpu.Regs.HL() != 0x5003 {
		t.Errorf("LDIR: HL should be 0x5003, got $%04X", cpu.Regs.HL())
	}
}

func TestADDHL_overflow(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SetHL(0xFFFF)
	cpu.Regs.SetBC(1)
	loadOpcodes(cpu, mem, 0x09) // ADD HL, BC
	cpu.Step()
	if cpu.Regs.HL() != 0x0000 {
		t.Errorf("ADD HL,BC overflow: expected HL=0, got $%04X", cpu.Regs.HL())
	}
	if cpu.Regs.F&FlagC == 0 {
		t.Error("ADD HL,BC overflow: carry should be set")
	}
}

func TestEX_SP_HL(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SP = 0xFF00
	cpu.Regs.SetHL(0x1234)
	mem.mem[0xFF00] = 0x78
	mem.mem[0xFF01] = 0x56
	loadOpcodes(cpu, mem, 0xE3) // EX (SP), HL
	cpu.Step()
	if cpu.Regs.HL() != 0x5678 {
		t.Errorf("EX (SP),HL: expected HL=$5678, got $%04X", cpu.Regs.HL())
	}
	if mem.mem[0xFF00] != 0x34 || mem.mem[0xFF01] != 0x12 {
		t.Error("EX (SP),HL: stack not updated correctly")
	}
}

func TestEXAF(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x11
	cpu.Regs.F = 0x22
	cpu.Regs.A1 = 0xAA
	cpu.Regs.F1 = 0xBB
	loadOpcodes(cpu, mem, 0x08) // EX AF, AF'
	cpu.Step()
	if cpu.Regs.A != 0xAA || cpu.Regs.F != 0xBB {
		t.Error("EX AF,AF': registers not swapped correctly")
	}
	if cpu.Regs.A1 != 0x11 || cpu.Regs.F1 != 0x22 {
		t.Error("EX AF,AF': shadow registers not swapped correctly")
	}
}

func TestRetZ(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SP = 0xFF00
	cpu.Regs.F |= FlagZ
	mem.mem[0xFF00] = 0x00
	mem.mem[0xFF01] = 0x30
	loadOpcodes(cpu, mem, 0xC8) // RET Z
	cpu.Step()
	if cpu.Regs.PC != 0x3000 {
		t.Errorf("RET Z: expected PC=$3000, got $%04X", cpu.Regs.PC)
	}
	if cpu.Regs.SP != 0xFF02 {
		t.Errorf("RET Z: expected SP=FF02, got $%04X", cpu.Regs.SP)
	}
}

func TestCPL(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = 0x55
	loadOpcodes(cpu, mem, 0x2F) // CPL
	cpu.Step()
	if cpu.Regs.A != 0xAA {
		t.Errorf("CPL: expected A=$AA, got $%02X", cpu.Regs.A)
	}
	if cpu.Regs.F&FlagH == 0 {
		t.Error("CPL: H flag should be set")
	}
	if cpu.Regs.F&FlagN == 0 {
		t.Error("CPL: N flag should be set")
	}
}

func TestSCF(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F &^= FlagC
	loadOpcodes(cpu, mem, 0x37) // SCF
	cpu.Step()
	if cpu.Regs.F&FlagC == 0 {
		t.Error("SCF: carry should be set")
	}
	if cpu.Regs.F&FlagH != 0 {
		t.Error("SCF: half-carry should be cleared")
	}
}

func TestCCF(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.F |= FlagC
	loadOpcodes(cpu, mem, 0x3F) // CCF
	cpu.Step()
	if cpu.Regs.F&FlagC != 0 {
		t.Error("CCF: carry should be inverted (clear)")
	}
}

func TestRST(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.SP = 0xFFFF
	loadOpcodes(cpu, mem, 0xC7) // RST 0
	cpu.Step()
	if cpu.Regs.PC != 0x0000 {
		t.Errorf("RST 0: expected PC=0, got $%04X", cpu.Regs.PC)
	}
	if cpu.Regs.SP != 0xFFFD {
		t.Errorf("RST 0: expected SP=FFFD, got $%04X", cpu.Regs.SP)
	}
}

func TestDJNZ(t *testing.T) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.B = 2
	loadOpcodes(cpu, mem, 0x10, 0xFE) // DJNZ $-2 (PC: 0->0)
	cpu.Step()
	if cpu.Regs.PC != 0 {
		t.Errorf("DJNZ first: expected PC=0, got %d", cpu.Regs.PC)
	}
	if cpu.Regs.B != 1 {
		t.Errorf("DJNZ first: expected B=1, got %d", cpu.Regs.B)
	}
	mem.mem[0] = 0x10
	mem.mem[1] = 0xFE
	cpu.Regs.PC = 0
	cpu.Step()
	if cpu.Regs.PC != 2 {
		t.Errorf("DJNZ second (B=0): expected PC=2, got %d", cpu.Regs.PC)
	}
}

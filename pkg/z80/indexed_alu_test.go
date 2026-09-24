package z80

import "testing"

// runIndexedALU executes "PREFIX opcode d" for an indexed (IZ+d) operand and
// returns the resulting A value.
func runIndexedALU(prefix, opcode, d, a, operand, carryIn byte) (*CPU, byte) {
	cpu, mem, _ := newTestCPU()
	cpu.Regs.A = a
	if carryIn != 0 {
		cpu.Regs.F |= FlagC
	}
	// Index register points at a scratch area so the operand address is known.
	cpu.Regs.SetIX(0x5C3A)
	cpu.Regs.SetIY(0x5C3A)
	mem.mem[0x5C3A+uint16(d)] = operand
	loadOpcodes(cpu, mem, prefix, opcode, d)
	cpu.Step()
	return cpu, cpu.Regs.A
}

// TestIndexedADDRegression covers ADD A,(IY+d). This used to apply the addition
// twice (A = 2A + 2*operand) because the indexed-ALU group both performed the
// operation and delegated to the (HL) helper, which performed it again. The ROM
// relies on ADD A,(IY+$31) in CL_ATTR, so the bug left the boot screen blank.
func TestIndexedADDRegression(t *testing.T) {
	cpu, got := runIndexedALU(0xFD, 0x86, 0x31, 0x17, 0x02, 0)
	if got != 0x19 {
		t.Errorf("ADD A,(IY+$31): expected A=$19, got $%02X", got)
	}
	if cpu.Regs.PC != 3 {
		t.Errorf("ADD A,(IY+d): expected PC=3, got %d", cpu.Regs.PC)
	}

	// Same operand through the IX prefix.
	if _, got := runIndexedALU(0xDD, 0x86, 0x31, 0x17, 0x02, 0); got != 0x19 {
		t.Errorf("ADD A,(IX+$31): expected A=$19, got $%02X", got)
	}
}

// TestIndexedALUGroup checks every opcode in the 0x86..0xBE indexed group
// applies its operation exactly once, for both index registers.
func TestIndexedALUGroup(t *testing.T) {
	// A = $17, operand = $02 for all cases.
	cases := []struct {
		name      string
		opcode    byte
		want      byte
		wantCarry bool
	}{
		{"ADD", 0x86, 0x19, false},
		{"ADC", 0x8E, 0x19, false},
		{"SUB", 0x96, 0x15, false},
		{"SBC", 0x9E, 0x15, false},
		{"AND", 0xA6, 0x02, false},
		{"XOR", 0xAE, 0x15, false},
		{"OR", 0xB6, 0x17, false},
		{"CP", 0xBE, 0x17, false}, // A is not modified by CP
	}

	for _, prefix := range []byte{0xDD, 0xFD} {
		for _, tc := range cases {
			cpu, got := runIndexedALU(prefix, tc.opcode, 0x31, 0x17, 0x02, 0)
			if got != tc.want {
				t.Errorf("prefix $%02X %s A,(IZ+d): expected A=$%02X, got $%02X",
					prefix, tc.name, tc.want, got)
			}
			if cpu.Regs.PC != 3 {
				t.Errorf("prefix $%02X %s: expected PC=3, got %d", prefix, tc.name, cpu.Regs.PC)
			}
		}
	}
}

// TestIndexedADCWithCarry makes sure the carry input is used exactly once.
func TestIndexedADCWithCarry(t *testing.T) {
	if _, got := runIndexedALU(0xFD, 0x8E, 0x31, 0x17, 0x02, 1); got != 0x1A {
		t.Errorf("ADC A,(IY+d) with carry: expected A=$1A, got $%02X", got)
	}
}

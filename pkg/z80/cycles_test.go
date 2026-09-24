package z80

import "testing"

// TestInstructionCycles checks the T-state count reported by Step against the
// canonical Z80 timing table (Zilog Z80 CPU User Manual). Timing matters to the
// ROM itself: keyboard debounce, the beeper and cassette loading all derive from
// the T-state clock, so a wrong count here shifts the whole emulated frame.
func TestInstructionCycles(t *testing.T) {
	// setup gives every case a valid, writable operand address.
	setup := func(c *CPU) {
		c.Regs.SetHL(0x8000)
		c.Regs.SetIX(0x8000)
		c.Regs.SetIY(0x8000)
		c.Regs.SetBC(0x0101)
		c.Regs.SetDE(0x0101)
		c.Regs.F = 0
		c.Regs.A = 0x11
	}

	cases := []struct {
		name string
		prep func(*CPU)
		code []uint8
		want int
	}{
		{"NOP", nil, []uint8{0x00}, 4},
		{"LD r,r'", nil, []uint8{0x41}, 4},
		{"LD r,n", nil, []uint8{0x3E, 0x11}, 7},
		{"LD r,(HL)", nil, []uint8{0x7E}, 7},
		{"LD (HL),r", nil, []uint8{0x77}, 7},
		{"LD (HL),n", nil, []uint8{0x36, 0x11}, 10},
		{"LD A,(nn)", nil, []uint8{0x3A, 0x00, 0x80}, 13},
		{"LD (nn),A", nil, []uint8{0x32, 0x00, 0x80}, 13},
		{"INC r", nil, []uint8{0x3C}, 4},
		{"INC (HL)", nil, []uint8{0x34}, 11},
		{"INC rr", nil, []uint8{0x03}, 6},
		{"DEC (HL)", nil, []uint8{0x35}, 11},
		{"ADD A,r", nil, []uint8{0x80}, 4},
		{"ADD A,n", nil, []uint8{0xC6, 0x01}, 7},
		{"ADD A,(HL)", nil, []uint8{0x86}, 7},
		{"CP (HL)", nil, []uint8{0xBE}, 7},
		{"ADD HL,rr", nil, []uint8{0x09}, 11},
		{"JR d taken", nil, []uint8{0x18, 0x02}, 12},
		{"JR NZ d not taken", func(c *CPU) { c.Regs.F = FlagZ }, []uint8{0x20, 0x02}, 7},
		{"DJNZ taken", func(c *CPU) { c.Regs.B = 3 }, []uint8{0x10, 0x02}, 13},
		{"DJNZ not taken", func(c *CPU) { c.Regs.B = 1 }, []uint8{0x10, 0x02}, 8},
		{"PUSH rr", nil, []uint8{0xC5}, 11},
		{"POP rr", nil, []uint8{0xC1}, 10},
		{"CALL nn", nil, []uint8{0xCD, 0x00, 0x90}, 17},
		{"CALL cc taken", func(c *CPU) { c.Regs.F = FlagZ }, []uint8{0xCC, 0x00, 0x90}, 17},
		{"RET", nil, []uint8{0xC9}, 10},
		{"RET cc not taken", nil, []uint8{0xC8}, 5},
		{"RST", nil, []uint8{0xCF}, 11},
		{"JP nn", nil, []uint8{0xC3, 0x00, 0x90}, 10},
		{"EX DE,HL", nil, []uint8{0xEB}, 4},
		{"EXX", nil, []uint8{0xD9}, 4},
		{"LD SP,HL", nil, []uint8{0xF9}, 6},
		{"AND r", nil, []uint8{0xA0}, 4},
		{"XOR r", nil, []uint8{0xA8}, 4},
		{"HALT", nil, []uint8{0x76}, 4},
		{"IN A,(n)", nil, []uint8{0xDB, 0xFE}, 11},
		{"OUT (n),A", nil, []uint8{0xD3, 0xFE}, 11},
		{"IN r,(C)", nil, []uint8{0xED, 0x40}, 12},
		{"OUT (C),r", nil, []uint8{0xED, 0x41}, 12},
		{"LDIR repeat", func(c *CPU) { c.Regs.SetBC(0x0002) }, []uint8{0xED, 0xB0}, 21},
		{"LDIR final", func(c *CPU) { c.Regs.SetBC(0x0001) }, []uint8{0xED, 0xB0}, 16},
		{"CB rot r", nil, []uint8{0xCB, 0x27}, 8},
		{"CB rot (HL)", nil, []uint8{0xCB, 0x06}, 15},
		{"CB BIT (HL)", nil, []uint8{0xCB, 0x46}, 12},
		{"CB SET (HL)", nil, []uint8{0xCB, 0xC6}, 15},
		{"CB RES (HL)", nil, []uint8{0xCB, 0x86}, 15},
		{"IZ LD r,(IZ+d)", nil, []uint8{0xDD, 0x7E, 0x01}, 19},
		{"IZ LD (IZ+d),r", nil, []uint8{0xDD, 0x77, 0x01}, 19},
		{"IZ ADD A,(IZ+d)", nil, []uint8{0xDD, 0x86, 0x01}, 19},
		{"IZ INC (IZ+d)", nil, []uint8{0xDD, 0x34, 0x01}, 23},
		{"IZCB BIT (IZ+d)", nil, []uint8{0xDD, 0xCB, 0x01, 0x46}, 20},
		{"IZCB SET (IZ+d)", nil, []uint8{0xDD, 0xCB, 0x01, 0xC6}, 23},
		{"IZ ADD IZ,rr", nil, []uint8{0xDD, 0x09}, 15},
		{"IZ PUSH IZ", nil, []uint8{0xDD, 0xE5}, 15},
		{"IZ POP IZ", nil, []uint8{0xDD, 0xE1}, 14},
		{"IZ JP (IZ)", nil, []uint8{0xDD, 0xE9}, 8},
		{"IZ LD SP,IZ", nil, []uint8{0xDD, 0xF9}, 10},
		{"EI", nil, []uint8{0xFB}, 4},
		{"DI", nil, []uint8{0xF3}, 4},
		{"IM 1", nil, []uint8{0xED, 0x56}, 8},
		{"RLCA", nil, []uint8{0x07}, 4},
		{"LD (nn),HL", nil, []uint8{0x22, 0x00, 0x80}, 16},
		{"LD HL,(nn)", nil, []uint8{0x2A, 0x00, 0x80}, 16},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cpu, mem, _ := newTestCPU()
			setup(cpu)
			if tc.prep != nil {
				tc.prep(cpu)
			}
			loadOpcodes(cpu, mem, tc.code...)
			if got := cpu.Step(); got != tc.want {
				t.Errorf("%s: got %d T-states, want %d", tc.name, got, tc.want)
			}
		})
	}
}

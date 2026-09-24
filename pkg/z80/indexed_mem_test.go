package z80

import (
	"strings"
	"testing"
)

// writeLogMem records every write so tests can prove an instruction did not
// write memory at all. A same-value write is invisible otherwise.
type writeLogMem struct {
	mem    [65536]uint8
	writes []string
}

func (x *writeLogMem) ReadMemory(addr uint16) uint8 { return x.mem[addr] }
func (x *writeLogMem) WriteMemory(addr uint16, val uint8) {
	x.mem[addr] = val
	x.writes = append(x.writes, fmtAddr(addr))
}

func fmtAddr(a uint16) string {
	const hex = "0123456789ABCDEF"
	return string([]byte{hex[a>>12], hex[a>>8&0xF], hex[a>>4&0xF], hex[a&0xF]})
}

// TestIndexedLoadDoesNotWrite covers the (IZ+d) load and store forms. A load that
// writes the value back is invisible to a register-level test but corrupts system
// variables the ROM keeps in the same memory, and it shows up as a spurious write
// in memory-write tracing.
func TestIndexedLoadDoesNotWrite(t *testing.T) {
	cases := []struct {
		name    string
		code    []uint8
		wantA   uint8
		wantWr  bool // the instruction is expected to write memory
		operand uint8
	}{
		{"LD A,(IY+d)", []uint8{0xFD, 0x7E, 0x01}, 0xAB, false, 0xAB},
		{"LD L,(IY+d)", []uint8{0xFD, 0x6E, 0x01}, 0x00, false, 0xAB},
		{"LD L,(IX+d)", []uint8{0xDD, 0x6E, 0x01}, 0x00, false, 0xAB},
		{"LD (IY+d),A", []uint8{0xFD, 0x77, 0x01}, 0xAB, true, 0x00},
		{"LD (IX+d),A", []uint8{0xDD, 0x77, 0x01}, 0xAB, true, 0x00},
		{"ADD A,(IY+d)", []uint8{0xFD, 0x86, 0x01}, 0xAB + 0x01, false, 0x01},
		{"INC (IY+d)", []uint8{0xFD, 0x34, 0x01}, 0x00, true, 0x01},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			mem := &writeLogMem{}
			cpu := NewCPU(mem, &mockIO{})
			cpu.Regs.SetIX(0x5C3A)
			cpu.Regs.SetIY(0x5C3A)
			cpu.Regs.A = 0xAB
			// The operand address (IZ+1) sits just past a byte we care about.
			mem.mem[0x5C3B] = tc.operand
			copy(mem.mem[0:], tc.code)
			cpu.Regs.PC = 0

			cpu.Step()

			if got := cpu.Regs.A; tc.wantA != 0 && got != tc.wantA {
				t.Errorf("A = %02X, want %02X", got, tc.wantA)
			}
			wrote := len(mem.writes) > 0
			if wrote != tc.wantWr {
				t.Errorf("wrote memory = %v (%s), want %v", wrote, strings.Join(mem.writes, ","), tc.wantWr)
			}
		})
	}
}

package emulator

import (
	"math/bits"
	"os"
	"strings"
	"testing"

	"github.com/defik74/spettrum/internal/emulator/romdata"
)

// TestKeyboardReachesROM types a BASIC command through the keyboard emulation and
// requires the ROM to run it. It covers the whole input chain: the key matrix, the
// ULA port read, the ROM's interrupt-driven keyboard scan and the editor that
// decodes the key. Two bugs made this fail before: IN A,(C) wrote its result into
// B (the register the ROM uses as the keyboard row selector), and the translation
// from host characters to matrix keys used lower case where the matrix stores the
// upper-case key legends, so no letter ever matched a key.
func TestKeyboardReachesROM(t *testing.T) {
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available at %s: %v", realROMPath, err)
	}

	emu := New(Config{
		ROMFile:  realROMPath,
		Audio:    false,
		Unpaced:  true,
		Headless: true,
		// 'p' enters the PRINT keyword in K mode, '+' is SYMBOL SHIFT+K, and CR
		// runs the line, so the ROM should print 4 followed by its report line.
		SimKey:       "p2+2\r",
		Instructions: 4_000_000,
	})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()
	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	screen := strings.Join(screenText(emu.mem[:]), "\n")
	if !strings.Contains(screen, "0 OK, 0:1") {
		t.Errorf("the ROM never reported a result; screen after %d instructions:\n%s",
			emu.totalInst, screen)
	}
	found := false
	for _, line := range screenText(emu.mem[:]) {
		if strings.HasPrefix(strings.TrimSpace(line), "4") {
			found = true
		}
	}
	if !found {
		t.Errorf("PRINT 2+2 did not produce 4; screen:\n%s", screen)
	}
}

// ROM character set base. The CHARS system variable points 256 bytes before it,
// so glyphs for codes $20-$7F start here.
const charsetBase = 0x3D00

// FRAMES system variable: incremented by the ROM's maskable interrupt handler.
const sysFrames = 0x5C78

// romGlyph returns the 8-byte bitmap for a ROM character code.
func romGlyph(code byte) []byte {
	base := charsetBase + (int(code)-0x20)*8
	return romdata.DefaultROM[base : base+8]
}

// vramByte returns the VRAM byte holding pixel row y for the column at x/8.
// The Spectrum bitmap uses an interleaved layout rather than a linear one.
func vramByte(mem []byte, x, y int) byte {
	return mem[0x4000+(y/64)*2048+(y%8)*256+((y%64)/8)*32+x/8]
}

// screenText decodes the display into 24 lines of 32 characters by matching
// each 8x8 cell against the ROM character set.
func screenText(mem []byte) []string {
	lines := make([]string, 24)
	var b strings.Builder
	for row := 0; row < 24; row++ {
		b.Reset()
		for col := 0; col < 32; col++ {
			var cell [8]byte
			for i := 0; i < 8; i++ {
				cell[i] = vramByte(mem, col*8, row*8+i)
			}
			best, bestDist := byte(' '), 1<<30
			for code := 0x20; code <= 0x7F; code++ {
				glyph := romGlyph(byte(code))
				d := 0
				for i := 0; i < 8; i++ {
					d += bits.OnesCount8(cell[i] ^ glyph[i])
				}
				if d < bestDist {
					bestDist, best = d, byte(code)
				}
			}
			if bestDist > 10 {
				best = ' '
			}
			b.WriteByte(best)
		}
		lines[row] = b.String()
	}
	return lines
}

// TestBootsEmbeddedROM boots the built-in 48K ROM headlessly and checks that the
// machine actually reaches a working BASIC editor state: frame interrupts are
// being delivered and the copyright message has been painted on screen.
//
// This is the regression test for the indexed-ALU bug that left the screen
// blank: ADD A,(IY+$31) in the ROM's CL_ATTR routine corrupted the attribute
// arithmetic, so the boot message was never drawn.
func TestBootsEmbeddedROM(t *testing.T) {
	const instructions = 3_000_000

	emu := New(Config{Headless: true, Instructions: instructions, Audio: false})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if emu.totalInst != instructions {
		t.Errorf("expected %d instructions executed, got %d", instructions, emu.totalInst)
	}

	frames := uint16(emu.mem[sysFrames]) | uint16(emu.mem[sysFrames+1])<<8
	if frames == 0 {
		t.Error("no 50Hz interrupts were delivered (FRAMES is still 0)")
	}

	text := strings.Join(screenText(emu.mem[:]), "\n")
	if !strings.Contains(text, "1982 Sinclair Research Ltd") {
		t.Errorf("ROM did not paint the boot message; screen was:\n%s", text)
	}
}

// TestWriteMemoryProtectsROM checks that ROM writes are ignored, which the
// original machine relies on.
func TestWriteMemoryProtectsROM(t *testing.T) {
	emu := New(Config{Headless: true, Audio: false})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	before := emu.mem[0x0000]
	emu.WriteMemory(0x0000, before^0xFF)
	if emu.mem[0x0000] != before {
		t.Error("WriteMemory modified ROM")
	}

	emu.WriteMemory(0x8000, 0x5A)
	if emu.mem[0x8000] != 0x5A {
		t.Error("WriteMemory did not write to RAM")
	}
}

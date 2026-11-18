package emulator

import (
	"math/bits"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// TestKeyboardReachesROM types a BASIC command through the keyboard emulation and
// requires the ROM to run it. It covers the whole input chain: the key matrix, the
// ULA port read, the ROM's interrupt-driven keyboard scan and the editor that
// decodes the key. Two bugs made this fail before: IN A,(C) wrote its result into
// B (the register the ROM uses as the keyboard row selector), and the translation
// from host characters to matrix keys used lower case where the matrix stores the
// upper-case key legends, so no letter ever matched a key.
func TestKeyboardReachesROM(t *testing.T) {
	requireROM(t)

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

// requireROM skips a test when the checkout has no Amstrad ROM image. Spettrum
// ships only OpenSE BASIC, so tests that pin the original ROM's behaviour only
// run where one was supplied.
func requireROM(t *testing.T) {
	t.Helper()
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available at %s: %v", realROMPath, err)
	}
}

// bundledROMPath is the freely licensed OpenSE BASIC image that releases ship.
var bundledROMPath = filepath.Join("..", "..", "third_party", "opense-basic", "opense.rom")

// fontBytes is the size of the printable-character bitmaps at charsetBase.
const fontBytes = (0x80 - 0x20) * 8

// testROM returns the ROM image the screen decoders compare against, or nil
// when it is not present.
var testROM = sync.OnceValue(func() []byte {
	data, err := os.ReadFile(realROMPath)
	if err != nil {
		return nil
	}
	return data
})

// romGlyph returns the 8-byte bitmap for a ROM character code, or nil when the
// ROM image is not available.
func romGlyph(rom []byte, code byte) []byte {
	base := (int(code) - 0x20) * 8
	if len(rom) < base+8 {
		return nil
	}
	return rom[base : base+8]
}

// vramByte returns the VRAM byte holding pixel row y for the column at x/8.
// The Spectrum bitmap uses an interleaved layout rather than a linear one.
func vramByte(mem []byte, x, y int) byte {
	return mem[0x4000+(y/64)*2048+(y%8)*256+((y%64)/8)*32+x/8]
}

// screenText decodes the display into 24 lines of 32 characters by matching
// each 8x8 cell against the character bitmaps of the ROM the tests boot.
func screenText(mem []byte) []string {
	rom := testROM()
	if len(rom) < charsetBase+fontBytes {
		return screenTextWith(mem, nil)
	}
	return screenTextWith(mem, rom[charsetBase:charsetBase+fontBytes])
}

// screenTextWith decodes the display using font, where font[0] is the bitmap for
// ' '. Passing a machine's own font decodes whichever ROM it booted.
func screenTextWith(mem, font []byte) []string {
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
				glyph := romGlyph(font, byte(code))
				if glyph == nil {
					continue
				}
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

// TestBootsROM boots the 48K ROM headlessly and checks that the machine
// actually reaches a working BASIC editor state: frame interrupts are being
// delivered and the copyright message has been painted on screen.
//
// The ROM image is not part of the repository, so this skips without one.
//
// This is the regression test for the indexed-ALU bug that left the screen
// blank: ADD A,(IY+$31) in the ROM's CL_ATTR routine corrupted the attribute
// arithmetic, so the boot message was never drawn.
func TestBootsROM(t *testing.T) {
	requireROM(t)
	const instructions = 3_000_000

	emu := New(Config{ROMFile: realROMPath, Headless: true, Instructions: instructions, Audio: false})
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

// TestBootsBundledROM boots the OpenSE BASIC image that release archives carry,
// so CI exercises a real ROM even though the Amstrad image is never shipped. It
// checks that the machine reaches the editor: interrupts are being delivered and
// the ROM painted its banner.
func TestBootsBundledROM(t *testing.T) {
	if _, err := os.Stat(bundledROMPath); err != nil {
		t.Fatalf("bundled ROM missing: %v", err)
	}

	const instructions = 3_000_000
	emu := New(Config{ROMFile: bundledROMPath, Headless: true, Instructions: instructions, Audio: false})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	frames := uint16(emu.mem[sysFrames]) | uint16(emu.mem[sysFrames+1])<<8
	if frames == 0 {
		t.Error("no 50Hz interrupts were delivered (FRAMES is still 0)")
	}

	text := strings.Join(screenTextWith(emu.mem[:], emu.mem[charsetBase:charsetBase+fontBytes]), "\n")
	if !strings.Contains(text, "Nine Tiles Networks") {
		t.Errorf("the bundled ROM did not paint its banner; screen was:\n%s", text)
	}
}

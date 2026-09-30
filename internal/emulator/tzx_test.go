package emulator

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/defik74/spettrum/pkg/tzx"
)

// ---------------------------------------------------------------------------
// Building TZX images
// ---------------------------------------------------------------------------

// tzxHeader is the ten-byte header every TZX file starts with: the signature and
// the version of the format, 1.20.
func tzxHeader() []byte {
	return append([]byte("ZXTape!\x1a"), 1, 20)
}

// tzxStandardBlock wraps a block of bytes as a standard speed data block: the
// pause that follows it in milliseconds, the length of the bytes, and the bytes
// themselves, which for a standard speed block are the flag, the payload and the
// checksum - exactly what a TAP file holds.
func tzxStandardBlock(pauseMs int, block []byte) []byte {
	out := []byte{0x10}
	out = binary.LittleEndian.AppendUint16(out, uint16(pauseMs))
	out = binary.LittleEndian.AppendUint16(out, uint16(len(block)))
	return append(out, block...)
}

// tzxTurboBlock wraps the same bytes in a turbo speed block, which carries its
// own timings rather than implying them. The timings here are the ROM's own, so
// that the ROM's loader can still read the result; what the block proves is that
// the explicit timings are parsed and played.
func tzxTurboBlock(pauseMs int, block []byte, flag byte) []byte {
	// The leader is longer for a header block, which is how the ROM can tell the
	// two apart. The counts are 8063 and 3223; the specification says 8060 and
	// 3220, which is wrong, as libspectrum notes.
	pilot := 3223
	if flag < 0x80 {
		pilot = 8063
	}
	out := []byte{0x11}
	for _, v := range []int{tzx.TimingPilot, tzx.TimingSync1, tzx.TimingSync2,
		tzx.TimingZero, tzx.TimingOne, pilot} {
		out = binary.LittleEndian.AppendUint16(out, uint16(v))
	}
	out = append(out, 8) // bits used in the last byte
	out = binary.LittleEndian.AppendUint16(out, uint16(pauseMs))
	out = append(out, byte(len(block)), byte(len(block)>>8), byte(len(block)>>16))
	return append(out, block...)
}

// tzxBlockData is the bytes a standard speed block carries: the block as it
// appears on the tape. That is a TAP block's bytes with its checksum and without
// the two-byte length a TAP file frames it with.
func tzxBlockData(payload []byte) []byte {
	return tapBlock(payload)[2:]
}

// writeTZX writes the given blocks out as a TZX image.
func writeTZX(t *testing.T, path string, blocks ...[]byte) {
	t.Helper()
	raw := tzxHeader()
	for _, b := range blocks {
		raw = append(raw, b...)
	}
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatalf("write tzx: %v", err)
	}
}

// tapAsTZX converts a TAP file into a TZX image in memory: every TAP block is a
// standard speed data block, and real tapes pause for a second between them.
func tapAsTZX(t *testing.T, tapFile string) []byte {
	t.Helper()
	raw, err := os.ReadFile(tapFile)
	if err != nil {
		t.Fatalf("read tap: %v", err)
	}

	out := tzxHeader()
	for len(raw) > 0 {
		if len(raw) < 2 {
			t.Fatalf("%s ends in the middle of a block length", tapFile)
		}
		length := int(binary.LittleEndian.Uint16(raw))
		raw = raw[2:]
		if length > len(raw) {
			t.Fatalf("%s says a block is %d bytes but only %d are left", tapFile, length, len(raw))
		}
		out = append(out, tzxStandardBlock(1000, raw[:length])...)
		raw = raw[length:]
	}
	return out
}

// ---------------------------------------------------------------------------
// Loading a TZX image the way a machine would
// ---------------------------------------------------------------------------

// loadTZX boots the ROM, types LOAD "" and runs until the instruction budget is
// spent, returning what ended up on the screen. It is loadTape for TZX images.
func loadTZX(t *testing.T, tape string, instructions int) string {
	t.Helper()
	emu := New(Config{
		TZXFile:      tape,
		SimKey:       "j\"\"\r",
		Headless:     true,
		Unpaced:      true,
		Audio:        false,
		Instructions: instructions,
	})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()
	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	return strings.Join(screenText(emu.mem[:]), "\n")
}

// TestTZXLoadsProgramFromGeneratedTape is the end-to-end check that a TZX image
// can be read by the ROM itself: boot the machine, type LOAD "" for it, and see
// the program it finds reported and then loaded.
func TestTZXLoadsProgramFromGeneratedTape(t *testing.T) {
	program := basicRem(10)
	tape := filepath.Join(t.TempDir(), "program.tzx")
	writeTZX(t, tape,
		tzxStandardBlock(1000, tzxBlockData(tapHeader(0, uint16(len(program)), 10, uint16(len(program)), "generated"))),
		tzxStandardBlock(1000, tzxBlockData(append([]byte{0xFF}, program...))),
	)

	screen := loadTZX(t, tape, 12_000_000)
	if !strings.Contains(screen, "generated") {
		t.Errorf("the ROM never reported the program on the tape; screen was:\n%s", screen)
	}
	if !strings.Contains(screen, "0 OK") {
		t.Errorf("the program did not load; screen was:\n%s", screen)
	}
}

// TestTZXTurboBlockLoadsProgram checks the same thing with the bytes in a turbo
// speed block, whose timings are in the file rather than implied by the format.
func TestTZXTurboBlockLoadsProgram(t *testing.T) {
	program := basicRem(10)
	tape := filepath.Join(t.TempDir(), "program.tzx")
	writeTZX(t, tape,
		tzxTurboBlock(1000, tzxBlockData(tapHeader(0, uint16(len(program)), 10, uint16(len(program)), "turbo")), 0x00),
		tzxTurboBlock(1000, tzxBlockData(append([]byte{0xFF}, program...)), 0xFF),
	)

	screen := loadTZX(t, tape, 12_000_000)
	if !strings.Contains(screen, "turbo") {
		t.Errorf("the ROM never reported the program on the tape; screen was:\n%s", screen)
	}
	if !strings.Contains(screen, "0 OK") {
		t.Errorf("the program did not load; screen was:\n%s", screen)
	}
}

// TestTZXLoadsARealTape converts a real commercial tape to TZX and loads it: a
// BASIC loader followed by two code blocks, after which the demo runs.
//
// The tape itself is not part of the repository, so this skips without it.
func TestTZXLoadsARealTape(t *testing.T) {
	if _, err := os.Stat(tapPath); err != nil {
		t.Skipf("tape image not available at %s: %v", tapPath, err)
	}
	tape := filepath.Join(t.TempDir(), "tasword.tzx")
	if err := os.WriteFile(tape, tapAsTZX(t, tapPath), 0o600); err != nil {
		t.Fatalf("write tzx: %v", err)
	}

	// The program clears the screen as soon as it runs, so its own text is the
	// evidence that the whole tape was read.
	screen := loadTZX(t, tape, 150_000_000)
	if !strings.Contains(screen, "Tasman Software") || !strings.Contains(screen, "Welcome!") {
		t.Errorf("the program on the tape never ran; screen was:\n%s", screen)
	}
}

// TestTZXRefusesBothTapeFlags checks that a machine is not given two tapes at
// once, which cannot be played at the same time.
func TestTZXRefusesBothTapeFlags(t *testing.T) {
	tape := filepath.Join(t.TempDir(), "empty.tzx")
	writeTZX(t, tape)

	emu := New(Config{TAPFile: "one.tap", TZXFile: tape, Headless: true, Audio: false})
	if err := emu.Init(); err == nil {
		emu.Close()
		t.Fatal("a machine was given both a TAP and a TZX file")
	}
}

// TestTZXReportsADamagedImage checks that a file which is not a TZX image is
// reported when it is opened rather than at the first read.
func TestTZXReportsADamagedImage(t *testing.T) {
	tape := filepath.Join(t.TempDir(), "not-a-tape.tzx")
	if err := os.WriteFile(tape, []byte("this is not a tape at all"), 0o600); err != nil {
		t.Fatalf("write: %v", err)
	}
	emu := New(Config{TZXFile: tape, Headless: true, Audio: false})
	defer emu.Close()
	if err := emu.Init(); err == nil {
		t.Error("a file that is not a TZX image was accepted")
	}
}

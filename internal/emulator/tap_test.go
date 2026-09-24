package emulator

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// tapPath is the cassette image kept beside the repository. It is not in the
// repository itself (see .gitignore), so tests that need it skip without it.
var tapPath = filepath.Join("..", "..", "z80s", "TASWDEMO.TAP")

// ---------------------------------------------------------------------------
// Building TAP files
//
// A TAP file is a sequence of blocks, each a two-byte little-endian length and
// that many bytes. The first byte of a block is its flag: 0x00 for a header,
// 0xFF for the data a header describes. The last byte is a checksum, chosen so
// that the exclusive-or of the whole block is zero.
// ---------------------------------------------------------------------------

// tapBlock frames a payload as a TAP block, checksum included.
func tapBlock(payload []byte) []byte {
	block := append([]byte(nil), payload...)
	block = append(block, checksum(block))
	out := make([]byte, 2, 2+len(block))
	binary.LittleEndian.PutUint16(out, uint16(len(block)))
	return append(out, block...)
}

// checksum returns the byte that makes the exclusive-or of the block zero.
func checksum(block []byte) byte {
	var sum byte
	for _, b := range block {
		sum ^= b
	}
	return sum
}

// tapHeader builds a header block payload describing the file that follows.
func tapHeader(kind byte, length, param1, param2 uint16, name string) []byte {
	h := make([]byte, 0, 19)
	h = append(h, 0x00, kind)
	nameBytes := make([]byte, 10)
	copy(nameBytes, name)
	h = append(h, nameBytes...)
	h = binary.LittleEndian.AppendUint16(h, length)
	h = binary.LittleEndian.AppendUint16(h, param1)
	h = binary.LittleEndian.AppendUint16(h, param2)
	return h
}

// writeProgramTape writes a tape holding one BASIC program, the way SAVE would.
func writeProgramTape(t *testing.T, path, name string, autostart uint16, program []byte) {
	t.Helper()
	var tape []byte
	tape = append(tape, tapBlock(tapHeader(0, uint16(len(program)), autostart, uint16(len(program)), name))...)
	tape = append(tape, tapBlock(append([]byte{0xFF}, program...))...)
	if err := os.WriteFile(path, tape, 0o600); err != nil {
		t.Fatalf("write tape: %v", err)
	}
}

// basicRem builds the smallest valid BASIC program: one REM line. A program is
// stored as lines of [number big-endian][length little-endian][tokens][0x0D],
// where the length counts the bytes that follow the four header bytes.
func basicRem(line uint16) []byte {
	body := []byte{0xEA, 0x0D} // REM, end of line
	out := make([]byte, 0, 4+len(body))
	out = binary.BigEndian.AppendUint16(out, line)
	out = binary.LittleEndian.AppendUint16(out, uint16(len(body)))
	return append(out, body...)
}

// ---------------------------------------------------------------------------
// Loading a tape the way a machine would
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Loading a tape the way a user would
// ---------------------------------------------------------------------------

// loadTape boots the ROM, types LOAD "" and runs until the instruction budget is
// spent, returning the emulator and what ended up on the screen.
//
// The quotes are typed out. Pressing J alone puts "LOAD " on the line and waits
// for a file name; a pair of quotes with nothing between them is what tells the
// ROM to take the next file on the tape.
func loadTape(t *testing.T, tape string, instructions int) (*Emulator, string) {
	t.Helper()
	return loadTapeWith(t, tape, instructions, false)
}

// loadTapeWith is loadTape with the choice of running the tape at real speed or
// winding it past as fast as the machine can go.
func loadTapeWith(t *testing.T, tape string, instructions int, quickLoad bool) (*Emulator, string) {
	t.Helper()
	emu := New(Config{
		TAPFile:      tape,
		QuickLoad:    quickLoad, // real speed, so the ROM reads the tape in emulated time
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
	return emu, strings.Join(screenText(emu.mem[:]), "\n")
}

// TestTapeLoadsProgramFromGeneratedTape is the end-to-end check that a TAP file
// can be loaded by the ROM itself: the machine is left to boot, LOAD "" is typed
// for it, and the tape - generated here, so the test needs nothing from outside
// the repository - has to be decoded well enough for the ROM to accept the
// header and report the program it found.
//
// Nothing short of correct pulse timing passes this. The ROM measures how long
// the EAR bit stays high, so a pilot tone of the wrong length, a missing sync
// pulse or a bit encoded with the wrong half-period leaves it waiting for a
// leader that never ends.
func TestTapeLoadsProgramFromGeneratedTape(t *testing.T) {
	tape := filepath.Join(t.TempDir(), "program.tap")
	writeProgramTape(t, tape, "generated", 10, basicRem(10))

	_, screen := loadTape(t, tape, 12_000_000)
	if !strings.Contains(screen, "generated") {
		t.Errorf("the ROM never reported the program on the tape; screen was:\n%s", screen)
	}
	if !strings.Contains(screen, "0 OK") {
		t.Errorf("the program did not load; screen was:\n%s", screen)
	}

	// The same tape has to load when the machine is allowed to wind it past at
	// full speed instead of playing it: only the pacing changes, not the signal.
	if _, screen := loadTapeWith(t, tape, 12_000_000, true); !strings.Contains(screen, "0 OK") {
		t.Errorf("the program did not load with the tape wound at full speed; screen was:\n%s", screen)
	}
}

// TestTapeLoadsProgramFromTASWDEMO loads the first file of a real tape, a BASIC
// loader followed by two code blocks, and checks that the ROM announces the
// program and finishes loading it.
//
// The tape is not part of the repository, so this skips without it.
func TestTapeLoadsProgramFromTASWDEMO(t *testing.T) {
	if _, err := os.Stat(tapPath); err != nil {
		t.Skipf("tape image not available at %s: %v", tapPath, err)
	}

	// The program clears the screen as soon as it runs, taking the ROM's own
	// "Bytes: tasword" line with it, so the program's text is the evidence here.
	_, screen := loadTape(t, tapPath, 150_000_000)
	// The tape carries a BASIC program followed by two code blocks, and the
	// program loads the rest of itself and then runs. Its own text being on the
	// screen is proof that the whole tape was read correctly.
	if !strings.Contains(screen, "Tasman Software") || !strings.Contains(screen, "Welcome!") {
		t.Errorf("the program on the tape never ran; screen was:\n%s", screen)
	}
}

// TestTapeWaitsForALoadBeforePlaying checks that a tape handed over at start-up
// is not played to nobody.
//
// The ROM reads the tape port from the moment it boots, for the keyboard, so a
// tape that starts on the first read of the port has played out before anyone
// can type LOAD "" - and then nothing can ever be loaded. It has to wait until
// the machine polls the port the way its loading routine does.
func TestTapeWaitsForALoadBeforePlaying(t *testing.T) {
	tape := filepath.Join(t.TempDir(), "program.tap")
	writeProgramTape(t, tape, "generated", 10, basicRem(10))

	// Enough emulated time for the tape to have played out several times over
	// had it started with the machine.
	emu := New(Config{
		TAPFile:      tape,
		Headless:     true,
		Unpaced:      true,
		Audio:        false,
		Instructions: 20_000_000,
	})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()
	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if emu.tapPlayer == nil {
		t.Fatal("no tape was attached")
	}
	if emu.tapPlayer.IsFinished() {
		t.Errorf("the tape played out after %d frames with no load asked for", emu.frameCount)
	}
}

// TestTapeLeavesNoTraceOfTheFileInMemory checks that the tape is not being
// written into memory behind the ROM's back. Loading is the ROM's job: a
// shortcut that copies a tape's blocks into memory cannot know where they
// belong, and writing them over whatever is at the start of RAM destroys the
// screen and the system variables.
func TestTapeIsNotCopiedIntoMemoryBlindly(t *testing.T) {
	tape := filepath.Join(t.TempDir(), "program.tap")
	writeProgramTape(t, tape, "generated", 10, basicRem(10))

	emu := New(Config{TAPFile: tape, QuickLoad: true, Headless: true, Unpaced: true, Audio: false})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	// The screen sits at 0x4000, which is where blocks used to be dumped.
	if got := emu.mem[0x4000:0x4010]; !bytes.Equal(got, make([]byte, 16)) {
		t.Errorf("the tape was written over the screen: % X", got)
	}
}

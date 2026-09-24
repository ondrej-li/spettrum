package tap

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// ---------------------------------------------------------------------------
// Building tapes
// ---------------------------------------------------------------------------

// block frames a payload as a TAP block: a two-byte length, the payload, and a
// checksum that makes the exclusive-or of the block zero.
func block(payload []byte) []byte {
	framed := append(append([]byte(nil), payload...), 0)
	for _, b := range framed[:len(framed)-1] {
		framed[len(framed)-1] ^= b
	}
	out := make([]byte, 2, 2+len(framed))
	binary.LittleEndian.PutUint16(out, uint16(len(framed)))
	return append(out, framed...)
}

// tapeOf writes the given blocks out as a TAP file and returns its path.
func tapeOf(t *testing.T, blocks ...[]byte) string {
	t.Helper()
	var tape []byte
	for _, b := range blocks {
		tape = append(tape, block(b)...)
	}
	path := filepath.Join(t.TempDir(), "test.tap")
	if err := os.WriteFile(path, tape, 0o600); err != nil {
		t.Fatalf("write tape: %v", err)
	}
	return path
}

// openTape writes a tape and opens a player on it.
func openTape(t *testing.T, blocks ...[]byte) *Player {
	t.Helper()
	p, err := NewPlayer(tapeOf(t, blocks...))
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	return p
}

// ---------------------------------------------------------------------------
// Reading tapes
// ---------------------------------------------------------------------------

// TestReadBlock checks the block framing, including the two ways a file can be
// damaged: a length that runs past the end of the file, and a truncated block.
func TestReadBlock(t *testing.T) {
	first := []byte{0x00, 0x03, 'a', 'b'}
	second := []byte{0xFF, 0x01, 0x02}
	tf, err := Open(tapeOf(t, first, second))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer tf.Close()

	// A block on the tape is its flag, its payload and a checksum, and that is
	// what the player has to produce, so that is what ReadBlock hands back.
	for i, want := range [][]byte{block(first)[2:], block(second)[2:]} {
		got, err := tf.ReadBlock()
		if err != nil {
			t.Fatalf("block %d: %v", i, err)
		}
		if string(got) != string(want) {
			t.Errorf("block %d is % X, want % X", i, got, want)
		}
	}
	if got, err := tf.ReadBlock(); err != nil || got != nil {
		t.Errorf("past the end: got % X, %v; want nothing", got, err)
	}

	// A block claiming more bytes than the file holds must be reported rather
	// than half-read: a damaged tape should not load as a short one.
	bad := append([]byte{0x10, 0x00}, []byte("fifteen bytes..")...)
	badPath := filepath.Join(t.TempDir(), "bad.tap")
	if err := os.WriteFile(badPath, bad, 0o600); err != nil {
		t.Fatalf("write tape: %v", err)
	}
	badFile, err := Open(badPath)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer badFile.Close()
	if _, err := badFile.ReadBlock(); err == nil {
		t.Error("a block of 16 bytes in a file holding 15 was accepted")
	}
}

// TestNewPlayerRejectsAnEmptyTape checks that a file with no blocks at all is
// refused at open, rather than leaving the machine waiting for a tape that will
// never play.
func TestNewPlayerRejectsAnEmptyTape(t *testing.T) {
	path := filepath.Join(t.TempDir(), "empty.tap")
	if err := os.WriteFile(path, nil, 0o600); err != nil {
		t.Fatalf("write tape: %v", err)
	}
	if _, err := NewPlayer(path); err == nil {
		t.Error("an empty tape was accepted")
	}
}

// ---------------------------------------------------------------------------
// The signal itself
// ---------------------------------------------------------------------------

// pulseLengths plays the tape up to the given cycle and returns the lengths of
// the pulses it produced, measured at one T-state resolution - which is how the
// ROM's loader measures them.
func pulseLengths(p *Player, until uint64) []int {
	return pulseLengthsFrom(p, 0, until)
}

// pulseLengthsFrom does the same, starting the measurement at the given cycle so
// that a tape picked up part way through the run is measured from there.
func pulseLengthsFrom(p *Player, from, until uint64) []int {
	var lengths []int
	level, start := -1, from
	for c := from; c <= until; c++ {
		l := p.ReadEAR(c)
		if level < 0 {
			level = l
			continue
		}
		if l != level {
			lengths = append(lengths, int(c-start))
			start, level = c, l
		}
	}
	return lengths
}

// TestTapeSignalFollowsTheStandard checks the pulse train against the cassette
// format, pulse for pulse: a leader tone, two sync pulses, then two pulses per
// bit - short for a 0 and exactly twice as long for a 1.
//
// This is the format a Spectrum's save routine writes and its load routine
// reads, so a signal that does not match it leaves the ROM waiting for a leader
// it never recognises and the tape never loads.
func TestTapeSignalFollowsTheStandard(t *testing.T) {
	for _, tc := range []struct {
		name  string
		flag  byte
		pilot int
	}{
		{"header block has the long leader", 0x00, pilotHeaderPulses},
		{"data block has the short leader", 0xFF, pilotDataPulses},
	} {
		t.Run(tc.name, func(t *testing.T) {
			// 0x85 is 1000 0101, so both bit lengths are exercised. tapeOf adds
			// the checksum that turns the payload into the block on the tape.
			payload := []byte{tc.flag, 0x85}
			content := block(payload)[2:]
			p := openTape(t, payload)

			total := tc.pilot + 2 + 2*8*len(content)
			until := uint64(tc.pilot)*pulsePilot + pulseSync1 + pulseSync2 +
				uint64(2*8*len(content))*pulseOne
			lengths := pulseLengths(p, until)

			// The tape ends by going quiet, so the closing edge of the last
			// pulse is the end of the tape and may not be seen at all: whether
			// the level it would have set differs from low decides that.
			if len(lengths) != total && len(lengths) != total-1 {
				t.Fatalf("the block produced %d pulses, want %d", len(lengths), total)
			}
			for i := range tc.pilot {
				if lengths[i] != pulsePilot {
					t.Fatalf("pilot pulse %d is %d T-states, want %d", i, lengths[i], pulsePilot)
				}
			}
			if lengths[tc.pilot] != pulseSync1 || lengths[tc.pilot+1] != pulseSync2 {
				t.Errorf("sync pulses are %d and %d T-states, want %d and %d",
					lengths[tc.pilot], lengths[tc.pilot+1], pulseSync1, pulseSync2)
			}
			for bit := range len(content) * 8 {
				want := pulseZero
				if content[bit/8]>>(7-uint(bit%8))&1 == 1 {
					want = pulseOne
				}
				first := tc.pilot + 2 + 2*bit
				if first+1 >= len(lengths) {
					// The closing edge of the very last pulse is the tape ending.
					break
				}
				if lengths[first] != want || lengths[first+1] != want {
					t.Fatalf("bit %d of % X is pulses %d and %d T-states, want two of %d",
						bit, content, lengths[first], lengths[first+1], want)
				}
			}
		})
	}
}

// TestTapeRunsEveryBlockInTurn checks that playback continues into the following
// block instead of stopping after the first one, which is what a tape holding a
// header and its data depends on.
func TestTapeRunsEveryBlockInTurn(t *testing.T) {
	firstPayload := []byte{0x00, 0x01}
	secondPayload := []byte{0xFF, 0x02}
	first := block(firstPayload)[2:]
	second := block(secondPayload)[2:]
	p := openTape(t, firstPayload, secondPayload)

	// Both blocks are headers or data according to their flag byte, and the
	// longest pulses are allowed for throughout.
	until := uint64(2*pilotHeaderPulses)*pulsePilot +
		uint64(2*8*(len(first)+len(second)))*pulseOne
	lengths := pulseLengths(p, until)

	total := (pilotHeaderPulses + 2 + 2*8*len(first)) + (pilotDataPulses + 2 + 2*8*len(second))
	if len(lengths) != total && len(lengths) != total-1 {
		t.Fatalf("the tape produced %d pulses, want %d", len(lengths), total)
	}
	if !p.IsFinished() {
		t.Error("the tape did not end after its last block")
	}
	// Past the end the input falls silent, so the ROM sees a level it can
	// recognise as "no tape" rather than a stuck bit.
	if level := p.ReadEAR(until + 1000); level != 0 {
		t.Errorf("after the tape ended the EAR bit is %d, want 0", level)
	}
}

// TestTapeStartsWhenTheMachineAsks checks that the leader starts when the ROM
// first reads the port rather than when the file was opened: a tape handed to
// the emulator at start-up may not be asked for until minutes later, by which
// time it would otherwise have played to nobody.
func TestTapeStartsWhenTheMachineAsks(t *testing.T) {
	p := openTape(t, []byte{0x00, 0x01})

	const late = 35_000_000 // ten seconds of emulated time
	// The signal starts low and turns high one pulse into the leader, which is
	// the polarity the ROM's own save routine writes.
	if level := p.ReadEAR(late); level != 0 {
		t.Fatalf("the first read returned %d, want the leader to start low", level)
	}
	// The leader is then a run of pilot-length pulses, the first of which starts
	// at the moment of that read.
	lengths := pulseLengthsFrom(p, late, late+6*pulsePilot)
	if p.IsFinished() {
		t.Fatal("the tape had finished before it was read")
	}
	if len(lengths) < 3 {
		t.Fatalf("only %d pulses after the tape was picked up", len(lengths))
	}
	for i, l := range lengths[:3] {
		if l != pulsePilot {
			t.Errorf("pulse %d after the start is %d T-states, want %d", i, l, pulsePilot)
		}
	}
}

// TestTapeCatchesUpWhenNobodyIsLooking checks that time spent not reading the
// tape does not lose pulses. The ROM does other work between reads, and a pulse
// it never saw is a bit it decodes wrongly.
func TestTapeCatchesUpWhenNobodyIsLooking(t *testing.T) {
	p := openTape(t, []byte{0x00, 0x01, 0x02, 0x03})

	p.ReadEAR(1000) // pick the tape up
	// Come back a thousand pilot pulses later, having read nothing in between.
	const missed = 1000
	late := uint64(1000 + pulsePilot/2 + missed*pulsePilot)
	p.ReadEAR(late)

	// The leader begins with one pulse, and every pulse since has to have been
	// played. Anything less means the tape fell behind the machine.
	if want := missed + 1; p.pulse < want-1 || p.pulse > want+1 {
		t.Errorf("played %d pulses while %d went by, want about %d", p.pulse, missed, want)
	}
}

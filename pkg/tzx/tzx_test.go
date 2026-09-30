package tzx

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// ---------------------------------------------------------------------------
// Building images
// ---------------------------------------------------------------------------

// blk builds one block byte by byte. Pinning the field layouts is what these
// tests are for, so they are written out one field at a time rather than hidden
// behind a struct the parser and the test could agree on while both being wrong.
type blk struct{ raw []byte }

func (w *blk) id(v byte) *blk       { w.raw = append(w.raw, v); return w }
func (w *blk) bytes(v ...byte) *blk { w.raw = append(w.raw, v...); return w }
func (w *blk) word(v int) *blk      { return w.bytes(byte(v), byte(v>>8)) }
func (w *blk) three(v int) *blk     { return w.bytes(byte(v), byte(v>>8), byte(v>>16)) }
func (w *blk) long(v int) *blk      { return w.bytes(byte(v), byte(v>>8), byte(v>>16), byte(v>>24)) }
func (w *blk) text8(s string) *blk  { return w.bytes(byte(len(s))).bytes([]byte(s)...) }
func (w *blk) build() []byte        { return w.raw }

// image wraps blocks in the header every TZX file starts with: the signature and
// the two version bytes.
func image(blocks ...[]byte) []byte {
	raw := append([]byte(signaturePrefix), 1, 20)
	for _, b := range blocks {
		raw = append(raw, b...)
	}
	return raw
}

// completeImage holds one of every block this package understands, with the
// values libspectrum's own test image uses.
func completeImage() [][]byte {
	return [][]byte{
		// 0x10 standard speed data: pause, length, bytes. The timings are not in
		// the file; the format fixes them.
		new(blk).id(idStandardSpeed).word(1000).word(4).bytes(0xff, 0xaa, 0xbb, 0xcc).build(),
		// 0x11 turbo speed data: every timing, then the leader pulse count, the
		// bits used in the last byte, the pause, a three-byte length, bytes.
		new(blk).id(idTurboSpeed).word(1000).word(123).word(456).word(789).word(400).
			word(5).bytes(8).word(0).three(4).bytes(0x00, 0xff, 0x55, 0xa0).build(),
		// 0x12 pure tone: one pulse length, repeated.
		new(blk).id(idPureTone).word(535).word(666).build(),
		// 0x13 pulse sequence: a count, then that many pulse lengths.
		new(blk).id(idPulseSequence).bytes(3).word(772).word(297).word(692).build(),
		// 0x14 pure data: bit pulse lengths, bits in the last byte, pause, a
		// three-byte length, bytes. No leader and no sync pulses.
		new(blk).id(idPureData).word(552).word(1639).bytes(6).word(554).three(3).
			bytes(0xff, 0x00, 0xfc).build(),
		// 0x15 direct recording: the sample period, pause, bits in the last
		// byte, a three-byte length, samples.
		new(blk).id(idDirectRecord).word(79).word(1000).bytes(8).three(2).bytes(0x3c, 0xf0).build(),
		// 0x20 pause, in milliseconds.
		new(blk).id(idPause).word(618).build(),
		// Metadata. It is read to keep the block numbering that jumps and loops
		// count in, and otherwise ignored.
		new(blk).id(idGroupStart).text8("Group Start").build(),
		new(blk).id(idGroupEnd).build(),
		new(blk).id(idText).text8("Comment here").build(),
		new(blk).id(idMessage).bytes(1).text8("A message").build(),
		new(blk).id(idArchiveInfo).word(15).
			bytes(0x00).text8("Full title").bytes(0x03).text8("Year").build(),
		new(blk).id(idHardware).bytes(2).bytes(0x00, 0x01, 0x02).bytes(0x01, 0x03, 0x04).build(),
		new(blk).id(idCustomInfo).bytes(make([]byte, 16)...).long(4).bytes('i', 'n', 'f', 'o').build(),
		new(blk).id(idGlue).long(4).bytes('g', 'l', 'u', 'e').build(),
		// Control flow. An offset is counted from the block holding it, and is
		// signed, so a call backwards is negative.
		new(blk).id(idJump).word(2).build(),
		new(blk).id(idLoopStart).word(3).build(),
		new(blk).id(idLoopEnd).build(),
		new(blk).id(idCall).word(-2).build(),
		new(blk).id(idReturn).build(),
		new(blk).id(idSelect).word(20).bytes(2).
			word(5).bytes([]byte("first")...).word(6).bytes([]byte("second")...).
			word(7).word(-7).build(),
		new(blk).id(idStopIf48K).bytes(0, 0, 0, 0).build(),
		new(blk).id(idSetLevel).bytes(1).build(),
	}
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// TestParseCompleteImage puts one of every supported block into a single image
// and checks that they come back with the right fields.
//
// Reading them all from one file is the point: a field read with the wrong width
// desynchronises everything after it, so the last block parsing at all is itself
// evidence that the ones before it were the size the format says they are.
func TestParseCompleteImage(t *testing.T) {
	blocks := completeImage()
	got, err := parse(image(blocks...))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(got) != len(blocks) {
		t.Fatalf("parsed %d blocks, want %d: a field was read with the wrong width",
			len(got), len(blocks))
	}

	// Standard speed data, whose timing comes from the format rather than the
	// file, and whose leader length comes from the flag byte.
	std := got[0]
	if std.id != idStandardSpeed || std.pauseMs != 1000 || len(std.data) != 4 {
		t.Errorf("standard speed block is %+v", std)
	}
	if std.pilot != TimingPilot || std.sync1 != TimingSync1 || std.sync2 != TimingSync2 ||
		std.zero != TimingZero || std.one != TimingOne {
		t.Errorf("standard speed block should hold the ROM's own timings, got %+v", std)
	}
	// A flag of 0xff is a data block, which has the shorter leader; the header
	// case is covered below.
	if std.pilotPulses != pilotDataPulses {
		t.Errorf("a block flagged 0xff got %d leader pulses, want the data count %d",
			std.pilotPulses, pilotDataPulses)
	}

	// Turbo speed data, where every timing is explicit.
	turbo := got[1]
	if turbo.pilot != 1000 || turbo.sync1 != 123 || turbo.sync2 != 456 ||
		turbo.zero != 789 || turbo.one != 400 || turbo.pilotPulses != 5 {
		t.Errorf("turbo block timings are %+v", turbo)
	}
	if turbo.bitsLast != 8 || len(turbo.data) != 4 {
		t.Errorf("turbo block has %d bytes with %d bits in the last, want 4 and 8",
			len(turbo.data), turbo.bitsLast)
	}

	if got[2].toneLength != 535 || got[2].toneCount != 666 {
		t.Errorf("pure tone is %+v", got[2])
	}
	if len(got[3].pulses) != 3 || got[3].pulses[0] != 772 || got[3].pulses[2] != 692 {
		t.Errorf("pulse sequence is %+v", got[3])
	}
	if got[4].zero != 552 || got[4].one != 1639 || got[4].bitsLast != 6 || got[4].pauseMs != 554 {
		t.Errorf("pure data is %+v", got[4])
	}
	if got[5].bitLength != 79 || got[5].pauseMs != 1000 || got[5].bitsLast != 8 {
		t.Errorf("direct recording is %+v", got[5])
	}
	if got[6].id != idPause || got[6].pauseMs != 618 {
		t.Errorf("pause is %+v", got[6])
	}

	// Control flow.
	if got[15].id != idJump || got[15].offset != 2 {
		t.Errorf("jump is %+v", got[15])
	}
	if got[16].id != idLoopStart || got[16].count != 3 {
		t.Errorf("loop start is %+v", got[16])
	}
	if got[18].id != idCall || got[18].offset != -2 {
		t.Errorf("call is %+v, want a negative offset", got[18])
	}
	if got[20].id != idSelect || got[20].offset != 7 {
		t.Errorf("select is %+v, want the first option's offset of 7", got[20])
	}
	if got[21].id != idStopIf48K {
		t.Errorf("block 21 is %#02x, want the stop block", got[21].id)
	}
	if got[22].id != idSetLevel {
		t.Errorf("block 22 is %#02x, want the set level block", got[22].id)
	}
}

// TestParseStandardSpeedLeaderFollowsTheFlag checks the one thing about a
// standard speed block that is not written down in the file.
func TestParseStandardSpeedLeaderFollowsTheFlag(t *testing.T) {
	for _, tc := range []struct {
		name string
		flag byte
		want int
	}{
		{"a header block", 0x00, pilotHeaderPulses},
		{"a data block", 0xff, pilotDataPulses},
		{"anything from 0x80 up is data", 0x80, pilotDataPulses},
		{"anything below it is a header", 0x7f, pilotHeaderPulses},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw := image(new(blk).id(idStandardSpeed).word(0).word(1).bytes(tc.flag).build())
			got, err := parse(raw)
			if err != nil {
				t.Fatalf("parse: %v", err)
			}
			if got[0].pilotPulses != tc.want {
				t.Errorf("flag %#02x got %d leader pulses, want %d",
					tc.flag, got[0].pilotPulses, tc.want)
			}
		})
	}
}

// TestParseNormalisesTheLastByteBits checks the convention that a file saying
// no bits are used in the last byte means a whole byte, which is what writers
// that never fill a partial byte leave behind.
func TestParseNormalisesTheLastByteBits(t *testing.T) {
	raw := image(new(blk).id(idPureData).word(855).word(1710).bytes(0).
		word(0).three(2).bytes(0xff, 0xff).build())
	got, err := parse(raw)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if got[0].bitsLast != 8 {
		t.Errorf("bits in the last byte is %d, want 8", got[0].bitsLast)
	}
	if n := bitsIn(got[0]); n != 16 {
		t.Errorf("the block carries %d bits, want 16", n)
	}
}

// TestParseRejectsDamagedImages checks that a file which is not a whole TZX
// image is refused, rather than read as far as it goes and then played.
func TestParseRejectsDamagedImages(t *testing.T) {
	good := image(new(blk).id(idStandardSpeed).word(0).word(1).bytes(0x00).build())

	for _, tc := range []struct {
		name string
		raw  []byte
		want string
	}{
		{"empty", nil, "too short"},
		{"header only", []byte(signaturePrefix), "too short"},
		{"not a TZX file", []byte("PK\x03\x04and then some"), "not a TZX image"},
		{"no blocks", image(), "no blocks"},
		{"truncated block", good[:len(good)-3], "file ends"},
		{"unknown block", image([]byte{0x99, 0, 0, 0}), "unknown block"},
		{"unsupported block", image([]byte{0x19, 0, 0, 0, 0}), "not supported"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := parse(tc.raw)
			if err == nil {
				t.Fatal("damaged image was accepted")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error is %q, want it to mention %q", err, tc.want)
			}
		})
	}
}

// TestParseRejectsASelectThatDoesNotAddUp checks that the length a select block
// states for itself is checked, since a wrong one would mean the parser is
// reading the wrong fields.
func TestParseRejectsASelectThatDoesNotAddUp(t *testing.T) {
	raw := image(new(blk).id(idSelect).word(99).bytes(1).
		word(5).bytes([]byte("first")...).word(0).build())
	if _, err := parse(raw); err == nil {
		t.Error("a select block whose length is wrong was accepted")
	}
}

// TestOpenReadsAFile covers the file-level entry point, since the tests above
// work on bytes already in memory.
func TestOpenReadsAFile(t *testing.T) {
	path := writeImage(t, image(new(blk).id(idPause).word(250).build()))
	blocks, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if len(blocks) != 1 || blocks[0].pauseMs != 250 {
		t.Errorf("read %+v, want a single 250ms pause", blocks)
	}
}

func TestOpenReportsAMissingFile(t *testing.T) {
	if _, err := Open("no-such-tape.tzx"); err == nil {
		t.Error("a missing file was accepted")
	}
}

// writeImage writes an image to a temporary file and returns its path.
func writeImage(t *testing.T, raw []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "image.tzx")
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatalf("write image: %v", err)
	}
	return path
}

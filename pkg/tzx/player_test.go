package tzx

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/defik74/spettrum/pkg/tap"
)

// pulseLengths plays the tape from the given cycle up to the given one and
// returns the length of every pulse it produced, measured at one T-state
// resolution, which is how the ROM's loader measures them.
func pulseLengths(p *Player, from, until uint64) []int {
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

// duration works out how long a block takes to play, so a test does not have to
// guess how far to drive the tape.
func duration(blocks []block) uint64 {
	var total uint64
	for _, b := range blocks {
		switch b.id {
		case idStandardSpeed, idTurboSpeed, idPureData:
			total += uint64(b.pilotPulses*b.pilot + b.sync1 + b.sync2)
			for i := 0; i < bitsIn(b); i++ {
				length := b.zero
				if bitAt(b, i) != 0 {
					length = b.one
				}
				total += uint64(2 * length)
			}
		case idPureTone:
			total += uint64(b.toneCount * b.toneLength)
		case idPulseSequence:
			for _, l := range b.pulses {
				total += uint64(l)
			}
		case idDirectRecord:
			total += uint64(bitsIn(b) * b.bitLength)
		}
		total += uint64(msToTStates(b.pauseMs))
	}
	return total
}

// sum is the length of every pulse in a list.
func sum(lengths []int) int {
	total := 0
	for _, l := range lengths {
		total += l
	}
	return total
}

// ---------------------------------------------------------------------------
// The signal
// ---------------------------------------------------------------------------

// TestStandardSpeedBlockFollowsTheStandard checks the pulse train of the format's
// commonest block against the cassette standard the ROM's loader expects: a
// leader of 2168 T-state pulses, the two sync pulses, then two pulses per bit,
// short for a 0 and twice as long for a 1, most significant bit first.
func TestStandardSpeedBlockFollowsTheStandard(t *testing.T) {
	// 0x85 is 1000 0101, so both bit lengths appear.
	content := []byte{0x00, 0x85, 0x85}
	blocks := []block{{
		id: idStandardSpeed, data: content, bitsLast: 8,
		pilot: TimingPilot, sync1: TimingSync1, sync2: TimingSync2,
		zero: TimingZero, one: TimingOne, pilotPulses: pilotHeaderPulses,
	}}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}

	lengths := pulseLengths(p, 0, duration(blocks))
	want := pilotHeaderPulses + 2 + 2*8*len(content)
	if len(lengths) != want && len(lengths) != want-1 {
		// The closing edge of the very last pulse is the tape ending, so whether
		// it is seen depends on the level it would have set.
		t.Fatalf("the block produced %d pulses, want %d", len(lengths), want)
	}
	for i := 0; i < pilotHeaderPulses; i++ {
		if lengths[i] != TimingPilot {
			t.Fatalf("leader pulse %d is %d T-states, want %d", i, lengths[i], TimingPilot)
		}
	}
	if lengths[pilotHeaderPulses] != TimingSync1 || lengths[pilotHeaderPulses+1] != TimingSync2 {
		t.Errorf("sync pulses are %d and %d T-states, want %d and %d",
			lengths[pilotHeaderPulses], lengths[pilotHeaderPulses+1], TimingSync1, TimingSync2)
	}
	for i := 0; i < bitsIn(blocks[0]); i++ {
		wantLen := TimingZero
		if bitAt(blocks[0], i) != 0 {
			wantLen = TimingOne
		}
		at := pilotHeaderPulses + 2 + 2*i
		if at+1 >= len(lengths) {
			break // the last pulse is cut short by the tape ending
		}
		if lengths[at] != wantLen || lengths[at+1] != wantLen {
			t.Fatalf("bit %d is pulses %d and %d T-states, want two of %d",
				i, lengths[at], lengths[at+1], wantLen)
		}
	}
}

// TestStandardSpeedLeaderLengthComesFromTheFlag checks that a block carrying a
// data flag byte gets the shorter leader, which is how the ROM tells the two
// kinds of block apart.
func TestStandardSpeedLeaderLengthComesFromTheFlag(t *testing.T) {
	for _, tc := range []struct {
		name  string
		flag  byte
		pilot int
	}{
		{"header", 0x00, pilotHeaderPulses},
		{"data", 0xff, pilotDataPulses},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw := image(new(blk).id(idStandardSpeed).word(0).word(2).bytes(tc.flag, 0x00).build())
			blocks, err := parse(raw)
			if err != nil {
				t.Fatalf("parse: %v", err)
			}
			p, err := NewPlayer(blocks)
			if err != nil {
				t.Fatalf("NewPlayer: %v", err)
			}
			lengths := pulseLengths(p, 0, duration(blocks))
			if len(lengths) < tc.pilot {
				t.Fatalf("only %d pulses", len(lengths))
			}
			for i := 0; i < tc.pilot; i++ {
				if lengths[i] != TimingPilot {
					t.Fatalf("pulse %d of the leader is %d T-states, want %d",
						i, lengths[i], TimingPilot)
				}
			}
		})
	}
}

// TestTurboBlockUsesItsOwnTimings checks that a turbo block is played with the
// timings in the file rather than the ROM's own: that is the whole point of a
// turbo loader, which sends the same bits in far less time.
func TestTurboBlockUsesItsOwnTimings(t *testing.T) {
	blocks := []block{{
		id: idTurboSpeed, data: []byte{0x00, 0xf0}, bitsLast: 8,
		pilot: 1000, sync1: 123, sync2: 456,
		zero: 789, one: 400, pilotPulses: 5,
	}}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}

	lengths := pulseLengths(p, 0, duration(blocks))
	wantPulses := 5 + 2 + 2*8*2
	if len(lengths) < wantPulses-1 {
		t.Fatalf("the block produced %d pulses, want %d", len(lengths), wantPulses)
	}
	for i := 0; i < 5; i++ {
		if lengths[i] != 1000 {
			t.Fatalf("leader pulse %d is %d T-states, want 1000", i, lengths[i])
		}
	}
	if lengths[5] != 123 || lengths[6] != 456 {
		t.Errorf("sync pulses are %d and %d T-states, want 123 and 456", lengths[5], lengths[6])
	}
	// The first bit is the flag's most significant bit, which is 0 here.
	if lengths[7] != 789 || lengths[8] != 789 {
		t.Errorf("first data pulses are %d and %d T-states, want two of 789", lengths[7], lengths[8])
	}
}

// TestPureToneAndPulseSequence checks the two blocks that describe raw signal
// rather than data.
func TestPureToneAndPulseSequence(t *testing.T) {
	blocks := []block{
		{id: idPureTone, toneLength: 535, toneCount: 4},
		{id: idPulseSequence, pulses: []int{100, 200, 300}},
	}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, duration(blocks))
	want := []int{535, 535, 535, 535, 100, 200, 300}
	if len(lengths) < len(want)-1 {
		t.Fatalf("produced %d pulses, want %d: %v", len(lengths), len(want), lengths)
	}
	for i, w := range want[:len(lengths)] {
		if lengths[i] != w {
			t.Errorf("pulse %d is %d T-states, want %d (%v)", i, lengths[i], w, lengths)
		}
	}
}

// TestDirectRecordingHoldsTheLevelBetweenChanges checks that a run of identical
// samples is one pulse rather than a pulse per sample: the real signal only
// changes where the samples do.
func TestDirectRecordingHoldsTheLevelBetweenChanges(t *testing.T) {
	// 0x0f is 00001111: a run of four zeroes, then a run of four ones.
	blocks := []block{{
		id: idDirectRecord, data: []byte{0x0f}, bitsLast: 8, bitLength: 100,
	}}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, duration(blocks))
	if len(lengths) == 0 {
		t.Fatal("direct recording produced no pulses")
	}
	if lengths[0] != 400 {
		t.Errorf("the first run is %d T-states, want 400 (four samples of 100)", lengths[0])
	}
	if len(lengths) > 1 && lengths[1] != 400 {
		t.Errorf("the second run is %d T-states, want 400", lengths[1])
	}
}

// TestPauseHoldsTheLevel checks that a pause is played as silence of the right
// length: the level held, with no edges inside it.
func TestPauseHoldsTheLevel(t *testing.T) {
	blocks := []block{
		{id: idPureTone, toneLength: 1000, toneCount: 2},
		{id: idPause, pauseMs: 1000},
		{id: idPureTone, toneLength: 2000, toneCount: 2},
	}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, duration(blocks))
	if len(lengths) < 4 {
		t.Fatalf("produced %d pulses: %v", len(lengths), lengths)
	}
	// The tone's two pulses, then the whole second of silence, then the tone.
	if lengths[0] != 1000 || lengths[1] != 1000 {
		t.Errorf("the first tone is %v", lengths[:2])
	}
	if lengths[2] != 1000*3500 {
		t.Errorf("the pause is %d T-states, want %d", lengths[2], 1000*3500)
	}
	if lengths[3] != 2000 {
		t.Errorf("the tone after the pause is %d T-states, want 2000", lengths[3])
	}
}

// TestLoopRepeatsTheSection checks the loop blocks, which is how a tape holds a
// section that is played more than once.
func TestLoopRepeatsTheSection(t *testing.T) {
	blocks := []block{
		{id: idLoopStart, count: 3},
		{id: idPureTone, toneLength: 100, toneCount: 1},
		{id: idLoopEnd},
		{id: idPureTone, toneLength: 500, toneCount: 1},
	}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, 10000)
	want := []int{100, 100, 100, 500}
	if len(lengths) < len(want) {
		t.Fatalf("produced %v, want %v", lengths, want)
	}
	for i, w := range want {
		if lengths[i] != w {
			t.Errorf("pulse %d is %d, want %d (%v)", i, lengths[i], w, lengths)
		}
	}
}

// TestJumpSkipsBlocks checks that a jump moves playback, so a tape can send the
// machine round a section again or past one it does not want.
func TestJumpSkipsBlocks(t *testing.T) {
	blocks := []block{
		{id: idJump, offset: 2}, // over the block after it, to the tone
		{id: idPureTone, toneLength: 111, toneCount: 1},
		{id: idPureTone, toneLength: 222, toneCount: 2},
	}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, 10000)
	if len(lengths) == 0 || lengths[0] != 222 {
		t.Errorf("the jump played %v, want the second tone", lengths)
	}
}

// TestStopIf48KEndsTheTape checks that a tape which says to stop on a 48K machine
// does stop: that is how a multi-load tape hands over to the program it just
// loaded.
func TestStopIf48KEndsTheTape(t *testing.T) {
	blocks := []block{
		{id: idPureTone, toneLength: 100, toneCount: 1},
		{id: idStopIf48K},
		{id: idPureTone, toneLength: 200, toneCount: 1},
	}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	lengths := pulseLengths(p, 0, 10000)
	if len(lengths) > 0 && lengths[0] != 100 {
		t.Errorf("played %v, want the tone before the stop block", lengths)
	}
	if !p.IsFinished() {
		t.Error("the tape did not finish at the stop block")
	}
	// Past the end the line is quiet, so the ROM sees "no tape" rather than a
	// level that never changes.
	if level := p.ReadEAR(10000); level != 0 {
		t.Errorf("after the tape stopped the EAR bit is %d, want 0", level)
	}
}

// TestTapeStartsWhenTheMachineAsks checks that nothing is played until the
// machine reads the port, and that the leader then starts from low.
func TestTapeStartsWhenTheMachineAsks(t *testing.T) {
	blocks := []block{{
		id: idStandardSpeed, data: []byte{0x00, 0x01}, bitsLast: 8,
		pilot: TimingPilot, sync1: TimingSync1, sync2: TimingSync2,
		zero: TimingZero, one: TimingOne, pilotPulses: pilotHeaderPulses,
	}}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}

	const late = 35_000_000 // ten seconds of emulated time
	if p.IsFinished() {
		t.Fatal("the tape had finished before it was read")
	}
	if level := p.ReadEAR(late); level != 0 {
		t.Errorf("the first read returned %d, want the leader to start low", level)
	}
	// The leader then runs from the moment of that read.
	lengths := pulseLengths(p, late, late+3*TimingPilot)
	if len(lengths) < 3 {
		t.Fatalf("only %d pulses after the tape was picked up", len(lengths))
	}
	for i, l := range lengths[:3] {
		if l != TimingPilot {
			t.Errorf("pulse %d after the start is %d T-states, want %d", i, l, TimingPilot)
		}
	}
}

// TestControlBlocksInACircleDoNotHang checks the guard against a file whose
// jumps lead in a circle: the emulator's main loop calls into the player, so a
// tape that never ends would take the whole machine with it.
func TestControlBlocksInACircleDoNotHang(t *testing.T) {
	blocks := []block{{id: idJump, offset: 0}}
	p, err := NewPlayer(blocks)
	if err != nil {
		t.Fatalf("NewPlayer: %v", err)
	}
	if level := p.ReadEAR(1_000_000); level != 0 {
		t.Errorf("a tape that jumps to itself returned %d, want silence", level)
	}
	if !p.IsFinished() {
		t.Error("a tape that jumps to itself was not given up on")
	}
}

// TestStandardSpeedMatchesTheTapPlayer plays the same block through both
// players and requires the same signal.
//
// A TAP file is a standard speed data block without the pause, so this is the
// cross-check between the two: two independent pieces of code, and the format
// documentation each was written from, have to agree pulse for pulse.
func TestStandardSpeedMatchesTheTapPlayer(t *testing.T) {
	content := []byte{0x00, 0x0a, 0x02, 0x00, 0xea, 0x0d}
	var parity byte
	for _, b := range content {
		parity ^= b
	}
	content = append(content, parity)

	// The same bytes as a TAP file: a two-byte length and then the block.
	tapPath := filepath.Join(t.TempDir(), "block.tap")
	raw := []byte{byte(len(content)), byte(len(content) >> 8)}
	if err := os.WriteFile(tapPath, append(raw, content...), 0o600); err != nil {
		t.Fatalf("write tap: %v", err)
	}
	tzxPath := writeImage(t, image(new(blk).id(idStandardSpeed).word(0).
		word(len(content)).bytes(content...).build()))

	tapPlayer, err := tap.NewPlayer(tapPath)
	if err != nil {
		t.Fatalf("tap.NewPlayer: %v", err)
	}
	tzxPlayer, err := OpenPlayer(tzxPath)
	if err != nil {
		t.Fatalf("OpenPlayer: %v", err)
	}

	// Long enough for the whole block: the leader alone is 8063 pulses of 2168
	// T-states.
	const until = 24_000_000
	fromTap := tapPulseLengths(tapPlayer, 0, until)
	fromTzx := pulseLengths(tzxPlayer, 0, until)

	if len(fromTap) == 0 || len(fromTzx) == 0 {
		t.Fatal("one of the players produced nothing")
	}
	if len(fromTap) != len(fromTzx) {
		t.Fatalf("the TAP player produced %d pulses and the TZX player %d",
			len(fromTap), len(fromTzx))
	}
	for i := range fromTap {
		if fromTap[i] != fromTzx[i] {
			t.Fatalf("pulse %d is %d T-states from the TAP file and %d from the TZX file",
				i, fromTap[i], fromTzx[i])
		}
	}
	if !tapPlayer.IsFinished() || !tzxPlayer.IsFinished() {
		t.Errorf("a tape did not finish: tap=%v tzx=%v",
			tapPlayer.IsFinished(), tzxPlayer.IsFinished())
	}
}

// tapPulseLengths is pulseLengths for the TAP player, which this package
// otherwise knows nothing about.
func tapPulseLengths(p *tap.Player, from, until uint64) []int {
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

package tzx

import "errors"

// errNoBlocks is returned for an image that holds no blocks at all, which is a
// file that would leave the machine waiting for a tape that never plays.
var errNoBlocks = errors.New("tzx: image holds no blocks")

// maxSilentSteps bounds how far the control flow may walk without producing a
// pulse. A tape may legitimately hold a run of comments and group markers, but a
// file whose jumps lead in a circle with nothing in between would otherwise spin
// here for ever.
const maxSilentSteps = 512

// Player presents a TZX image as the signal a cassette player's output would
// have had on the EAR line, and moves the tape on as the machine reads it.
//
// The signal is a train of pulses, each the time the level is held before it
// changes: a leader pulse of 2168 T-states is the level held for 2168 T-states,
// then the opposite level for the next pulse, and so on. That is what the ULA
// sees and what the ROM's loader measures.
//
// Nothing is played until the machine first reads the port: a tape handed to the
// emulator at start-up may not be asked for until long afterwards, and the
// leader has to begin when something is listening for it, not before.
type Player struct {
	blocks []block

	pc       int // the block being played
	cur      cursor
	loops    []loopFrame
	calls    []int
	level    int
	nextEdge uint64
	started  bool
	finished bool
	paused   bool // the current block's pause has been paid
}

// loopFrame remembers where a loop began and how many more times to take it.
type loopFrame struct {
	pc        int
	remaining int
}

// OpenPlayer reads a TZX image and returns a player over it.
func OpenPlayer(path string) (*Player, error) {
	blocks, err := Open(path)
	if err != nil {
		return nil, err
	}
	return NewPlayer(blocks)
}

// NewPlayer starts a player over blocks that have already been read.
func NewPlayer(blocks []block) (*Player, error) {
	if len(blocks) == 0 {
		return nil, errNoBlocks
	}
	// The block index starts before the first block: the first pulse asks for
	// the next one, which is block zero.
	return &Player{blocks: blocks, pc: -1}, nil
}

// ReadEAR returns the level of the EAR bit (bit 6 of port 0xFE) at the given CPU
// cycle, moving the tape on as far as that cycle.
func (p *Player) ReadEAR(cpuCycles uint64) int {
	if p.finished {
		return 0
	}
	if !p.started {
		// The leader begins at the first read, from low: the ROM's own save
		// routine writes its leader as a low pulse first, and the loader waits
		// for that first change.
		p.started = true
		p.level = 0
		length, ok := p.takePulse()
		if !ok {
			p.finished = true
			return 0
		}
		p.nextEdge = cpuCycles + uint64(length)
	}

	// A caller that has been away - the ROM doing something other than reading
	// the tape - must not lose pulses, so every edge the elapsed time contains
	// is applied, however many there are.
	for cpuCycles >= p.nextEdge {
		p.level ^= 1
		length, ok := p.takePulse()
		if !ok {
			// The tape runs out into silence, which is a level the ROM reads as
			// "no tape" rather than as a stuck bit.
			p.finished = true
			p.level = 0
			return 0
		}
		p.nextEdge += uint64(length)
	}
	return p.level
}

// IsFinished reports whether the whole tape has been played.
func (p *Player) IsFinished() bool { return p.finished }

// takePulse returns the length of the next pulse in T-states, asking the block
// the control flow leads to whenever the current one is spent. It reports false
// at the end of the tape.
func (p *Player) takePulse() (int, bool) {
	for steps := 0; steps < maxSilentSteps; steps++ {
		if p.cur != nil {
			if length, ok := p.cur.next(p.blocks[p.pc]); ok {
				return length, true
			}
			// The block is spent: pay its pause once, then move on.
			if b := p.blocks[p.pc]; !p.paused && b.pauseMs > 0 {
				p.paused = true
				p.cur = &pauseCursor{left: 1, length: msToTStates(b.pauseMs)}
				continue
			}
		}
		if !p.nextBlock() {
			return 0, false
		}
	}
	// Nothing but control blocks in a circle: better to stop than to hang.
	p.finished = true
	return 0, false
}

// nextBlock moves on from the block that has just finished, or has just been
// stepped over, and reports whether there is a block ready to play. The blocks
// it passes on the way - loops, jumps, calls, comments - are what decide where
// "on" is.
func (p *Player) nextBlock() bool {
	next := p.pc + 1
	for steps := 0; steps < maxSilentSteps; steps++ {
		p.pc = next
		if p.pc < 0 || p.pc >= len(p.blocks) {
			p.finished = true
			return false
		}
		b := p.blocks[p.pc]
		next = p.pc + 1
		p.paused = false

		switch b.id {
		case idLoopStart:
			// The count is how many times the section that follows is played;
			// zero would mean a loop that never ends, so it is treated as one.
			count := b.count
			if count < 1 {
				count = 1
			}
			p.loops = append(p.loops, loopFrame{pc: p.pc + 1, remaining: count})

		case idLoopEnd:
			if n := len(p.loops); n > 0 {
				f := &p.loops[n-1]
				f.remaining--
				if f.remaining > 0 {
					next = f.pc
				} else {
					p.loops = p.loops[:n-1]
				}
			}

		case idJump:
			// The offset is counted from the block holding the jump.
			next = p.pc + b.offset

		case idCall:
			p.calls = append(p.calls, p.pc+1)
			next = p.pc + b.offset

		case idReturn:
			if n := len(p.calls); n > 0 {
				next = p.calls[n-1]
				p.calls = p.calls[:n-1]
			}

		case idSelect:
			// Only the first option is ever taken: there is nobody to ask which
			// one to load, and a tape that offers a choice puts the thing a
			// plain LOAD "" wants first.
			next = p.pc + b.offset

		case idStopIf48K:
			// The machine being modelled is a 48K one, so the tape stops here.
			// That is what a multi-load tape means by it: the rest is loaded by
			// the program that has just started.
			p.finished = true
			return false

		default:
			// Either a block with signal to play, or one to step over.
			p.cur = newCursor(b)
			if b.signal() {
				return true
			}
		}
	}
	// Nothing but control blocks in a circle: better to stop than to hang.
	p.finished = true
	return false
}

// ---------------------------------------------------------------------------
// Turning blocks into pulses
// ---------------------------------------------------------------------------

// cursor hands out the pulses of one block, one at a time, and reports when the
// block has no more to give.
type cursor interface {
	next(b block) (int, bool)
}

// newCursor returns the cursor for a block. Blocks that carry no signal get one
// that is spent from the start.
func newCursor(b block) cursor {
	switch b.id {
	case idStandardSpeed, idTurboSpeed, idPureData:
		return &dataCursor{}
	case idDirectRecord:
		return &recordCursor{}
	case idPureTone:
		return &toneCursor{left: b.toneCount, length: b.toneLength}
	case idPulseSequence:
		return &sequenceCursor{pulses: b.pulses}
	}
	return spent{}
}

// spent is a block with nothing to play.
type spent struct{}

func (spent) next(block) (int, bool) { return 0, false }

// pauseCursor holds one level for a while. Silence is the level not changing, so
// a pause is a single interval with no edge inside it.
type pauseCursor struct {
	left   int
	length int
}

func (c *pauseCursor) next(block) (int, bool) {
	if c.left <= 0 {
		return 0, false
	}
	c.left--
	return c.length, true
}

// toneCursor plays one pulse length a given number of times.
type toneCursor struct {
	left   int
	length int
}

func (c *toneCursor) next(block) (int, bool) {
	if c.left <= 0 {
		return 0, false
	}
	c.left--
	return c.length, true
}

// sequenceCursor plays a list of pulse lengths, once each.
type sequenceCursor struct {
	pulses []int
	at     int
}

func (c *sequenceCursor) next(block) (int, bool) {
	if c.at >= len(c.pulses) {
		return 0, false
	}
	l := c.pulses[c.at]
	c.at++
	return l, true
}

// dataCursor walks a block of bits: leader, two sync pulses, then every bit of
// every byte, most significant bit first. A bit is two pulses of the same
// length, which is what gives the ROM's loader the bit period it measures.
type dataCursor struct {
	stage     int // 0 leader, 1 sync1, 2 sync2, 3 bits
	pilotLeft int
	bit       int // index of the next bit within the block
	half      int // which of the bit's two pulses comes next
	loaded    bool
}

func (c *dataCursor) next(b block) (int, bool) {
	if !c.loaded {
		c.loaded = true
		c.pilotLeft = b.pilotPulses
	}
	for {
		switch c.stage {
		case 0:
			if c.pilotLeft > 0 {
				c.pilotLeft--
				return b.pilot, true
			}
			c.stage++

		case 1:
			c.stage++
			if b.sync1 > 0 {
				return b.sync1, true
			}

		case 2:
			c.stage++
			if b.sync2 > 0 {
				return b.sync2, true
			}

		case 3:
			if c.bit >= bitsIn(b) {
				c.stage++
				continue
			}
			length := b.zero
			if bitAt(b, c.bit) != 0 {
				length = b.one
			}
			if c.half == 0 {
				c.half = 1
			} else {
				c.half = 0
				c.bit++
			}
			return length, true

		default:
			return 0, false
		}
	}
}

// recordCursor plays direct recording, which stores one sample per bit period
// rather than an encoding. Runs of the same sample are the level being held, so
// each run is one pulse: that is where the real signal would have changed and
// nowhere else.
type recordCursor struct {
	bit    int
	left   int // the run in progress, if it has not been handed out yet
	length int
}

func (c *recordCursor) next(b block) (int, bool) {
	for {
		if c.left > 0 {
			c.left--
			return c.length, true
		}
		if c.bit >= bitsIn(b) {
			return 0, false
		}
		// Measure the run of identical samples starting here. Runs alternate by
		// construction, so each one is a change of level.
		level := bitAt(b, c.bit)
		run := 0
		for c.bit+run < bitsIn(b) && bitAt(b, c.bit+run) == level {
			run++
		}
		c.bit += run
		c.left = 1
		c.length = run * b.bitLength
	}
}

// bitsIn is how many bits a data block carries: eight for every whole byte, and
// only as many as the file says for the last one.
func bitsIn(b block) int {
	if len(b.data) == 0 {
		return 0
	}
	last := b.bitsLast
	if last < 1 || last > 8 {
		last = 8
	}
	return (len(b.data)-1)*8 + last
}

// bitAt returns bit i of the block, most significant bit first.
func bitAt(b block, i int) int {
	if i < 0 || i >= len(b.data)*8 {
		return 0
	}
	return int(b.data[i/8]>>(7-uint(i%8))) & 1
}

// msToTStates converts the milliseconds a pause is given in into the T-states
// the tape is timed in: the machine's clock is 3.5MHz, so a millisecond is 3500
// of them.
func msToTStates(ms int) int {
	return ms * 3500
}

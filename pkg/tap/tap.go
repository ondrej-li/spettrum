// Package tap provides ZX Spectrum TAP cassette file loading.
//
// A TAP file is a sequence of blocks, each stored as a two-byte little-endian
// length followed by that many bytes. The first byte of a block is its flag:
// 0x00 for a header, which describes the file that follows, and 0xFF for the
// data itself. The last byte is a checksum picked so that the exclusive-or of
// the whole block is zero.
//
// Loading works the way it does on the real machine. Nothing here decodes the
// tape: Player replays the pulses a cassette would produce and the ROM's own
// tape routine reads them through the ULA's EAR bit (port 0xFE bit 6). That is
// what makes multi-part tapes work at all - a game whose BASIC loader decides
// for itself what to load next, and where to put it, is followed by the ROM just
// as it is on real hardware.
package tap

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
)

// ---------------------------------------------------------------------------
// Tape timing
//
// Pulse lengths are in T-states at 3.5MHz. A block opens with a leader tone, two
// sync pulses, and then two pulses per data bit; the ROM times how long the EAR
// bit stays high, so a 1 takes exactly twice as long as a 0.
// ---------------------------------------------------------------------------

const (
	// pulsePilot is one pulse of the leader tone.
	pulsePilot = 2168
	// pulseSync1 and pulseSync2 mark the end of the leader. Both are shorter
	// than any data pulse, which is how the ROM recognises them.
	pulseSync1 = 667
	pulseSync2 = 735
	// Each data bit is two pulses of the same length: two short ones for a 0,
	// two long ones for a 1.
	pulseZero = 855
	pulseOne  = 1710

	// A header block is preceded by a long leader, the data block that follows
	// it by a short one.
	pilotHeaderPulses = 8063
	pilotDataPulses   = 3223

	// flagHeader and flagData are the first byte of a block.
	flagHeader = 0x00
	flagData   = 0xFF
)

// ---------------------------------------------------------------------------
// File format
// ---------------------------------------------------------------------------

// File reads the blocks of a TAP file in order.
type File struct {
	f *os.File
}

// Open opens a TAP file for reading.
func Open(path string) (*File, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	return &File{f: f}, nil
}

// Close closes the file.
func (tf *File) Close() error {
	return tf.f.Close()
}

// ReadBlock reads the next block. It returns nil at the end of the file, and an
// error if the file is damaged: a block that claims to be longer than the file
// holds is reported rather than half-read.
func (tf *File) ReadBlock() ([]byte, error) {
	var length [2]byte
	if _, err := io.ReadFull(tf.f, length[:]); err != nil {
		if errors.Is(err, io.EOF) {
			return nil, nil // clean end of file
		}
		return nil, fmt.Errorf("read block length: %w", err)
	}

	n := int(binary.LittleEndian.Uint16(length[:]))
	block := make([]byte, n)
	if _, err := io.ReadFull(tf.f, block); err != nil {
		return nil, fmt.Errorf("read %d byte block: %w", n, err)
	}
	return block, nil
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

// Player replays a TAP file as a cassette signal.
//
// It holds no timing of its own: the caller asks for the EAR level at the CPU
// cycle it has reached, and the tape moves on to whatever pulse that cycle falls
// in. The signal is therefore a function of emulated time, so a tape loads at
// exactly the speed the machine is running at - the authentic minutes on a paced
// run, or seconds when the emulator is running flat out.
type Player struct {
	// blocks are the tape's blocks, in order, each with its flag byte first.
	blocks [][]byte
	// block is the index of the block being played.
	block int

	// pilot is the number of leader pulses for the current block, and pulses the
	// total number of pulses it is made of.
	pilot  int
	pulses int

	// pulse is the index of the pulse that starts at nextEdge, and level the
	// state of the EAR bit until then.
	pulse    int
	level    int
	nextEdge uint64
	started  bool
}

// NewPlayer opens a TAP file and prepares it for playback. The whole tape is
// read up front so that a damaged file is reported here rather than part way
// through a load.
func NewPlayer(path string) (*Player, error) {
	tf, err := Open(path)
	if err != nil {
		return nil, err
	}
	defer tf.Close()

	var blocks [][]byte
	for {
		block, err := tf.ReadBlock()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
		if block == nil {
			break
		}
		if len(block) == 0 {
			continue // no flag byte, so no signal to play
		}
		blocks = append(blocks, block)
	}
	if len(blocks) == 0 {
		return nil, fmt.Errorf("%s: no tape blocks", path)
	}

	p := &Player{blocks: blocks}
	p.selectBlock(0)
	return p, nil
}

// selectBlock prepares the block at index i for playback.
func (p *Player) selectBlock(i int) {
	p.block = i
	p.pilot = pilotDataPulses
	if p.blocks[i][0] == flagHeader {
		p.pilot = pilotHeaderPulses
	}
	p.pulses = p.pilot + 2 + 2*8*len(p.blocks[i])
	p.pulse = 0
}

// ReadEAR returns the level of the EAR bit (bit 6 of port 0xFE) at the given CPU
// cycle, moving the tape on as far as that cycle.
//
// The tape only starts moving when the machine first reads the port: the load
// command may be typed long after the file was handed to the emulator, and the
// leader has to begin when the ROM starts listening for it, not before.
func (p *Player) ReadEAR(cpuCycles uint64) int {
	if p.finished() {
		return 0
	}
	if !p.started {
		p.started = true
		p.level = 0
		p.nextEdge = cpuCycles + uint64(p.pulseLen(0))
		p.pulse = 1
	}

	// A caller that has been away for a while - the ROM doing something other
	// than reading the tape - must not lose pulses, so this catches up with as
	// many edges as the elapsed time contains.
	for cpuCycles >= p.nextEdge {
		p.level ^= 1
		if p.pulse >= p.pulses {
			if !p.nextBlock() {
				p.level = 0
				return 0
			}
		}
		p.nextEdge += uint64(p.pulseLen(p.pulse))
		p.pulse++
	}
	return p.level
}

// IsFinished reports whether the whole tape has been played.
func (p *Player) IsFinished() bool {
	return p.finished()
}

func (p *Player) finished() bool {
	return p.block >= len(p.blocks)
}

// nextBlock moves to the following block, or reports the end of the tape.
func (p *Player) nextBlock() bool {
	if p.block+1 >= len(p.blocks) {
		p.block = len(p.blocks)
		return false
	}
	p.selectBlock(p.block + 1)
	return true
}

// pulseLen returns the length of the pulse at the given index within the current
// block. A block is a leader tone, the two sync pulses, and then two pulses for
// every bit of every byte, most significant bit first.
func (p *Player) pulseLen(i int) int {
	if i < p.pilot {
		return pulsePilot
	}
	switch i - p.pilot {
	case 0:
		return pulseSync1
	case 1:
		return pulseSync2
	}
	if p.bitAt((i-p.pilot-2)/2) == 0 {
		return pulseZero
	}
	return pulseOne
}

// bitAt returns bit i of the current block, most significant bit first.
func (p *Player) bitAt(i int) int {
	block := p.blocks[p.block]
	if i < 0 || i >= len(block)*8 {
		return 0
	}
	return int(block[i/8]>>(7-uint(i%8))) & 1
}

// Package tzx reads TZX tape images and plays them as the signal a cassette
// would have carried.
//
// TZX is the container the Spectrum world settled on after TAP. Where a TAP file
// can only describe what the ROM's own save routine writes - a leader, two sync
// pulses and eight bits a byte - a TZX file is a list of blocks, each introduced
// by an identifier byte, and blocks may carry their own timings (turbo loaders),
// raw tones, direct recording, or the control flow (loops, jumps, calls) that
// multi-load games are built from.
//
// Only the blocks that describe signal or control playback are kept; the
// descriptive ones (text, archive info, hardware type) are parsed to keep the
// block numbering that jumps and loops refer to, but produce nothing.
package tzx

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
)

// signaturePrefix is the eight bytes every TZX file starts with. Two version
// bytes follow it.
const signaturePrefix = "ZXTape!\x1a"

// Block identifiers.
const (
	idStandardSpeed = 0x10 // the ROM's own format: leader, sync, then eight bits a byte
	idTurboSpeed    = 0x11 // the same, with every timing given explicitly
	idPureTone      = 0x12 // a run of identical pulses
	idPulseSequence = 0x13 // a run of explicit pulse lengths
	idPureData      = 0x14 // bits with given pulse lengths, no leader or sync
	idDirectRecord  = 0x15 // one sample per bit period, as a real player would record it
	idPause         = 0x20 // silence

	idGroupStart  = 0x21
	idGroupEnd    = 0x22
	idJump        = 0x23
	idLoopStart   = 0x24
	idLoopEnd     = 0x25
	idCall        = 0x26
	idReturn      = 0x27
	idSelect      = 0x28
	idStopIf48K   = 0x2a
	idSetLevel    = 0x2b // forces the level; read, but it has nothing to play
	idText        = 0x30
	idMessage     = 0x31
	idArchiveInfo = 0x32
	idHardware    = 0x33
	idCustomInfo  = 0x35
	idGlue        = 0x5a
)

// The timings of the ROM's own loader, in T-states. These are what a standard
// speed data block implies, and what a TAP file always uses.
const (
	TimingPilot = 2168
	TimingSync1 = 667
	TimingSync2 = 735
	TimingZero  = 855
	TimingOne   = 1710

	// The leader of a header block is longer than that of a data block, which
	// is how the ROM's loader can tell them apart before it has read a byte.
	pilotHeaderPulses = 8063
	pilotDataPulses   = 3223
)

// block is one parsed TZX block. Only the fields the block's identifier uses are
// set; a block with no signal (a comment, or a group marker) has none of them.
type block struct {
	id byte

	// Data blocks (0x10, 0x11, 0x14, 0x15): the bytes on the tape, which for a
	// standard speed block are the flag, the payload and the checksum, exactly
	// as they appear in a TAP file.
	data     []byte
	bitsLast int // bits used in the last byte; a file that says 0 means 8

	// Timings for the bit stream, in T-states per pulse. A bit is two pulses
	// of the same length, so a 0 bit occupies 2*zero T-states.
	pilot, sync1, sync2, zero, one int
	pilotPulses                    int
	bitLength                      int // 0x15: T-states per recorded sample

	// 0x12 pure tone: one pulse length, repeated.
	toneLength int
	toneCount  int

	// 0x13 pulse sequence: pulse lengths played one after another.
	pulses []int

	// Pause after the block, in milliseconds (0x20 and the data blocks).
	pauseMs int

	// Control blocks: 0x23/0x26 offset in blocks, 0x24 count, 0x28 options.
	offset int
	count  int
}

// signal reports whether the block produces any pulses.
func (b block) signal() bool {
	switch b.id {
	case idStandardSpeed, idTurboSpeed, idPureTone, idPulseSequence, idPureData, idDirectRecord:
		return true
	case idPause:
		return b.pauseMs > 0
	}
	return false
}

// Open reads a TZX image into the blocks it holds.
func Open(path string) ([]block, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("tzx: %w", err)
	}
	return parse(raw)
}

func parse(raw []byte) ([]block, error) {
	header := len(signaturePrefix) + 2
	if len(raw) < header {
		return nil, errors.New("tzx: file is too short to hold a header")
	}
	if string(raw[:len(signaturePrefix)]) != signaturePrefix {
		return nil, fmt.Errorf("tzx: not a TZX image: it does not start with %q", signaturePrefix)
	}

	p := &parser{raw: raw, pos: header}
	var blocks []block
	for p.pos < len(raw) {
		b, err := p.block()
		if err != nil {
			return nil, err
		}
		blocks = append(blocks, b)
	}
	if len(blocks) == 0 {
		return nil, errors.New("tzx: file holds no blocks")
	}
	return blocks, nil
}

// parser walks the file, refusing anything that does not fit rather than
// reading whatever follows as if it were the next field.
type parser struct {
	raw []byte
	pos int
}

func (p *parser) byte() (byte, error) {
	if p.pos >= len(p.raw) {
		return 0, p.short("a byte")
	}
	b := p.raw[p.pos]
	p.pos++
	return b, nil
}

func (p *parser) word() (int, error) {
	b, err := p.bytes(2)
	if err != nil {
		return 0, err
	}
	return int(binary.LittleEndian.Uint16(b)), nil
}

// signedWord is a two-byte signed value, which jumps and calls use for the
// distance to move in blocks.
func (p *parser) signedWord() (int, error) {
	w, err := p.word()
	if err != nil {
		return 0, err
	}
	return int(int16(uint16(w))), nil
}

// three reads a three-byte little-endian length, which the data blocks use
// because their payload can be longer than a block's worth of words.
func (p *parser) three() (int, error) {
	b, err := p.bytes(3)
	if err != nil {
		return 0, err
	}
	return int(b[0]) | int(b[1])<<8 | int(b[2])<<16, nil
}

func (p *parser) bytes(n int) ([]byte, error) {
	if n < 0 || p.pos+n > len(p.raw) {
		return nil, p.short(fmt.Sprintf("%d bytes", n))
	}
	b := p.raw[p.pos : p.pos+n]
	p.pos += n
	return b, nil
}

func (p *parser) short(what string) error {
	return fmt.Errorf("tzx: file ends at byte %d, where %s should be", p.pos, what)
}

func (p *parser) block() (block, error) {
	id, err := p.byte()
	if err != nil {
		return block{}, err
	}
	b := block{id: id, bitsLast: 8}

	switch id {
	case idStandardSpeed:
		// Pause, then the length and the bytes. The leader length is not in the
		// file: it follows from the flag byte, a header having a longer one.
		if b.pauseMs, err = p.word(); err != nil {
			return b, err
		}
		if b.data, err = p.dataBlock(2); err != nil {
			return b, err
		}
		b.pilot, b.sync1, b.sync2 = TimingPilot, TimingSync1, TimingSync2
		b.zero, b.one = TimingZero, TimingOne
		b.pilotPulses = pilotDataPulses
		if len(b.data) > 0 && b.data[0] < 0x80 {
			b.pilotPulses = pilotHeaderPulses
		}

	case idTurboSpeed:
		// Every timing is given, which is what makes a turbo loader a turbo
		// loader: it can send the same bits in far less time.
		fields := []*int{&b.pilot, &b.sync1, &b.sync2, &b.zero, &b.one, &b.pilotPulses}
		for _, f := range fields {
			if *f, err = p.word(); err != nil {
				return b, err
			}
		}
		last, err := p.byte()
		if err != nil {
			return b, err
		}
		if b.pauseMs, err = p.word(); err != nil {
			return b, err
		}
		if b.data, err = p.dataBlock(3); err != nil {
			return b, err
		}
		b.bitsLast = normaliseBits(last)

	case idPureTone:
		if b.toneLength, err = p.word(); err != nil {
			return b, err
		}
		if b.toneCount, err = p.word(); err != nil {
			return b, err
		}

	case idPulseSequence:
		count, err := p.byte()
		if err != nil {
			return b, err
		}
		b.pulses = make([]int, 0, count)
		for i := 0; i < int(count); i++ {
			l, err := p.word()
			if err != nil {
				return b, err
			}
			b.pulses = append(b.pulses, l)
		}

	case idPureData:
		if b.zero, err = p.word(); err != nil {
			return b, err
		}
		if b.one, err = p.word(); err != nil {
			return b, err
		}
		last, err := p.byte()
		if err != nil {
			return b, err
		}
		if b.pauseMs, err = p.word(); err != nil {
			return b, err
		}
		if b.data, err = p.dataBlock(3); err != nil {
			return b, err
		}
		b.bitsLast = normaliseBits(last)

	case idDirectRecord:
		// What a real tape player would have recorded: one sample per bit
		// period, rather than a description of how the bits were encoded.
		if b.bitLength, err = p.word(); err != nil {
			return b, err
		}
		if b.pauseMs, err = p.word(); err != nil {
			return b, err
		}
		last, err := p.byte()
		if err != nil {
			return b, err
		}
		if b.data, err = p.dataBlock(3); err != nil {
			return b, err
		}
		b.bitsLast = normaliseBits(last)

	case idPause:
		if b.pauseMs, err = p.word(); err != nil {
			return b, err
		}

	case idGroupStart:
		if _, err = p.string8(); err != nil {
			return b, err
		}

	case idJump, idCall:
		if b.offset, err = p.signedWord(); err != nil {
			return b, err
		}

	case idLoopStart:
		if b.count, err = p.word(); err != nil {
			return b, err
		}

	case idGroupEnd, idLoopEnd, idReturn:
		// Blocks that are a single identifier and nothing else: they end a
		// group, a loop or a called section.

	case idSelect:
		length, err := p.word()
		if err != nil {
			return b, err
		}
		start := p.pos
		count, err := p.byte()
		if err != nil {
			return b, err
		}
		for i := 0; i < int(count); i++ {
			if _, err = p.string16(); err != nil {
				return b, err
			}
		}
		// One offset per option, but only the first is ever followed.
		for i := 0; i < int(count); i++ {
			o, err := p.signedWord()
			if err != nil {
				return b, err
			}
			if i == 0 {
				b.offset = o
			}
		}
		if p.pos-start != length {
			return b, fmt.Errorf("tzx: select block says it is %d bytes but holds %d",
				length, p.pos-start)
		}

	case idStopIf48K:
		// Four bytes of reserved data; the machine this emulator is modelling is
		// a 48K one, so this always stops the tape.
		if _, err = p.bytes(4); err != nil {
			return b, err
		}

	case idSetLevel:
		// The level the signal should be held at. It is read and then dropped,
		// which is safe because every loader this emulator drives times the
		// interval between level changes: a signal that is inverted throughout
		// reads the same, and the leader of the block that follows has thousands
		// of changes in it, so the polarity is settled long before any data.
		if _, err = p.byte(); err != nil {
			return b, err
		}

	case idText:
		if _, err = p.string8(); err != nil {
			return b, err
		}

	case idMessage:
		if _, err = p.byte(); err != nil { // display duration
			return b, err
		}
		if _, err = p.string8(); err != nil {
			return b, err
		}

	case idArchiveInfo:
		length, err := p.word()
		if err != nil {
			return b, err
		}
		end := p.pos + length
		if end > len(p.raw) {
			return b, p.short(fmt.Sprintf("an archive info block of %d bytes", length))
		}
		for p.pos < end {
			if _, err = p.byte(); err != nil { // the type of the entry
				return b, err
			}
			if _, err = p.string8(); err != nil {
				return b, err
			}
		}

	case idHardware:
		count, err := p.byte()
		if err != nil {
			return b, err
		}
		if _, err = p.bytes(int(count) * 3); err != nil {
			return b, err
		}

	case idCustomInfo:
		if _, err = p.bytes(16); err != nil { // the identifier of the extension
			return b, err
		}
		// Then the length of what follows. This emulator has no use for custom
		// info, so it is skipped rather than interpreted.
		length, err := p.long()
		if err != nil {
			return b, err
		}
		if _, err = p.bytes(length); err != nil {
			return b, err
		}

	case idGlue:
		length, err := p.long()
		if err != nil {
			return b, err
		}
		if _, err = p.bytes(length); err != nil {
			return b, err
		}

	case 0x18, 0x19:
		// CSW and generalised data describe the signal in ways the other blocks
		// do not; refusing them by name beats decoding them wrongly.
		return b, fmt.Errorf("tzx: block %#02x at byte %d is not supported by this emulator",
			id, p.pos-1)

	default:
		return b, fmt.Errorf("tzx: unknown block %#02x at byte %d", id, p.pos-1)
	}

	return b, nil
}

// dataBlock reads a 2- or 3-byte length and the bytes that follow it. The
// payload includes the flag and checksum bytes, so it is what the ROM asks for
// plus two.
func (p *parser) dataBlock(lengthBytes int) ([]byte, error) {
	var length int
	var err error
	if lengthBytes == 3 {
		length, err = p.three()
	} else {
		length, err = p.word()
	}
	if err != nil {
		return nil, err
	}
	return p.bytes(length)
}

func (p *parser) string8() (string, error) {
	n, err := p.byte()
	if err != nil {
		return "", err
	}
	b, err := p.bytes(int(n))
	if err != nil {
		return "", err
	}
	return string(b), nil
}

func (p *parser) string16() (string, error) {
	n, err := p.word()
	if err != nil {
		return "", err
	}
	b, err := p.bytes(n)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

// long reads a four-byte little-endian length.
func (p *parser) long() (int, error) {
	b, err := p.bytes(4)
	if err != nil {
		return 0, err
	}
	return int(binary.LittleEndian.Uint32(b)), nil
}

// normaliseBits applies the format's own convention: a file that says zero bits
// are used in the last byte means a whole byte, which is what writers that
// never fill a partial byte leave behind.
func normaliseBits(n byte) int {
	if n == 0 {
		return 8
	}
	return int(n)
}

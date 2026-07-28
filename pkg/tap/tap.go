// Package tap provides ZX Spectrum TAP cassette file loading.
// Supports quick-load mode (direct memory copy) and authentic tape mode
// (cycle-accurate state machine simulating cassette playback).
package tap

import (
	"encoding/binary"
	"fmt"
	"os"
)

// ---------------------------------------------------------------------------
// TAP file format
// ---------------------------------------------------------------------------

// TAP block: 2-byte length (little-endian) followed by N data bytes.
const headerSize = 2

// File wraps a TAP file for sequential block reading.
type File struct {
	f     *os.File
	size  int64
	pos   int64
}

// Open opens a TAP file for reading.
func Open(path string) (*File, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	return &File{f: f, size: info.Size()}, nil
}

// Close closes the TAP file.
func (tf *File) Close() error {
	return tf.f.Close()
}

// ReadBlock reads the next block from the TAP file.
// Returns block data, or nil if EOF.
func (tf *File) ReadBlock() ([]byte, error) {
	var lenBuf [headerSize]byte
	n, err := tf.f.Read(lenBuf[:])
	if err != nil || n < headerSize {
		return nil, nil // EOF
	}
	blockLen := int(binary.LittleEndian.Uint16(lenBuf[:]))
	tf.pos += headerSize

	data := make([]byte, blockLen)
	n, err = tf.f.Read(data)
	if err != nil {
		return nil, err
	}
	tf.pos += int64(n)
	return data[:n], nil
}

// ---------------------------------------------------------------------------
// Quick-load: load all TAP blocks sequentially into memory
// ---------------------------------------------------------------------------

// LoadToMemory opens a TAP file and loads all blocks into memory starting at baseAddr.
// Returns the total bytes loaded and the end address.
func LoadToMemory(path string, mem []uint8, baseAddr uint16) (int, uint16, error) {
	tf, err := Open(path)
	if err != nil {
		return 0, 0, err
	}
	defer tf.Close()

	addr := int(baseAddr)
	totalBytes := 0
	blockCount := 0

	for {
		data, err := tf.ReadBlock()
		if err != nil {
			return 0, 0, fmt.Errorf("read block %d: %w", blockCount, err)
		}
		if data == nil {
			break
		}

		copy(mem[addr:], data)
		addr += len(data)
		totalBytes += len(data)
		blockCount++
	}

	return totalBytes, uint16(addr), nil
}

// ---------------------------------------------------------------------------
// Authentic tape playback state machine
// ---------------------------------------------------------------------------

// Timing constants (T-states at 3.5MHz)
const (
	pilotPulseLen   = 2168 // T-states per pilot pulse
	pilotCountLong  = 8063 // Number of pilot pulses for header blocks
	pilotCountShort = 3223 // Number of pilot pulses for data blocks
	sync1Len        = 667  // First sync pulse
	sync2Len        = 735  // Second sync pulse
	zeroLen         = 855  // Bit 0 pulse length
	oneLen          = 1710 // Bit 1 pulse length
)

// State is the tape playback state machine state.
type State int

const (
	StateIdle State = iota
	StatePilot
	StateSync
	StateData
	StateEnd
)

// Player implements an authentic Spectrum cassette tape player.
type Player struct {
	tapFile *File

	// Current block
	blockData  []uint8
	blockLen   int
	blockIdx   int // which block we're on
	bitPos     int // bit position within current byte (0-7, MSB first)

	// State machine
	state       State
	pulseCount  int // remaining pulses in current phase
	pulsePhase  int // 0 or 1 within a data bit
	earLevel    int // current EAR level (0 or 1)
	cycleCount  int // T-states since last edge
	lastEdgeAt  int // T-state count of last edge

	// Config
	pilotLen int // pilot pulse count for current block

	// Debug
	DebugLog string
}

// NewPlayer creates a new tape player for authentic loading.
func NewPlayer(path string) (*Player, error) {
	tf, err := Open(path)
	if err != nil {
		return nil, err
	}

	p := &Player{
		tapFile: tf,
		state:   StatePilot,
	}

	// Load first block
	if err := p.loadNextBlock(); err != nil {
		tf.Close()
		return nil, err
	}

	return p, nil
}

// Close closes the player.
func (p *Player) Close() {
	if p.tapFile != nil {
		p.tapFile.Close()
	}
}

// loadNextBlock loads the next block from the TAP file.
func (p *Player) loadNextBlock() error {
	data, err := p.tapFile.ReadBlock()
	if err != nil {
		return err
	}
	if data == nil {
		p.state = StateEnd
		return nil
	}

	p.blockData = data
	p.blockLen = len(data)
	p.bitPos = 0
	p.blockIdx++

	// Determine pilot length: flag byte 0 = header (long), 3 = data (short)
	if data[0] == 0x00 {
		p.pilotLen = pilotCountLong
	} else {
		p.pilotLen = pilotCountShort
	}

	p.state = StatePilot
	p.pulseCount = p.pilotLen
	p.pulsePhase = 0
	p.earLevel = 0
	p.cycleCount = pilotPulseLen
	p.lastEdgeAt = 0

	return nil
}

// nextBit reads the next bit from the current block (MSB first).
func (p *Player) nextBit() int {
	if p.bitPos >= p.blockLen*8 {
		return 0
	}
	byteIdx := p.bitPos / 8
	bitIdx := 7 - (p.bitPos % 8) // MSB first
	bit := (p.blockData[byteIdx] >> bitIdx) & 1
	p.bitPos++
	return int(bit)
}

// ReadEAR reads the current EAR (earphone) signal level.
// cpuCycles is the total T-states elapsed. Returns 0 or 1 (bit 6 of port 0xFE).
func (p *Player) ReadEAR(cpuCycles int) int {
	if p.state == StateEnd {
		return p.earLevel
	}
	if p.state == StateIdle {
		return 0
	}

	elapsed := cpuCycles - p.lastEdgeAt

	switch p.state {
	case StatePilot:
		if elapsed >= p.cycleCount {
			p.earLevel ^= 1
			p.lastEdgeAt = cpuCycles
			p.pulseCount--
			if p.pulseCount <= 0 {
				p.state = StateSync
				p.pulseCount = 2
				p.pulsePhase = 0
			}
		}

	case StateSync:
		if elapsed >= p.cycleCount {
			p.earLevel ^= 1
			p.lastEdgeAt = cpuCycles
			p.pulseCount--
			if p.pulseCount == 1 {
				p.cycleCount = sync2Len
			} else if p.pulseCount <= 0 {
				p.state = StateData
				p.pulsePhase = 0
				p.cycleCount = zeroLen
			} else {
				p.cycleCount = sync1Len
			}
		}

	case StateData:
		if elapsed >= p.cycleCount {
			p.earLevel ^= 1
			p.lastEdgeAt = cpuCycles
			if p.pulsePhase == 0 {
				// Read next bit, set cycle count based on bit value
				bit := p.nextBit()
				if bit == 0 {
					p.cycleCount = zeroLen
				} else {
					p.cycleCount = oneLen
				}
				p.pulsePhase = 1
			} else {
				// Second pulse of bit
				p.pulsePhase = 0
				// Check if block is exhausted
				if p.bitPos >= p.blockLen*8 {
					if err := p.loadNextBlock(); err != nil {
						p.state = StateEnd
					}
				}
			}
		}
	}

	return p.earLevel
}

// IsFinished returns true if tape playback has ended.
func (p *Player) IsFinished() bool {
	return p.state == StateEnd
}

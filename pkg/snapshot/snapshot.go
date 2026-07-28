// Package snapshot provides Z80 snapshot file (.z80) loading.
// Supports versions 1, 2, and 3 with RLE decompression.
package snapshot

import (
	"encoding/binary"
	"fmt"
	"os"
)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const (
	HeaderSize    = 30
	V1MemSize     = 48 * 1024 // 48KB uncompressed
	BlockHdrSize  = 3         // compressed_len (2) + page_number (1)
)

// Hardware modes
const (
	HW48K       = 0
	HW48KIF1    = 1
	HWSAMRAM    = 2
	HW128K      = 3
	HW128KIF1   = 4
	HWPlus3     = 5
	HWPlus2A    = 6
	HWPentagon  = 7
	HWScorpion  = 8
)

// V1 header (30 bytes). All register values are 8-bit unless noted.
type V1Header struct {
	A, F        uint8
	BC, HL      uint16 // little-endian
	PC, SP      uint16
	I, R        uint8
	Flags       uint8  // bit 0 = R bit 7, bit 1-2 = border, etc.
	DE          uint16
	BC1, DE1, HL1 uint16
	A1, F1      uint8
	IY, IX      uint16
	IFF1, IFF2  uint8
	IM          uint8
}

// V2Extended extends the V1 header with additional fields.
type V2Extended struct {
	Len               uint16 // extra header length (23 or 54/55)
	PC                uint16 // overrides V1's PC=0
	HardwareMode      uint8
	LastOut           uint8 // last OUT to 0xFFFD (128K paging)
	InterfaceROM      uint8
	EmulationFlags    uint8
	SoundRegisters    [16]uint8
}

// LoadResult holds the result of loading a .z80 snapshot.
type LoadResult struct {
	Version       int
	HardwareMode  uint8
}

// Load loads a .z80 snapshot file and restores CPU state and memory.
// mem is a 64KB buffer; CPU registers are returned as a struct.
func Load(path string, mem []uint8) (*CPUState, *LoadResult, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read snapshot: %w", err)
	}
	if len(data) < HeaderSize {
		return nil, nil, fmt.Errorf("file too small: %d bytes", len(data))
	}

	// Parse V1 header
	var h V1Header
	h.A = data[0]
	h.F = data[1]
	h.BC = binary.LittleEndian.Uint16(data[2:4])
	h.HL = binary.LittleEndian.Uint16(data[4:6])
	h.PC = binary.LittleEndian.Uint16(data[6:8])
	h.SP = binary.LittleEndian.Uint16(data[8:10])
	h.I = data[10]
	h.R = data[11]
	h.Flags = data[12]
	h.DE = binary.LittleEndian.Uint16(data[13:15])
	h.BC1 = binary.LittleEndian.Uint16(data[15:17])
	h.DE1 = binary.LittleEndian.Uint16(data[17:19])
	h.HL1 = binary.LittleEndian.Uint16(data[19:21])
	h.A1 = data[21]
	h.F1 = data[22]
	h.IY = binary.LittleEndian.Uint16(data[23:25])
	h.IX = binary.LittleEndian.Uint16(data[25:27])
	h.IFF1 = data[27]
	h.IFF2 = data[28]
	h.IM = data[29]

	result := &LoadResult{}

	var cpu CPUState
	cpu.A = h.A
	cpu.F = h.F
	cpu.B = uint8(h.BC >> 8)
	cpu.C = uint8(h.BC & 0xFF)
	cpu.D = uint8(h.DE >> 8)
	cpu.E = uint8(h.DE & 0xFF)
	cpu.H = uint8(h.HL >> 8)
	cpu.L = uint8(h.HL & 0xFF)
	cpu.SP = h.SP
	cpu.I = h.I
	cpu.R = (h.R & 0x7F) | ((h.Flags & 1) << 7) // R bit 7 from flags
	cpu.IXh = uint8(h.IX >> 8)
	cpu.IXl = uint8(h.IX & 0xFF)
	cpu.IYh = uint8(h.IY >> 8)
	cpu.IYl = uint8(h.IY & 0xFF)
	cpu.A1 = h.A1
	cpu.F1 = h.F1
	cpu.B1 = uint8(h.BC1 >> 8)
	cpu.C1 = uint8(h.BC1 & 0xFF)
	cpu.D1 = uint8(h.DE1 >> 8)
	cpu.E1 = uint8(h.DE1 & 0xFF)
	cpu.H1 = uint8(h.HL1 >> 8)
	cpu.L1 = uint8(h.HL1 & 0xFF)
	cpu.IFF1 = h.IFF1 != 0
	cpu.IFF2 = h.IFF2 != 0
	cpu.IM = h.IM

	// Determine version
	pos := HeaderSize
	if h.PC != 0 {
		result.Version = 1
		cpu.PC = h.PC
	} else {
		// V2 or V3: read extended header
		extLen := binary.LittleEndian.Uint16(data[pos:])
		pos += 2
		if extLen >= 23 {
			result.Version = 2
			if extLen >= 54 {
				result.Version = 3
			}
			cpu.PC = binary.LittleEndian.Uint16(data[pos:])
			pos += 2
			result.HardwareMode = data[pos]
			pos += 1
			// Skip remaining extended header fields
			pos += int(extLen) - 3
		}
	}

	// Load memory
	if result.Version == 1 {
		compressed := h.Flags&0x20 != 0
		if compressed {
			buf := make([]uint8, V1MemSize)
			if err := decompress(data[pos:], buf, V1MemSize); err != nil {
				return nil, nil, fmt.Errorf("decompress V1: %w", err)
			}
			copy(mem[0x4000:], buf) // RAM starts at 0x4000
		} else {
			copy(mem[0x4000:], data[pos:pos+V1MemSize])
		}
	} else {
		// V2/V3: read memory blocks
		for pos < len(data) {
			if pos+BlockHdrSize > len(data) {
				break
			}
			compLen := int(binary.LittleEndian.Uint16(data[pos:]))
			pageNum := data[pos+2]
			pos += BlockHdrSize

			var pageAddr int
			switch pageNum {
			case 0:
				pageAddr = 0x0000
			case 4:
				pageAddr = 0x8000
			case 5:
				pageAddr = 0xC000
			case 8:
				pageAddr = 0x4000
			default:
				// Unknown page, skip
				if compLen == 0xFFFF {
					pos += 16384
				} else {
					pos += compLen
				}
				continue
			}

			if compLen == 0xFFFF {
				// Uncompressed 16KB
				copy(mem[pageAddr:], data[pos:pos+16384])
				pos += 16384
			} else {
				// RLE compressed
				buf := make([]uint8, 16384)
				n, err := decompressTo(data[pos:], buf)
				if err != nil {
					return nil, nil, fmt.Errorf("decompress page %d: %w", pageNum, err)
				}
				copy(mem[pageAddr:], buf[:n])
				pos += compLen
			}
		}
	}

	return &cpu, result, nil
}

// CPUState holds all Z80 registers as loaded from a snapshot.
type CPUState struct {
	A, F, B, C, D, E, H, L         uint8
	IXh, IXl, IYh, IYl             uint8
	A1, F1, B1, C1, D1, E1, H1, L1 uint8
	PC, SP                         uint16
	I, R                           uint8
	IFF1, IFF2                     bool
	IM                             uint8
}

// ---------------------------------------------------------------------------
// RLE decompression
// ---------------------------------------------------------------------------

// decompress decompresses Z80 snapshot RLE data into dst (exact size).
// Format: ED ED xx yy = repeat yy for xx times. 00 ED ED 00 = end marker.
func decompress(src []uint8, dst []uint8, expectedSize int) error {
	n, err := decompressTo(src, dst)
	if err != nil {
		return err
	}
	if n < expectedSize {
		// Pad with zeros if needed
		for i := n; i < expectedSize && i < len(dst); i++ {
			dst[i] = 0
		}
	}
	return nil
}

// decompressTo decompresses data and returns the number of bytes written.
func decompressTo(src []uint8, dst []uint8) (int, error) {
	dstIdx := 0
	for i := 0; i < len(src); {
		if i+3 < len(src) && src[i] == 0x00 && src[i+1] == 0xED && src[i+2] == 0xED && src[i+3] == 0x00 {
			break // end marker
		}
		if i+3 < len(src) && src[i] == 0xED && src[i+1] == 0xED {
			count := int(src[i+2])
			val := src[i+3]
			for j := 0; j < count && dstIdx < len(dst); j++ {
				dst[dstIdx] = val
				dstIdx++
			}
			i += 4
		} else {
			if dstIdx < len(dst) {
				dst[dstIdx] = src[i]
				dstIdx++
			}
			i++
		}
	}
	return dstIdx, nil
}

// Package ula provides ZX Spectrum video RAM to terminal rendering.
// Supports three render modes: block (2x2 Unicode quadrants), braille (2x4 dots), and OCR (8x8 font matching).
package ula

import (
	"fmt"
	"math/bits"
	"os"
	"sync"
	"time"

	"golang.org/x/term"
)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const (
	ScreenWidth      = 256
	ScreenHeight     = 192
	ScreenWidthBytes = ScreenWidth / 8       // 32
	VRAMSize         = ScreenWidthBytes * ScreenHeight // 6144
	AttrSize         = 32 * 24               // 768
	TotalVRAM        = VRAMSize + AttrSize   // 6912

	AttrCols = 32
	AttrRows = 24

	// Attribute byte masks
	AttrInk    = 0x07
	AttrPaper  = 0x38
	AttrBright = 0x40
	AttrBlink  = 0x80

	// Output dimensions
	OutputWidth  = ScreenWidth / 2  // 128 (block mode)
	OutputHeight = ScreenHeight / 2 // 96

	BrailleOutputWidth  = ScreenWidth / 2  // 128
	BrailleOutputHeight = ScreenHeight / 4 // 48

	OCROutputWidth  = ScreenWidth / 8  // 32
	OCROutputHeight = ScreenHeight / 8 // 24

	FrameTargetUS = 20000 // 20ms = 50Hz
)

// RenderMode selects the terminal rendering style.
type RenderMode int

const (
	RenderBraille RenderMode = iota
	RenderBlock
	RenderOCR
)

// ---------------------------------------------------------------------------
// Color types
// ---------------------------------------------------------------------------

// ColorAttr holds decoded attribute data for a character cell.
type ColorAttr struct {
	Ink    uint8
	Paper  uint8
	Bright uint8
	Blink  uint8
}

// Spectrum color index → ANSI color index mapping.
// Spectrum: 0=Black, 1=Blue, 2=Red, 3=Magenta, 4=Green, 5=Cyan, 6=Yellow, 7=White
// ANSI:     0=Black, 1=Red, 2=Green, 3=Yellow, 4=Blue, 5=Magenta, 6=Cyan, 7=White
var spectrumToANSI = [8]int{0, 4, 1, 5, 2, 6, 3, 7}

// ---------------------------------------------------------------------------
// Block characters (2x2 pixels → Unicode quadrant)
// ---------------------------------------------------------------------------

var blockChars = [16]string{
	" ", "▗", "▖", "▄", "▝", "▐", "▞", "▟",
	"▘", "▚", "▌", "▙", "▀", "▜", "▛", "█",
}

// ---------------------------------------------------------------------------
// Sinclair ZX Spectrum ROM font (96 characters: ASCII 32–127)
// Characters 96 = £, 127 = ©
// ---------------------------------------------------------------------------

var sinclairFont = [96][8]uint8{
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //  32 SPACE
	{0x00, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x00}, //  33 !
	{0x00, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00}, //  34 "
	{0x00, 0x24, 0x7E, 0x24, 0x24, 0x7E, 0x24, 0x00}, //  35 #
	{0x00, 0x08, 0x3E, 0x28, 0x3E, 0x0A, 0x3E, 0x08}, //  36 $
	{0x00, 0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00}, //  37 %
	{0x00, 0x10, 0x28, 0x10, 0x2A, 0x44, 0x3A, 0x00}, //  38 &
	{0x00, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}, //  39 '
	{0x00, 0x04, 0x08, 0x08, 0x08, 0x08, 0x04, 0x00}, //  40 (
	{0x00, 0x20, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00}, //  41 )
	{0x00, 0x00, 0x14, 0x08, 0x3E, 0x08, 0x14, 0x00}, //  42 *
	{0x00, 0x00, 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00}, //  43 +
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x10}, //  44 ,
	{0x00, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00}, //  45 -
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, //  46 .
	{0x00, 0x00, 0x02, 0x04, 0x08, 0x10, 0x20, 0x00}, //  47 /
	{0x00, 0x3C, 0x46, 0x4A, 0x52, 0x62, 0x3C, 0x00}, //  48 0
	{0x00, 0x18, 0x28, 0x08, 0x08, 0x08, 0x3E, 0x00}, //  49 1
	{0x00, 0x3C, 0x42, 0x02, 0x3C, 0x40, 0x7E, 0x00}, //  50 2
	{0x00, 0x3C, 0x42, 0x0C, 0x02, 0x42, 0x3C, 0x00}, //  51 3
	{0x00, 0x08, 0x18, 0x28, 0x48, 0x7E, 0x08, 0x00}, //  52 4
	{0x00, 0x7E, 0x40, 0x7C, 0x02, 0x42, 0x3C, 0x00}, //  53 5
	{0x00, 0x3C, 0x40, 0x7C, 0x42, 0x42, 0x3C, 0x00}, //  54 6
	{0x00, 0x7E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x00}, //  55 7
	{0x00, 0x3C, 0x42, 0x3C, 0x42, 0x42, 0x3C, 0x00}, //  56 8
	{0x00, 0x3C, 0x42, 0x42, 0x3E, 0x02, 0x3C, 0x00}, //  57 9
	{0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00}, //  58 :
	{0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x10, 0x20}, //  59 ;
	{0x00, 0x00, 0x04, 0x08, 0x10, 0x08, 0x04, 0x00}, //  60 <
	{0x00, 0x00, 0x00, 0x3E, 0x00, 0x3E, 0x00, 0x00}, //  61 =
	{0x00, 0x00, 0x10, 0x08, 0x04, 0x08, 0x10, 0x00}, //  62 >
	{0x00, 0x3C, 0x42, 0x04, 0x08, 0x00, 0x08, 0x00}, //  63 ?
	{0x00, 0x3C, 0x4A, 0x56, 0x5E, 0x40, 0x3C, 0x00}, //  64 @
	{0x00, 0x3C, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x00}, //  65 A
	{0x00, 0x7C, 0x42, 0x7C, 0x42, 0x42, 0x7C, 0x00}, //  66 B
	{0x00, 0x3C, 0x42, 0x40, 0x40, 0x42, 0x3C, 0x00}, //  67 C
	{0x00, 0x78, 0x44, 0x42, 0x42, 0x44, 0x78, 0x00}, //  68 D
	{0x00, 0x7E, 0x40, 0x7C, 0x40, 0x40, 0x7E, 0x00}, //  69 E
	{0x00, 0x7E, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x00}, //  70 F
	{0x00, 0x3C, 0x42, 0x40, 0x4E, 0x42, 0x3C, 0x00}, //  71 G
	{0x00, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00}, //  72 H
	{0x00, 0x3E, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00}, //  73 I
	{0x00, 0x02, 0x02, 0x02, 0x42, 0x42, 0x3C, 0x00}, //  74 J
	{0x00, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00}, //  75 K
	{0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E, 0x00}, //  76 L
	{0x00, 0x42, 0x66, 0x5A, 0x42, 0x42, 0x42, 0x00}, //  77 M
	{0x00, 0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x00}, //  78 N
	{0x00, 0x3C, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00}, //  79 O
	{0x00, 0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x00}, //  80 P
	{0x00, 0x3C, 0x42, 0x42, 0x52, 0x4A, 0x3C, 0x00}, //  81 Q
	{0x00, 0x7C, 0x42, 0x42, 0x7C, 0x44, 0x42, 0x00}, //  82 R
	{0x00, 0x3C, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00}, //  83 S
	{0x00, 0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00}, //  84 T
	{0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00}, //  85 U
	{0x00, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00}, //  86 V
	{0x00, 0x42, 0x42, 0x42, 0x42, 0x5A, 0x24, 0x00}, //  87 W
	{0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00}, //  88 X
	{0x00, 0x82, 0x44, 0x28, 0x10, 0x10, 0x10, 0x00}, //  89 Y
	{0x00, 0x7E, 0x04, 0x08, 0x10, 0x20, 0x7E, 0x00}, //  90 Z
	{0x00, 0x0E, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00}, //  91 [
	{0x00, 0x00, 0x40, 0x20, 0x10, 0x08, 0x04, 0x00}, //  92 backslash
	{0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00}, //  93 ]
	{0x00, 0x10, 0x38, 0x54, 0x10, 0x10, 0x10, 0x00}, //  94 ^
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, //  95 _
	{0x00, 0x1C, 0x22, 0x78, 0x20, 0x20, 0x7E, 0x00}, //  96 £
	{0x00, 0x00, 0x38, 0x04, 0x3C, 0x44, 0x3C, 0x00}, //  97 a
	{0x00, 0x20, 0x20, 0x3C, 0x22, 0x22, 0x3C, 0x00}, //  98 b
	{0x00, 0x00, 0x1C, 0x20, 0x20, 0x20, 0x1C, 0x00}, //  99 c
	{0x00, 0x04, 0x04, 0x3C, 0x44, 0x44, 0x3C, 0x00}, // 100 d
	{0x00, 0x00, 0x38, 0x44, 0x78, 0x40, 0x3C, 0x00}, // 101 e
	{0x00, 0x0C, 0x10, 0x18, 0x10, 0x10, 0x10, 0x00}, // 102 f
	{0x00, 0x00, 0x3C, 0x44, 0x44, 0x3C, 0x04, 0x38}, // 103 g
	{0x00, 0x40, 0x40, 0x78, 0x44, 0x44, 0x44, 0x00}, // 104 h
	{0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x38, 0x00}, // 105 i
	{0x00, 0x04, 0x00, 0x04, 0x04, 0x04, 0x24, 0x18}, // 106 j
	{0x00, 0x20, 0x28, 0x30, 0x30, 0x28, 0x24, 0x00}, // 107 k
	{0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0C, 0x00}, // 108 l
	{0x00, 0x00, 0x68, 0x54, 0x54, 0x54, 0x54, 0x00}, // 109 m
	{0x00, 0x00, 0x78, 0x44, 0x44, 0x44, 0x44, 0x00}, // 110 n
	{0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x38, 0x00}, // 111 o
	{0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40}, // 112 p
	{0x00, 0x00, 0x3C, 0x44, 0x44, 0x3C, 0x04, 0x06}, // 113 q
	{0x00, 0x00, 0x1C, 0x20, 0x20, 0x20, 0x20, 0x00}, // 114 r
	{0x00, 0x00, 0x38, 0x40, 0x38, 0x04, 0x78, 0x00}, // 115 s
	{0x00, 0x10, 0x38, 0x10, 0x10, 0x10, 0x0C, 0x00}, // 116 t
	{0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00}, // 117 u
	{0x00, 0x00, 0x44, 0x44, 0x28, 0x28, 0x10, 0x00}, // 118 v
	{0x00, 0x00, 0x44, 0x54, 0x54, 0x54, 0x28, 0x00}, // 119 w
	{0x00, 0x00, 0x44, 0x28, 0x10, 0x28, 0x44, 0x00}, // 120 x
	{0x00, 0x00, 0x44, 0x44, 0x44, 0x3C, 0x04, 0x38}, // 121 y
	{0x00, 0x00, 0x7C, 0x08, 0x10, 0x20, 0x7C, 0x00}, // 122 z
	{0x00, 0x0E, 0x08, 0x30, 0x08, 0x08, 0x0E, 0x00}, // 123 {
	{0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, // 124 |
	{0x00, 0x70, 0x10, 0x0C, 0x10, 0x10, 0x70, 0x00}, // 125 }
	{0x00, 0x14, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00}, // 126 ~
	{0x3C, 0x42, 0x99, 0xA1, 0xA1, 0x99, 0x42, 0x3C}, // 127 ©
}

// ---------------------------------------------------------------------------
// ULA state
// ---------------------------------------------------------------------------

// State holds the ULA renderer state.
type State struct {
	mu           sync.Mutex
	VRAM         []uint8
	border       uint8
	FrameCounter uint32
	RenderMode   RenderMode

	// Terminal state
	origTermios *term.State
	termWidth   int
}

// New creates a new ULA state bound to the given VRAM buffer.
func New(vram []uint8, mode RenderMode) *State {
	return &State{
		VRAM:       vram,
		RenderMode: mode,
	}
}

// SetBorderColor sets the border color (0-7).
func (s *State) SetBorderColor(c uint8) {
	s.mu.Lock()
	s.border = c
	s.mu.Unlock()
}

// BorderColor returns the current border color.
func (s *State) BorderColor() uint8 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.border
}

// ---------------------------------------------------------------------------
// Pixel access (Z80 interleaved scanline layout)
// ---------------------------------------------------------------------------

// getPixel reads a single pixel from VRAM at (x, y).
func getPixel(vram []uint8, x, y int) int {
	section := y / 64
	lineInSection := y % 64
	charRow := lineInSection / 8
	pixelRow := lineInSection % 8
	charCol := x / 8

	addr := (section * 2048) + (pixelRow * 256) + (charRow * 32) + charCol
	if addr >= VRAMSize {
		return 0
	}

	byte := vram[addr]
	bitIndex := 7 - (x % 8)
	return int((byte >> bitIndex) & 1)
}

// getAttr reads the attribute for the character cell at (x, y).
func getAttr(vram []uint8, x, y int) ColorAttr {
	charCol := (x / 8) % AttrCols
	charRow := (y / 8) % AttrRows

	attrAddr := VRAMSize + (charRow * AttrCols) + charCol
	if attrAddr >= len(vram) {
		return ColorAttr{Ink: 0, Paper: 7} // default: black on white
	}

	b := vram[attrAddr]
	return ColorAttr{
		Ink:    b & AttrInk,
		Paper:  (b & AttrPaper) >> 3,
		Bright: (b & AttrBright) >> 6,
		Blink:  (b & AttrBlink) >> 7,
	}
}

// ansiColor returns the ANSI SGR code for a Spectrum color.
func ansiColor(spectrum uint8, bright uint8, isForeground bool) int {
	c := spectrumToANSI[spectrum]
	base := 30
	if !isForeground {
		base = 40
	}
	if bright != 0 {
		base += 60 // bright: 90-97 or 100-107
	}
	return base + c
}

// ---------------------------------------------------------------------------
// Block mode rendering (2x2 → Unicode quadrant)
// ---------------------------------------------------------------------------

func writeBlockFrame(s *State, buf *[]byte) {
	vram := s.VRAM
	fc := s.FrameCounter
	blinkPhase := (fc/16)&1 == 0

	for row := 0; row < OutputHeight; row++ {
		for col := 0; col < OutputWidth; col++ {
			px := col * 2
			py := row * 2

			tl := getPixel(vram, px, py)
			tr := getPixel(vram, px+1, py)
			bl := getPixel(vram, px, py+1)
			br := getPixel(vram, px+1, py+1)

			pattern := (tl << 3) | (tr << 2) | (bl << 1) | br
			ch := blockChars[pattern]

			attr := getAttr(vram, px, py)
			ink := attr.Ink
			paper := attr.Paper
			if attr.Blink != 0 && !blinkPhase {
				ink, paper = paper, ink
			}

			fg := ansiColor(ink, attr.Bright, true)
			bg := ansiColor(paper, 0, false)
			*buf = append(*buf, fmt.Sprintf("\033[%d;%dm%s", fg, bg, ch)...)
		}
		*buf = append(*buf, "\033[0m\n"...)
	}
}

// ---------------------------------------------------------------------------
// Braille mode rendering (2x4 → Unicode Braille)
// ---------------------------------------------------------------------------

func writeBrailleFrame(s *State, buf *[]byte) {
	vram := s.VRAM
	fc := s.FrameCounter
	blinkPhase := (fc/16)&1 == 0

	for row := 0; row < BrailleOutputHeight; row++ {
		for col := 0; col < BrailleOutputWidth; col++ {
			px := col * 2
			py := row * 4

			// Read 8 pixels: 2 cols × 4 rows
			var pattern uint16
			// Left column: dots 0,1,2,6
			if getPixel(vram, px, py) != 0 { pattern |= 1 << 0 }
			if getPixel(vram, px, py+1) != 0 { pattern |= 1 << 1 }
			if getPixel(vram, px, py+2) != 0 { pattern |= 1 << 2 }
			if getPixel(vram, px+1, py) != 0 { pattern |= 1 << 3 }
			if getPixel(vram, px+1, py+1) != 0 { pattern |= 1 << 4 }
			if getPixel(vram, px+1, py+2) != 0 { pattern |= 1 << 5 }
			if getPixel(vram, px, py+3) != 0 { pattern |= 1 << 6 }
			if getPixel(vram, px+1, py+3) != 0 { pattern |= 1 << 7 }

			// UTF-8 encoding of U+2800 + pattern
			cp := 0x2800 + pattern
			utf8 := []byte{0xE2, 0xA0 | byte(cp>>6), 0x80 | byte(cp&0x3F)}

			attr := getAttr(vram, px, py)
			ink := attr.Ink
			paper := attr.Paper
			if attr.Blink != 0 && !blinkPhase {
				ink, paper = paper, ink
			}

			fg := ansiColor(ink, attr.Bright, true)
			bg := ansiColor(paper, 0, false)
			*buf = append(*buf, fmt.Sprintf("\033[%d;%dm%s", fg, bg, string(utf8))...)
		}
		*buf = append(*buf, "\033[0m\n"...)
	}
}

// ---------------------------------------------------------------------------
// OCR mode rendering (8x8 → ASCII character via font matching)
// ---------------------------------------------------------------------------

// hammingDistance returns the number of differing bits between two 8-byte bitmaps.
func hammingDistance(a, b [8]uint8) int {
	d := 0
	for i := 0; i < 8; i++ {
		d += bits.OnesCount8(a[i] ^ b[i])
	}
	return d
}

func writeOCRFrame(s *State, buf *[]byte) {
	vram := s.VRAM
	fc := s.FrameCounter
	blinkPhase := (fc/16)&1 == 0

	for row := 0; row < OCROutputHeight; row++ {
		for col := 0; col < OCROutputWidth; col++ {
			px := col * 8
			py := row * 8

			// Read 8x8 bitmap
			var bitmap [8]uint8
			for i := 0; i < 8; i++ {
				for j := 0; j < 8; j++ {
					if getPixel(vram, px+j, py+i) != 0 {
						bitmap[i] |= 1 << (7 - j)
					}
				}
			}

			// Match against font
			bestDist := 999
			bestChar := byte(' ')
			for c := 0; c < 96; c++ {
				d := hammingDistance(bitmap, sinclairFont[c])
				if d < bestDist {
					bestDist = d
					bestChar = byte(c + 32)
				}
			}

			// If too many bits differ (>12), treat as space
			if bestDist > 12 {
				bestChar = ' '
			}

			// Handle non-standard characters
			var out string
			switch bestChar {
			case 96:
				out = "£"
			case 127:
				out = "©"
			default:
				out = string(bestChar)
			}

			attr := getAttr(vram, px, py)
			ink := attr.Ink
			paper := attr.Paper
			if attr.Blink != 0 && !blinkPhase {
				ink, paper = paper, ink
			}

			fg := ansiColor(ink, attr.Bright, true)
			bg := ansiColor(paper, 0, false)
			*buf = append(*buf, fmt.Sprintf("\033[%d;%dm%s", fg, bg, out)...)
		}
		*buf = append(*buf, "\033[0m\n"...)
	}
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

// RenderFrame builds the full terminal frame buffer based on current VRAM state.
// Returns the rendered string.
func (s *State) RenderFrame() string {
	s.mu.Lock()
	defer s.mu.Unlock()

	var buf []byte
	buf = append(buf, "\033[H"...) // cursor home

	// Border top
	borderANSI := fmt.Sprintf("\033[%dm", 40+ansiColor(s.BorderColor(), 0, false))
	borderWidth := s.termWidth
	if borderWidth == 0 {
		borderWidth = 80
	}

	topBorderRows := 1
	for i := 0; i < topBorderRows; i++ {
		for j := 0; j < borderWidth; j++ {
			buf = append(buf, borderANSI...)
			buf = append(buf, ' ')
		}
		buf = append(buf, "\033[0m\n"...)
	}

	switch s.RenderMode {
	case RenderBlock:
		writeBlockFrame(s, &buf)
	case RenderBraille:
		writeBrailleFrame(s, &buf)
	case RenderOCR:
		writeOCRFrame(s, &buf)
	}

	// Border bottom
	for i := 0; i < 1; i++ {
		for j := 0; j < borderWidth; j++ {
			buf = append(buf, borderANSI...)
			buf = append(buf, ' ')
		}
		buf = append(buf, "\033[0m"...)
	}

	s.FrameCounter++
	return string(buf)
}

// ---------------------------------------------------------------------------
// 50Hz frame timing
// ---------------------------------------------------------------------------

// WaitFrame waits until the next 50Hz frame boundary.
func WaitFrame(frameStart time.Time) {
	elapsed := time.Since(frameStart)
	if elapsed < FrameTargetUS*time.Microsecond {
		time.Sleep(FrameTargetUS*time.Microsecond - elapsed)
	}
}

// ---------------------------------------------------------------------------
// Terminal init/cleanup
// ---------------------------------------------------------------------------

// TermInit initializes the terminal for rendering: raw mode, alternate screen, hidden cursor.
func TermInit() (*term.State, error) {
	orig, err := term.MakeRaw(int(os.Stdin.Fd()))
	if err != nil {
		return nil, fmt.Errorf("term.MakeRaw: %w", err)
	}

	fmt.Print("\033[?1049h") // enter alternate screen
	fmt.Print("\033[2J")      // clear
	fmt.Print("\033[?25l")    // hide cursor

	return orig, nil
}

// TermCleanup restores the terminal to original state.
func TermCleanup(orig *term.State) {
	fmt.Print("\033[?25h")       // show cursor
	fmt.Print("\033[?1049l")     // exit alternate screen
	if orig != nil {
		term.Restore(int(os.Stdin.Fd()), orig)
	}
}

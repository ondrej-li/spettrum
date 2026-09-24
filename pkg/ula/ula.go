// Package ula provides ZX Spectrum video RAM to terminal rendering.
// Supports three render modes: block (2x2 Unicode quadrants), braille (2x4 dots), and OCR (8x8 font matching).
package ula

import (
	"fmt"
	"math/bits"
	"os"
	"strconv"
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
	ScreenWidthBytes = ScreenWidth / 8                 // 32
	VRAMSize         = ScreenWidthBytes * ScreenHeight // 6144
	AttrSize         = 32 * 24                         // 768
	TotalVRAM        = VRAMSize + AttrSize             // 6912

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
)

// RenderMode selects the terminal rendering style.
type RenderMode int

// String returns the render mode's name.
func (m RenderMode) String() string {
	switch m {
	case RenderBlock:
		return "block"
	case RenderBraille:
		return "braille"
	default:
		return "ocr"
	}
}

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

	// Terminal geometry to fit frames to. Zero means the output is not a
	// terminal, in which case frames are emitted at their natural size.
	termCols int
	termRows int
}

// SetTerminalSize overrides the size frames are fitted to. Passing zeroes means
// "not a terminal" and restores natural-size frames; that is what tests, which
// do not run attached to a terminal, should use.
func (s *State) SetTerminalSize(cols, rows int) {
	s.mu.Lock()
	s.termCols, s.termRows = cols, rows
	s.mu.Unlock()
}

// BodySize returns the size in characters of the rendered display for the current
// render mode, excluding any border.
func (s *State) BodySize() (w, h int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	w, h, _ = s.bodyWriter()
	return w, h
}

// TerminalSize returns the size of the terminal attached to stdout, if any.
func TerminalSize() (cols, rows int, ok bool) {
	c, r, err := term.GetSize(int(os.Stdout.Fd()))
	if err != nil || c <= 0 || r <= 0 {
		return 0, 0, false
	}
	return c, r, true
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

// noColour marks "no colour emitted yet" in appendCell's last key.
const noColour = ^uint32(0)

// appendCell appends one character with the given foreground/background ANSI
// colours, emitting the escape sequence only when the colours differ from the
// previous cell. Emitting a code for every cell costs several times more
// terminal traffic and makes the renderer the bottleneck on slow terminals.
func appendCell(buf []byte, last *uint32, fg, bg int, ch string) []byte {
	key := uint32(fg)<<8 | uint32(bg)
	if key != *last {
		buf = append(buf, "\033["...)
		buf = strconv.AppendInt(buf, int64(fg), 10)
		buf = append(buf, ';')
		buf = strconv.AppendInt(buf, int64(bg), 10)
		buf = append(buf, 'm')
		*last = key
	}
	return append(buf, ch...)
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

// writeBlockRow appends one row of the 2x2 block rendering to buf.
func writeBlockRow(s *State, row, maxCols int, buf *[]byte) {
	vram := s.VRAM
	blinkPhase := (s.FrameCounter/16)&1 == 0
	last := uint32(noColour)

	for col := 0; col < OutputWidth && col < maxCols; col++ {
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

		*buf = appendCell(*buf, &last,
			ansiColor(ink, attr.Bright, true), ansiColor(paper, 0, false), ch)
	}
}

// ---------------------------------------------------------------------------
// Braille mode rendering (2x4 → Unicode Braille)
// ---------------------------------------------------------------------------

// writeBrailleRow appends one row of the 2x4 braille rendering to buf.
func writeBrailleRow(s *State, row, maxCols int, buf *[]byte) {
	vram := s.VRAM
	blinkPhase := (s.FrameCounter/16)&1 == 0
	last := uint32(noColour)

	for col := 0; col < BrailleOutputWidth && col < maxCols; col++ {
		px := col * 2
		py := row * 4

		// Read 8 pixels: 2 cols × 4 rows
		var pattern uint16
		// Left column: dots 0,1,2,6
		if getPixel(vram, px, py) != 0 {
			pattern |= 1 << 0
		}
		if getPixel(vram, px, py+1) != 0 {
			pattern |= 1 << 1
		}
		if getPixel(vram, px, py+2) != 0 {
			pattern |= 1 << 2
		}
		if getPixel(vram, px+1, py) != 0 {
			pattern |= 1 << 3
		}
		if getPixel(vram, px+1, py+1) != 0 {
			pattern |= 1 << 4
		}
		if getPixel(vram, px+1, py+2) != 0 {
			pattern |= 1 << 5
		}
		if getPixel(vram, px, py+3) != 0 {
			pattern |= 1 << 6
		}
		if getPixel(vram, px+1, py+3) != 0 {
			pattern |= 1 << 7
		}

		// UTF-8 encoding of U+2800 + pattern
		cp := 0x2800 + pattern
		braille := []byte{0xE2, 0xA0 | byte(cp>>6), 0x80 | byte(cp&0x3F)}

		attr := getAttr(vram, px, py)
		ink := attr.Ink
		paper := attr.Paper
		if attr.Blink != 0 && !blinkPhase {
			ink, paper = paper, ink
		}

		fg := ansiColor(ink, attr.Bright, true)
		bg := ansiColor(paper, 0, false)
		*buf = appendCell(*buf, &last, fg, bg, string(braille))
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

// writeOCRRow appends one row of the 8x8 font-matched rendering to buf.
func writeOCRRow(s *State, row, maxCols int, buf *[]byte) {
	vram := s.VRAM
	blinkPhase := (s.FrameCounter/16)&1 == 0
	last := uint32(noColour)

	for col := 0; col < OCROutputWidth && col < maxCols; col++ {
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
		*buf = appendCell(*buf, &last, fg, bg, out)
	}
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

// bodyWriter returns the display size in characters and the per-row writer for
// the current render mode.
func (s *State) bodyWriter() (w, h int, writeRow func(*State, int, int, *[]byte)) {
	switch s.RenderMode {
	case RenderBlock:
		return OutputWidth, OutputHeight, writeBlockRow
	case RenderBraille:
		return BrailleOutputWidth, BrailleOutputHeight, writeBrailleRow
	default:
		return OCROutputWidth, OCROutputHeight, writeOCRRow
	}
}

// terminalSize reports the geometry frames should be fitted to, as set by
// SetTerminalSize. A zero size means the output is not a terminal and frames are
// emitted at their natural size. The caller must hold s.mu.
func (s *State) terminalSize() (cols, rows int, ok bool) {
	if s.termCols > 0 && s.termRows > 0 {
		return s.termCols, s.termRows, true
	}
	return 0, 0, false
}

// RenderFrame builds the full terminal frame buffer based on current VRAM state.
// Returns the rendered string.
//
// When the output is a terminal the frame is fitted to it: the border is dropped
// unless the whole body plus borders fits, and the image is clipped to the window.
// A frame taller than the terminal (or a row wider than it) would wrap or scroll,
// and scrolling smears successive frames into each other so the picture appears to
// crawl diagonally. When the output is not a terminal the frame is emitted at its
// natural size so that captured frames stay self-contained.
func (s *State) RenderFrame() string {
	s.mu.Lock()
	defer s.mu.Unlock()

	bodyW, bodyH, writeRow := s.bodyWriter()

	cols, rows, onTerminal := s.terminalSize()
	borderRows := 1
	if onTerminal {
		// Only draw the border when the body plus both border rows fit.
		if rows < bodyH+2 {
			borderRows = 0
		}
	} else if cols <= 0 {
		cols = 80
	}

	// When the window is too short, show the bottom of the display: the ROM's
	// prompt, reports and the boot message all live there.
	bodyRows := bodyH
	if avail := rows - 2*borderRows; onTerminal && avail < bodyH {
		bodyRows = avail
		if bodyRows < 1 {
			bodyRows = 1
		}
	}
	firstBodyRow := bodyH - bodyRows

	maxCols := bodyW
	if onTerminal && cols < maxCols {
		maxCols = cols
	}

	totalRows := 2*borderRows + bodyRows
	borderANSI := fmt.Sprintf("\033[%dm", 40+ansiColor(s.border, 0, false))

	var buf []byte
	buf = append(buf, "\033[H"...) // cursor home

	// endRow erases whatever the frame no longer covers using the colour still in
	// effect, resets the colours, and moves to the next row.
	//
	// The separator is CRLF rather than a bare LF because the emulator puts the
	// terminal in raw mode, and term.MakeRaw clears OPOST. With output processing
	// off nothing translates LF into a carriage return, so a bare LF only moves
	// the cursor down and the next row starts wherever the previous one ended: the
	// picture drifts right by one row width per row, the rows wrap round the
	// window and the erase bands leave a diagonal smear across the screen.
	//
	// Deliberately no separator after the final row: that is what pushes a
	// full-height frame over the bottom edge and makes the terminal scroll.
	rowIndex := 0
	endRow := func() {
		buf = append(buf, "\033[K"...)
		buf = append(buf, "\033[0m"...)
		rowIndex++
		if rowIndex < totalRows {
			buf = append(buf, "\r\n"...)
		}
	}
	borderRow := func() {
		buf = append(buf, borderANSI...)
		for j := 0; j < maxCols; j++ {
			buf = append(buf, ' ')
		}
		endRow()
	}

	for i := 0; i < borderRows; i++ {
		borderRow()
	}
	for i := 0; i < bodyRows; i++ {
		writeRow(s, firstBodyRow+i, maxCols, &buf)
		endRow()
	}
	for i := 0; i < borderRows; i++ {
		borderRow()
	}

	s.FrameCounter++
	return string(buf)
}

// ---------------------------------------------------------------------------
// 50Hz frame timing
// ---------------------------------------------------------------------------

// WaitFrame waits until frameDuration has elapsed since frameStart.
//
// The duration is passed in rather than assumed because the emulator's frame
// clock has to agree with the one its audio is generated from: if frames are
// paced at a different rate than the CPU cycles they contain, a sound card fed
// from those cycles slowly starves or overflows.
func WaitFrame(frameStart time.Time, frameDuration time.Duration) {
	if remaining := frameDuration - time.Since(frameStart); remaining > 0 {
		time.Sleep(remaining)
	}
}

// ---------------------------------------------------------------------------
// Terminal init/cleanup
// ---------------------------------------------------------------------------

// TermInit initializes the terminal for rendering: raw mode, alternate screen, hidden cursor.
//
// If stdin is not a terminal this is a no-op, so the emulator can still run with
// its output redirected to a pipe or file (the display is then a plain stream of
// frames). It returns a nil state in that case.
func TermInit() (*term.State, error) {
	if !term.IsTerminal(int(os.Stdin.Fd())) {
		return nil, nil
	}

	orig, err := term.MakeRaw(int(os.Stdin.Fd()))
	if err != nil {
		return nil, fmt.Errorf("term.MakeRaw: %w", err)
	}

	fmt.Print("\033[?1049h") // enter alternate screen
	fmt.Print("\033[2J")     // clear
	fmt.Print("\033[?25l")   // hide cursor

	return orig, nil
}

// TermCleanup restores the terminal to original state. A nil state means
// TermInit did not touch the terminal, so there is nothing to undo.
func TermCleanup(orig *term.State) {
	if orig == nil {
		return
	}
	fmt.Print("\033[?25h")   // show cursor
	fmt.Print("\033[?1049l") // exit alternate screen
	term.Restore(int(os.Stdin.Fd()), orig)
}

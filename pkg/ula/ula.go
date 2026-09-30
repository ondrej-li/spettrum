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

	// blinkFrames is how long one half of a flash cycle lasts. The Spectrum
	// flashes at 1.6Hz, and sixteen frames of 50Hz is half a cycle.
	blinkFrames = 16

	// selfHealFrames is how often delta rendering is interrupted by a whole
	// frame. A delta is only correct while the renderer and the terminal agree
	// on what is on the screen, and there is no way to ask; repainting
	// occasionally bounds how long any disagreement can survive.
	selfHealFrames = 250
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
	// RenderModeUnset is the zero value: nothing was chosen, and the emulator
	// resolves it to the OCR display. It has to exist, or choosing braille would
	// be indistinguishable from not choosing anything and would be overridden.
	RenderModeUnset RenderMode = iota
	RenderBraille
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

	// Incremental rendering. shadow is VRAM as it was last painted and dirty
	// marks the cells that differ from it; only those are repainted, which is
	// what keeps a still picture from being pushed to the terminal fifty times a
	// second. The prev* fields describe the frame the shadow belongs to, since a
	// delta is only valid while the picture is still being drawn in the same
	// place, in the same mode, after the same border.
	shadow     []uint8
	dirty      []bool
	havePrev   bool
	prevMode   RenderMode
	prevCols   int
	prevRows   int
	prevBorder uint8
	prevBlink  bool
	sinceFull  int

	// incremental selects delta rendering. It only applies when the output is a
	// terminal: a pipe or a file is given whole frames whatever it says.
	incremental bool
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
		VRAM:        vram,
		RenderMode:  mode,
		shadow:      make([]uint8, TotalVRAM),
		dirty:       make([]bool, OutputWidth*OutputHeight),
		incremental: true,
	}
}

// SetIncremental turns delta rendering on or off. Off sends the whole picture
// every frame, which is the renderer as it was before deltas existed; it is the
// escape hatch for a terminal that does not take the cursor moves well, and what
// --full-refresh asks for.
func (s *State) SetIncremental(on bool) {
	s.mu.Lock()
	s.incremental = on
	s.mu.Unlock()
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

// cellRenderer draws one cell of the body and returns the character to put there
// with the ANSI foreground and background colours to put it in.
//
// Both the whole-frame path and the delta path go through it, so a cell cannot
// come out differently depending on which one drew it.
type cellRenderer func(*State, int, int) (string, int, int)

// blinkPhase reports which half of the flash cycle a frame falls in. A flashing
// cell shows its ink and paper the other way round for one half of the cycle.
func (s *State) blinkPhase() bool {
	return (s.FrameCounter/blinkFrames)&1 == 0
}

// cellColours reads the attribute covering the pixel at (px, py) and returns the
// colours its cell is to be drawn in, with a flashing cell's ink and paper
// swapped in the half of the cycle where it is inverted.
func (s *State) cellColours(px, py int) (attr ColorAttr, ink, paper uint8) {
	attr = getAttr(s.VRAM, px, py)
	ink, paper = attr.Ink, attr.Paper
	if attr.Blink != 0 && !s.blinkPhase() {
		ink, paper = paper, ink
	}
	return attr, ink, paper
}

// ---------------------------------------------------------------------------
// Block mode rendering (2x2 → Unicode quadrant)
// ---------------------------------------------------------------------------

// renderBlockCell returns the cell of the 2x2 block display at (row, col): the
// four pixels at (2*col, 2*row) as one quadrant character.
func renderBlockCell(s *State, row, col int) (string, int, int) {
	px := col * 2
	py := row * 2

	tl := getPixel(s.VRAM, px, py)
	tr := getPixel(s.VRAM, px+1, py)
	bl := getPixel(s.VRAM, px, py+1)
	br := getPixel(s.VRAM, px+1, py+1)

	pattern := (tl << 3) | (tr << 2) | (bl << 1) | br
	attr, ink, paper := s.cellColours(px, py)
	return blockChars[pattern], ansiColor(ink, attr.Bright, true), ansiColor(paper, 0, false)
}

// ---------------------------------------------------------------------------
// Braille mode rendering (2x4 → Unicode Braille)
// ---------------------------------------------------------------------------

// renderBrailleCell returns the cell of the 2x4 braille display at (row, col):
// the eight pixels at (2*col, 4*row) as one braille character.
func renderBrailleCell(s *State, row, col int) (string, int, int) {
	vram := s.VRAM
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

	attr, ink, paper := s.cellColours(px, py)
	return string(braille), ansiColor(ink, attr.Bright, true), ansiColor(paper, 0, false)
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

// renderOCRCell returns the cell of the 8x8 display at (row, col): the character
// of the ROM's font that the cell's bitmap matches most closely.
func renderOCRCell(s *State, row, col int) (string, int, int) {
	vram := s.VRAM
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

	attr, ink, paper := s.cellColours(px, py)
	return out, ansiColor(ink, attr.Bright, true), ansiColor(paper, 0, false)
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

// bodyWriter returns the display size in characters and the per-cell renderer for
// the current render mode.
func (s *State) bodyWriter() (w, h int, render cellRenderer) {
	switch s.RenderMode {
	case RenderBlock:
		return OutputWidth, OutputHeight, renderBlockCell
	case RenderBraille:
		return BrailleOutputWidth, BrailleOutputHeight, renderBrailleCell
	default:
		return OCROutputWidth, OCROutputHeight, renderOCRCell
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

// frameGeometry is the shape of one frame: the first row of the body that is
// shown, and how much of the body fits in the window.
type frameGeometry struct {
	bodyW        int
	firstBodyRow int
	bodyRows     int
	maxCols      int
	borderRows   int
	onTerminal   bool
}

// RenderFrame renders what the terminal has to be sent to show the current VRAM,
// and returns it as the string to write out.
//
// When the output is a terminal the frame is fitted to it: the border is dropped
// unless the whole body plus borders fits, and the image is clipped to the window.
// A frame taller than the terminal (or a row wider than it) would wrap or scroll,
// and scrolling smears successive frames into each other so the picture appears to
// crawl diagonally.
//
// On a terminal a frame that differs from the last one in only a few cells is
// sent as those cells alone, each addressed with a cursor move, and a frame that
// differs in nothing is sent as nothing at all. That is what takes the renderer
// off the critical path: a still picture - an idle machine at its prompt, a
// program waiting for a key - costs one comparison per cell instead of a screen
// full of text, and no terminal bandwidth at all.
//
// When the output is not a terminal the frame is emitted at its natural size and
// always whole, so that captured frames stay self-contained: a stream of deltas
// cannot be read back without a screen to apply them to.
func (s *State) RenderFrame() string {
	s.mu.Lock()
	defer s.mu.Unlock()

	bodyW, bodyH, render := s.bodyWriter()

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

	maxCols := bodyW
	if onTerminal && cols < maxCols {
		maxCols = cols
	}

	g := frameGeometry{
		bodyW:        bodyW,
		firstBodyRow: bodyH - bodyRows,
		bodyRows:     bodyRows,
		maxCols:      maxCols,
		borderRows:   borderRows,
		onTerminal:   onTerminal,
	}

	blink := s.blinkPhase()

	// A delta is only valid while the last frame is still where it was put: the
	// same mode, the same window, the same border. A resize, a mode change, the
	// first frame, the periodic self-heal and --full-refresh all repaint whole.
	sameWindow := s.RenderMode == s.prevMode && cols == s.prevCols && rows == s.prevRows
	delta := s.incremental && onTerminal && s.havePrev && sameWindow &&
		s.sinceFull < selfHealFrames

	var buf []byte
	if delta {
		buf = s.appendDelta(buf, g, blink, render)
		if len(buf) > 0 {
			// The colours are left as the last cell asked for; put the terminal
			// back to its own so that nothing printed later inherits them.
			buf = append(buf, "\033[0m"...)
		}
	} else {
		// Drawing a window the last picture was not drawn in - a resize, or a
		// change of render mode - means clearing what the old frame left behind.
		buf = s.appendWholeFrame(buf, g, s.havePrev && !sameWindow, render)
		s.syncShadow()
	}

	s.prevMode, s.prevCols, s.prevRows = s.RenderMode, cols, rows
	s.prevBorder, s.prevBlink = s.border, blink
	s.havePrev = true
	if delta {
		s.sinceFull++
	} else {
		s.sinceFull = 0
	}

	s.FrameCounter++
	return string(buf)
}

// appendWholeFrame draws every visible cell, in order, from the current VRAM.
// When clear is set it starts by emptying the window, which is what a frame that
// lands somewhere the last one did not needs.
func (s *State) appendWholeFrame(buf []byte, g frameGeometry, clear bool, render cellRenderer) []byte {
	totalRows := 2*g.borderRows + g.bodyRows
	borderANSI := fmt.Sprintf("\033[%dm", 40+ansiColor(s.border, 0, false))

	buf = append(buf, "\033[H"...) // cursor home
	if g.onTerminal && clear {
		buf = append(buf, "\033[2J"...)
	}

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
		for j := 0; j < g.maxCols; j++ {
			buf = append(buf, ' ')
		}
		endRow()
	}

	for i := 0; i < g.borderRows; i++ {
		borderRow()
	}
	for i := 0; i < g.bodyRows; i++ {
		row := g.firstBodyRow + i
		last := uint32(noColour)
		for col := 0; col < g.maxCols; col++ {
			ch, fg, bg := render(s, row, col)
			buf = appendCell(buf, &last, fg, bg, ch)
		}
		endRow()
	}
	for i := 0; i < g.borderRows; i++ {
		borderRow()
	}
	return buf
}

// appendDelta repaints only the cells that have changed since the last frame, and
// draws nothing at all when none have.
//
// Every run of changed cells is addressed with an absolute cursor move, so a
// delta never contains a newline: it cannot scroll the window however tall it is,
// and it does not care that the terminal is in raw mode, where a bare LF moves
// the cursor down without returning it to the first column.
func (s *State) appendDelta(buf []byte, g frameGeometry, blink bool, render cellRenderer) []byte {
	borderChanged := g.borderRows > 0 && s.border != s.prevBorder
	if !s.markChangedCells(g, blink) && !borderChanged {
		return buf
	}

	if borderChanged {
		borderANSI := fmt.Sprintf("\033[%dm", 40+ansiColor(s.border, 0, false))
		for r := 1; r <= g.borderRows; r++ {
			buf = appendBorderRow(buf, r, g.maxCols, borderANSI)
		}
		for r := 1 + g.borderRows + g.bodyRows; r <= 2*g.borderRows+g.bodyRows; r++ {
			buf = appendBorderRow(buf, r, g.maxCols, borderANSI)
		}
	}

	for i := 0; i < g.bodyRows; i++ {
		row := g.firstBodyRow + i
		absRow := 1 + g.borderRows + i
		base := row * g.bodyW
		for col := 0; col < g.maxCols; {
			if !s.dirty[base+col] {
				col++
				continue
			}
			start := col
			for col < g.maxCols && s.dirty[base+col] {
				col++
			}
			buf = appendCursorTo(buf, absRow, start+1)
			last := uint32(noColour)
			for c := start; c < col; c++ {
				ch, fg, bg := render(s, row, c)
				buf = appendCell(buf, &last, fg, bg, ch)
			}
		}
	}
	return buf
}

// markChangedCells marks every cell that differs from the one painted last time
// and, when the flash phase has just turned, every cell that flashes. It reports
// whether anything was marked.
//
// The comparison is per 8x8 attribute cell, which is the coarsest thing the three
// render modes have in common: OCR draws one character for it, block mode four by
// four and braille four across by two down. Marking the whole rectangle for one
// changed byte is a few cells more than strictly needed and many fewer than the
// screen, and it keeps the three modes on one path.
func (s *State) markChangedCells(g frameGeometry, blink bool) bool {
	clear(s.dirty)
	marked := false
	flashed := blink != s.prevBlink

	for ar := 0; ar < AttrRows; ar++ {
		for ac := 0; ac < AttrCols; ac++ {
			if !s.cellChanged(ar, ac) && !(flashed && s.blinks(ar, ac)) {
				continue
			}
			r0, r1, c0, c1 := s.attrCellRect(ar, ac)
			for r := r0; r <= r1; r++ {
				for c := c0; c <= c1; c++ {
					s.dirty[r*g.bodyW+c] = true
				}
			}
			marked = true
		}
	}
	return marked
}

// cellChanged reports whether the 8x8 cell with attribute coordinates (ar, ac)
// differs from the one that was painted, and records the current bytes in the
// shadow so that the next frame compares against this one.
//
// A cell's bytes are its eight bitmap bytes - one per pixel row, spaced 256 apart
// because of the way the display file is interleaved - and its attribute byte.
func (s *State) cellChanged(ar, ac int) bool {
	base := (ar/8)*2048 + (ar%8)*32 + ac
	attrAddr := VRAMSize + ar*AttrCols + ac

	changed := false
	for i := 0; i < 8; i++ {
		addr := base + i*256
		if v := s.vramByte(addr); s.shadow[addr] != v {
			s.shadow[addr] = v
			changed = true
		}
	}
	if v := s.vramByte(attrAddr); s.shadow[attrAddr] != v {
		s.shadow[attrAddr] = v
		changed = true
	}
	return changed
}

// attrCellRect returns the range of display cells the 8x8 pixels of attribute
// cell (ar, ac) are drawn as in the current render mode.
func (s *State) attrCellRect(ar, ac int) (r0, r1, c0, c1 int) {
	switch s.RenderMode {
	case RenderBlock:
		return ar * 4, ar*4 + 3, ac * 4, ac*4 + 3
	case RenderBraille:
		return ar * 2, ar*2 + 1, ac * 4, ac*4 + 3
	default:
		return ar, ar, ac, ac
	}
}

// vramByte reads a byte of the display file, treating anything past the end of
// the buffer as blank - which is what the pixel reader does with it too.
func (s *State) vramByte(addr int) uint8 {
	if addr < 0 || addr >= len(s.VRAM) {
		return 0
	}
	return s.VRAM[addr]
}

// blinks reports whether the attribute of cell (ar, ac) asks for flashing.
func (s *State) blinks(ar, ac int) bool {
	return s.vramByte(VRAMSize+ar*AttrCols+ac)&AttrBlink != 0
}

// syncShadow records the whole of VRAM as painted, which is what a frame that
// draws every cell leaves behind.
func (s *State) syncShadow() {
	n := copy(s.shadow, s.VRAM)
	for i := n; i < len(s.shadow); i++ {
		s.shadow[i] = 0
	}
}

// appendCursorTo moves the terminal's cursor to a one-based row and column.
func appendCursorTo(buf []byte, row, col int) []byte {
	buf = append(buf, "\033["...)
	buf = strconv.AppendInt(buf, int64(row), 10)
	buf = append(buf, ';')
	buf = strconv.AppendInt(buf, int64(col), 10)
	return append(buf, 'H')
}

// appendBorderRow paints one row of border across the width of the window.
func appendBorderRow(buf []byte, absRow, maxCols int, borderANSI string) []byte {
	buf = appendCursorTo(buf, absRow, 1)
	buf = append(buf, borderANSI...)
	for i := 0; i < maxCols; i++ {
		buf = append(buf, ' ')
	}
	return buf
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

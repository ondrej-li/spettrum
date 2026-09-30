package ula

import (
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"
)

// cell is one character a frame put on the screen and where it put it.
type cell struct {
	row, col int // one-based, as the terminal counts
	ch       rune
}

// painted replays a frame - its cursor moves and the characters it wrote - and
// returns every character with the position it landed at. Replaying is the only
// way to know what a delta does: what matters is not which bytes it holds but
// which cells it changes.
func painted(frame string) []cell {
	var cells []cell
	row, col := 0, 0
	for i := 0; i < len(frame); {
		if frame[i] == 0x1b && i+1 < len(frame) && frame[i+1] == '[' {
			j := i + 2
			for j < len(frame) && !(frame[j] >= 0x40 && frame[j] <= 0x7e) {
				j++
			}
			if j == len(frame) {
				break
			}
			if frame[j] == 'H' || frame[j] == 'f' {
				row, col = parseCursor(frame[i+2 : j])
			}
			i = j + 1
			continue
		}
		r, size := utf8.DecodeRuneInString(frame[i:])
		i += size
		switch r {
		case '\r':
			col = 1
		case '\n':
			row++
		default:
			cells = append(cells, cell{row: row, col: col, ch: r})
			col++
		}
	}
	return cells
}

// cursorMoves counts how often a frame addresses the cursor.
func cursorMoves(frame string) int {
	moves := 0
	for i := 0; i+1 < len(frame); i++ {
		if frame[i] != 0x1b || frame[i+1] != '[' {
			continue
		}
		j := i + 2
		for j < len(frame) && !(frame[j] >= 0x40 && frame[j] <= 0x7e) {
			j++
		}
		if j < len(frame) && (frame[j] == 'H' || frame[j] == 'f') {
			moves++
		}
	}
	return moves
}

// parseCursor reads a cursor-position parameter list. An empty one means "home",
// which is row 1, column 1.
func parseCursor(params string) (row, col int) {
	row, col = 1, 1
	fields := strings.Split(params, ";")
	if len(fields) > 0 {
		if v, err := strconv.Atoi(fields[0]); err == nil && v > 0 {
			row = v
		}
	}
	if len(fields) > 1 {
		if v, err := strconv.Atoi(fields[1]); err == nil && v > 0 {
			col = v
		}
	}
	return row, col
}

// newState returns a state showing a blank display in a window of the given size,
// with the first - necessarily whole - frame already drawn. The VRAM slice is
// returned so that a test can change the display the way the CPU would.
func newState(t *testing.T, mode RenderMode, cols, rows int) (*State, []byte) {
	t.Helper()
	vram := make([]byte, TotalVRAM)
	s := New(vram, mode)
	s.SetTerminalSize(cols, rows)
	if first := s.RenderFrame(); !strings.Contains(first, "\x1b[H") {
		t.Fatalf("the first frame is not a whole frame: %q", first)
	}
	return s, vram
}

// attrAddr is where the attribute byte of the cell at (row, col) lives.
func attrAddr(row, col int) int { return VRAMSize + row*AttrCols + col }

// pixelAddr is where the byte holding the eight pixels of display line y at
// character column charCol lives. The display file is interleaved, so the three
// parts of the address are not next to each other.
func pixelAddr(y, charCol int) int {
	return (y/64)*2048 + ((y%64)/8)*32 + charCol + (y%8)*256
}

// TestUnchangedFrameIsNotSent is the point of the exercise: a picture that has
// not changed costs nothing at all on the wire.
func TestUnchangedFrameIsNotSent(t *testing.T) {
	s, _ := newState(t, RenderOCR, 40, 30)
	for i := 0; i < 5; i++ {
		if frame := s.RenderFrame(); frame != "" {
			t.Fatalf("frame %d of an unchanged display was written: %q", i, frame)
		}
	}
}

// TestChangedAttributeRepaintsOneOCRCell pins the granularity in the mode where
// one cell is one character: a changed attribute must repaint that character and
// nothing else.
func TestChangedAttributeRepaintsOneOCRCell(t *testing.T) {
	s, vram := newState(t, RenderOCR, 40, 30)
	const ar, ac = 5, 7
	vram[attrAddr(ar, ac)] = AttrPaper | 2 // red paper

	cells := painted(s.RenderFrame())
	if len(cells) != 1 {
		t.Fatalf("repainted %d cells, want 1: %v", len(cells), cells)
	}
	// The border row sits above the body, so body row 0 is screen row 2.
	if cells[0].row != 2+ar || cells[0].col != 1+ac {
		t.Errorf("repainted cell %d;%d, want %d;%d", cells[0].row, cells[0].col, 2+ar, 1+ac)
	}
}

// TestChangedAttributeRepaintsItsBlockOfCells pins the other side of the
// granularity: the 8x8 pixels of one attribute cell are drawn as a 4x4 block of
// quadrant characters, so all sixteen of them come back.
func TestChangedAttributeRepaintsItsBlockOfCells(t *testing.T) {
	s, vram := newState(t, RenderBlock, 140, 100)
	const ar, ac = 3, 4
	vram[attrAddr(ar, ac)] = AttrInk | AttrPaper

	frame := s.RenderFrame()
	cells := painted(frame)
	if len(cells) != 16 {
		t.Fatalf("repainted %d cells, want the 4x4 block of one attribute cell", len(cells))
	}
	for _, c := range cells {
		row, col := c.row-2, c.col-1 // the border shifts the body down one row
		if row < ar*4 || row > ar*4+3 || col < ac*4 || col > ac*4+3 {
			t.Errorf("repainted cell %d;%d, outside attribute cell (%d,%d)'s 4x4 block",
				c.row, c.col, ar, ac)
		}
	}
	// Four rows of four cells: one cursor move per run, not per cell.
	if moves := cursorMoves(frame); moves != 4 {
		t.Errorf("the block took %d cursor moves, want one per row (4)", moves)
	}
}

// TestChangedAttributeRepaintsItsBrailleCells checks the third mapping: braille
// draws an attribute cell as four characters across and two down.
func TestChangedAttributeRepaintsItsBrailleCells(t *testing.T) {
	s, vram := newState(t, RenderBraille, 140, 52)
	const ar, ac = 6, 2
	vram[attrAddr(ar, ac)] = AttrInk | 7

	cells := painted(s.RenderFrame())
	if len(cells) != 8 {
		t.Fatalf("repainted %d cells, want the 4x2 block of one attribute cell", len(cells))
	}
	for _, c := range cells {
		row, col := c.row-2, c.col-1
		if row < ar*2 || row > ar*2+1 || col < ac*4 || col > ac*4+3 {
			t.Errorf("repainted cell %d;%d, outside attribute cell (%d,%d)'s 4x2 block",
				c.row, c.col, ar, ac)
		}
	}
}

// TestChangedBitmapByteRepaintsOnlyItsCell covers the other half of the
// comparison: the display file, not just the attributes.
func TestChangedBitmapByteRepaintsOnlyItsCell(t *testing.T) {
	s, vram := newState(t, RenderOCR, 40, 30)
	const ar, ac = 20, 30
	vram[pixelAddr(ar*8+3, ac)] = 0xFF

	cells := painted(s.RenderFrame())
	if len(cells) != 1 {
		t.Fatalf("repainted %d cells for one changed byte, want 1: %v", len(cells), cells)
	}
	if cells[0].row != 2+ar || cells[0].col != 1+ac {
		t.Errorf("repainted cell %d;%d, want %d;%d", cells[0].row, cells[0].col, 2+ar, 1+ac)
	}
}

// TestRunOfChangedCellsIsOneCursorMove pins that a row of changed cells is one
// run: addressing every cell separately would cost more than the cells do.
func TestRunOfChangedCellsIsOneCursorMove(t *testing.T) {
	s, vram := newState(t, RenderOCR, 40, 30)
	for ac := 10; ac < 18; ac++ {
		vram[attrAddr(12, ac)] = AttrPaper | 3
	}

	frame := s.RenderFrame()
	if cells := painted(frame); len(cells) != 8 {
		t.Fatalf("repainted %d cells, want the 8 that changed", len(cells))
	}
	if moves := cursorMoves(frame); moves != 1 {
		t.Errorf("eight neighbouring cells took %d cursor moves, want 1", moves)
	}
}

// TestBlinkTurnRepaintsOnlyBlinkingCells: nothing in VRAM changes when a
// flashing cell inverts, so the flash phase has to be part of the comparison or
// the flash would never be seen.
func TestBlinkTurnRepaintsOnlyBlinkingCells(t *testing.T) {
	s, vram := newState(t, RenderOCR, 40, 30)
	vram[attrAddr(2, 3)] = AttrBlink | AttrInk | 7
	vram[attrAddr(4, 5)] = AttrBlink | AttrPaper
	s.RenderFrame() // draw the attributes just written

	// The phase turns every blinkFrames frames and nothing changes in between,
	// so every frame up to the turn has to be empty; the turn itself redraws
	// exactly the cells that flash.
	turned := ""
	for i := 0; i < blinkFrames; i++ {
		if frame := s.RenderFrame(); frame != "" {
			turned = frame
			break
		}
	}
	if turned == "" {
		t.Fatalf("the flash did not turn within %d frames", blinkFrames)
	}
	cells := painted(turned)
	if len(cells) != 2 {
		t.Fatalf("the flash turn repainted %d cells, want the 2 that flash: %v",
			len(cells), cells)
	}
	for i, want := range []cell{{row: 4, col: 4}, {row: 6, col: 6}} {
		if cells[i].row != want.row || cells[i].col != want.col {
			t.Errorf("the flash turn repainted cell %d;%d, want %d;%d",
				cells[i].row, cells[i].col, want.row, want.col)
		}
	}
}

// TestBorderChangeRepaintsTheBorderRows: the border colour lives outside VRAM, so
// it has to be compared too.
func TestBorderChangeRepaintsTheBorderRows(t *testing.T) {
	s, _ := newState(t, RenderOCR, 40, 30)
	s.SetBorderColor(2)

	cells := painted(s.RenderFrame())
	// One border row above and below a 24-row body, each as wide as the body.
	if want := 2 * OCROutputWidth; len(cells) != want {
		t.Fatalf("repainted %d cells, want the %d of two border rows", len(cells), want)
	}
	for _, c := range cells {
		if c.row != 1 && c.row != 2+OCROutputHeight {
			t.Errorf("repainted cell %d;%d, which is not on a border row", c.row, c.col)
		}
	}
}

// TestResizeIsRepaintedWhole: a delta assumes the picture is still where the last
// frame put it, and a resize moves it.
func TestResizeIsRepaintedWhole(t *testing.T) {
	s, _ := newState(t, RenderOCR, 40, 30)
	s.SetTerminalSize(60, 30)

	frame := s.RenderFrame()
	if !strings.Contains(frame, "\x1b[2J") {
		t.Errorf("a resize did not clear what the old frame left behind: %q", frame)
	}
	if cells, want := painted(frame), 26*OCROutputWidth; len(cells) != want {
		t.Errorf("a resize repainted %d cells, want the %d of a whole frame", len(cells), want)
	}
}

// TestWholeFrameEverySoOften bounds how long the picture can be wrong: a delta
// stream is only as good as the renderer's idea of what is on the screen.
func TestWholeFrameEverySoOften(t *testing.T) {
	s, _ := newState(t, RenderOCR, 40, 30)

	for i := 0; i < selfHealFrames; i++ {
		if frame := s.RenderFrame(); frame != "" {
			t.Fatalf("frame %d of the delta run was written: %q", i, frame)
		}
	}
	if frame := s.RenderFrame(); !strings.Contains(frame, "\x1b[H") {
		t.Errorf("no whole frame after %d frames: %q", selfHealFrames, frame)
	}
}

// TestSetIncrementalOffRestoresWholeFrames is the escape hatch behind
// --full-refresh.
func TestSetIncrementalOffRestoresWholeFrames(t *testing.T) {
	s, _ := newState(t, RenderOCR, 40, 30)
	s.SetIncremental(false)

	frame := s.RenderFrame()
	if !strings.Contains(frame, "\x1b[H") || len(painted(frame)) != 26*OCROutputWidth {
		t.Errorf("--full-refresh did not draw a whole frame: %q", frame)
	}
	s.SetIncremental(true)
	if frame := s.RenderFrame(); frame != "" {
		t.Errorf("deltas did not resume: %q", frame)
	}
}

// TestNonTerminalOutputIsAlwaysWholeFrames: a pipe or a file has no screen to
// apply deltas to, so it gets the whole picture every time, and identical
// pictures stay byte for byte identical.
func TestNonTerminalOutputIsAlwaysWholeFrames(t *testing.T) {
	s := New(make([]byte, TotalVRAM), RenderOCR)

	first := s.RenderFrame()
	second := s.RenderFrame()
	if first == "" || second == "" {
		t.Fatal("a frame was left out of a stream that has no screen to apply one to")
	}
	if first != second {
		t.Error("two identical frames were written differently")
	}
	if !strings.HasPrefix(first, "\x1b[H") {
		t.Errorf("the frame does not start at the home position: %q", first)
	}
	if strings.Contains(first, "\x1b[2J") || cursorMoves(first) != 1 {
		t.Error("the frame is a delta, or expects a screen that is already dirty")
	}
}

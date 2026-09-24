package emulator

import (
	"bytes"
	"os"
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/defik74/spettrum/pkg/ula"
)

// termModel is a minimal terminal: enough of the control sequence, auto-wrap and
// scroll behaviour to reproduce what a real terminal displays for a byte stream.
// It exists because "does the frame fit and stay put?" cannot be checked by
// looking at the bytes alone — wrapping and scrolling only manifest on screen.
type termModel struct {
	cols, rows int
	grid       [][]rune
	x, y       int

	wrapped  bool // a printable character landed past the last column
	scrolled int  // the screen scrolled up
	scrollY  int  // row that was current when the first scroll happened
}

func newTermModel(cols, rows int) *termModel {
	m := &termModel{cols: cols, rows: rows}
	m.grid = make([][]rune, rows)
	for i := range m.grid {
		m.grid[i] = make([]rune, cols)
		for j := range m.grid[i] {
			m.grid[i][j] = ' '
		}
	}
	return m
}

func (m *termModel) put(r rune) {
	if m.x >= m.cols {
		m.wrapped = true
		m.x = 0
		m.y++
	}
	if m.y >= m.rows {
		if m.scrolled == 0 {
			m.scrollY = m.y
		}
		m.scrolled++
		copy(m.grid, m.grid[1:])
		last := make([]rune, m.cols)
		for i := range last {
			last[i] = ' '
		}
		m.grid[m.rows-1] = last
		m.y = m.rows - 1
	}
	m.grid[m.y][m.x] = r
	m.x++
}

func (m *termModel) feed(s string) {
	for i := 0; i < len(s); {
		if s[i] == 0x1b && i+1 < len(s) && s[i+1] == '[' {
			// Consume the whole CSI sequence: parameters, then a final byte.
			j := i + 2
			for j < len(s) && !(s[j] >= 0x40 && s[j] <= 0x7e) {
				j++
			}
			if j < len(s) {
				switch s[j] {
				case 'H', 'f':
					m.moveTo(s[i+2 : j])
				case 'K':
					// erase to end of line: blanks the rest of the row, no movement
					if m.y >= 0 && m.y < m.rows {
						for k := m.x; k < m.cols; k++ {
							m.grid[m.y][k] = ' '
						}
					}
				}
				i = j + 1
				continue
			}
		}
		r, size := utf8.DecodeRuneInString(s[i:])
		i += size
		switch {
		case r == '\r':
			// Carriage return: first column, same row.
			m.x = 0
		case r == '\n':
			// Line feed moves down only. The emulator leaves the terminal in raw
			// mode, where OPOST is cleared, so nothing translates LF into CRLF and
			// a bare LF would leave the cursor in the middle of the line.
			m.y++
			if m.y >= m.rows {
				m.scroll()
			}
		case r >= 0x20 && r != 0x7f:
			m.put(r)
		}
	}
}

// moveTo handles a cursor-position sequence. An empty parameter list means home.
func (m *termModel) moveTo(params string) {
	row, col := 1, 1
	if params != "" {
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
	}
	m.x, m.y = col-1, row-1
}

// scroll drops the top row and blanks the bottom one, as a terminal does when
// output runs off the last line.
func (m *termModel) scroll() {
	if m.scrolled == 0 {
		m.scrollY = m.y
	}
	m.scrolled++
	copy(m.grid, m.grid[1:])
	last := make([]rune, m.cols)
	for i := range last {
		last[i] = ' '
	}
	m.grid[m.rows-1] = last
	m.y = m.rows - 1
}

// screen renders the visible grid as text lines.
func (m *termModel) screen() []string {
	out := make([]string, m.rows)
	for i, row := range m.grid {
		out[i] = strings.TrimRight(string(row), " ")
	}
	return out
}

// TestTerminalFrameGeometry replays the real frame stream into a terminal model
// and checks that frames neither wrap nor scroll, and that the boot screen ends
// up showing the ROM's message. Wrapping or scrolling is what smears successive
// frames together and makes the picture look torn.
func TestTerminalFrameGeometry(t *testing.T) {
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available: %v", err)
	}

	for _, tc := range []struct {
		name       string
		cols, rows int
	}{
		{"80x24", 80, 24},
		{"80x25", 80, 25},
		{"80x30", 80, 30},
		{"120x30", 120, 30},
		{"100x24", 100, 24},
		{"40x24", 40, 24},
		{"32x24", 32, 24},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var out bytes.Buffer
			emu := New(Config{
				ROMFile:      realROMPath,
				RenderMode:   ula.RenderOCR,
				Output:       &out,
				NoTerminal:   true,
				Unpaced:      true,
				Audio:        false,
				Instructions: 3_000_000,
			})
			if err := emu.Init(); err != nil {
				t.Fatalf("Init: %v", err)
			}
			defer emu.Close()
			emu.display.SetTerminalSize(tc.cols, tc.rows)
			if err := emu.Run(); err != nil {
				t.Fatalf("Run: %v", err)
			}

			model := newTermModel(tc.cols, tc.rows)
			frames := splitFrames(out.String())
			distinct := map[string]bool{}
			messageRows := map[int]int{}
			messageFrames := 0
			seenMessage := false
			droppedMessage := 0
			for _, frame := range frames {
				// splitFrames drops the delimiter, so put the cursor-home back.
				model.feed("\x1b[H" + frame)
				screen := strings.Join(model.screen(), "\n")
				distinct[screen] = true
				present := false
				for i, line := range model.screen() {
					if strings.Contains(line, "Sinclair") {
						present = true
						messageRows[i]++
						messageFrames++
					}
				}
				if present {
					seenMessage = true
				} else if seenMessage {
					// Once the ROM has painted the boot message it must stay on
					// screen. It flashing or vanishing is the reported symptom of
					// memory corruption rather than of rendering.
					droppedMessage++
				}
			}

			if droppedMessage != 0 {
				t.Errorf("boot message disappeared again in %d of %d frames",
					droppedMessage, len(frames))
			}

			if model.wrapped {
				t.Error("output wrapped: a row is wider than the terminal")
			}
			if model.scrolled != 0 {
				t.Errorf("output scrolled the terminal %d times (first at row %d)",
					model.scrolled, model.scrollY)
			}

			final := model.screen()
			if !strings.Contains(strings.Join(final, "\n"), "1982 Sinclair Research Ltd") {
				t.Errorf("boot message not on the final screen:\n%s", strings.Join(final, "\n"))
			}
			t.Logf("%d frames, %d distinct screens, message on %d of %d frames",
				len(frames), len(distinct), messageFrames, len(frames))
			for row, count := range messageRows {
				t.Logf("message appeared on screen row %d in %d frames", row, count)
			}
		})
	}
}

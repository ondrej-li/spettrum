package emulator

import (
	"bytes"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/defik74/spettrum/pkg/ula"
)

// ansisPattern matches the SGR colour sequences the renderer emits.
var ansiPattern = regexp.MustCompile(`\x1b\[[0-9;]*[A-Za-z]`)

// realROMPath is the full 16K ZX Spectrum 48K ROM image kept in the repository.
var realROMPath = filepath.Join("..", "..", "rom", "ZX_Spectrum_48k.rom")

// splitFrames splits a rendered stream into frames at the cursor-home sequence.
func splitFrames(rendered string) []string {
	parts := strings.Split(rendered, "\x1b[H")
	if len(parts) > 0 && parts[0] == "" {
		parts = parts[1:]
	}
	return parts
}

// frameLines returns the visible lines of a frame with the escape sequences
// removed. Rows are separated by CRLF, so the CR has to go too. A fitted frame
// has no trailing separator, so the count is exact.
func frameLines(frame string) []string {
	plain := ansiPattern.ReplaceAllString(frame, "")
	plain = strings.ReplaceAll(plain, "\r\n", "\n")
	return strings.Split(plain, "\n")
}

// assertCRLFRows fails if any row separator is a bare LF. The terminal is in raw
// mode, where output processing is off and an LF moves the cursor down without
// returning it to the first column, so a bare LF makes the picture drift right by
// one row width per row.
func assertCRLFRows(t *testing.T, frame string) {
	t.Helper()
	for i := 0; i < len(frame); i++ {
		if frame[i] != '\n' {
			continue
		}
		if i == 0 || frame[i-1] != '\r' {
			t.Errorf("row separator at byte %d is a bare LF; rows must be separated by CRLF", i)
			return
		}
	}
}

// lastFrame returns the final rendered frame as plain-text lines.
func lastFrame(rendered string) []string {
	frames := splitFrames(rendered)
	if len(frames) == 0 {
		return nil
	}
	return frameLines(frames[len(frames)-1])
}

// TestBootsRealROMInOCRMode is the end-to-end acceptance test: it loads the real
// ROM image from rom/, runs it through the OCR renderer and requires the boot
// message to be on screen. Reaching that message means the whole chain works:
// CPU, memory, ULA frame interrupts, the ROM's screen driver and the renderer.
func TestBootsRealROMInOCRMode(t *testing.T) {
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available at %s: %v", realROMPath, err)
	}

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

	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	frame := lastFrame(out.String())
	screen := strings.Join(frame, "\n")
	if !strings.Contains(screen, "1982 Sinclair Research Ltd") {
		t.Fatalf("boot message not on screen after %d instructions; last frame was:\n%s",
			emu.totalInst, screen)
	}

	// The message belongs on the lower part of the display (the ROM prints it on
	// the line above the "0 OK" report), not somewhere random.
	msgRow := -1
	for i, line := range frame {
		if strings.Contains(line, "1982 Sinclair Research Ltd") {
			msgRow = i
		}
	}
	if msgRow < 18 {
		t.Errorf("boot message rendered at frame row %d, expected the lower screen", msgRow)
	}

	// The rest of the display must be blank: the ROM clears it at boot.
	for i, line := range frame {
		if i == msgRow {
			continue
		}
		if strings.TrimSpace(line) != "" {
			t.Errorf("unexpected content on frame row %d: %q", i, line)
		}
	}
}

// TestOCRFrameFitsTerminal covers the geometry that made the boot screen look
// torn in a real terminal: an 80x24 window cannot hold a 24-row body plus a
// top and bottom border row, so a 26-row frame scrolled the screen and successive
// frames smeared into each other. Frames must never exceed the window, and a row
// must never exceed its width, or the terminal wraps it.
func TestOCRFrameFitsTerminal(t *testing.T) {
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available at %s: %v", realROMPath, err)
	}

	cases := []struct {
		name        string
		cols, rows  int
		wantRows    int
		wantMessage bool
	}{
		{"80x24 has no room for a border", 80, 24, 24, true},
		{"80x25 has no room for a border", 80, 25, 24, true},
		{"80x30 fits body and border", 80, 30, 26, true},
		{"80x26 fits body and border exactly", 80, 26, 26, true},
		{"20x24 clips the width", 20, 24, 24, false},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var out bytes.Buffer
			emu := New(Config{
				ROMFile:      realROMPath,
				RenderMode:   ula.RenderOCR,
				Output:       &out,
				NoTerminal:   true,
				Unpaced:      true,
				Audio:        false,
				Instructions: 2_000_000,
			})
			if err := emu.Init(); err != nil {
				t.Fatalf("Init: %v", err)
			}
			defer emu.Close()

			// Pretend we are attached to a window of this size.
			emu.display.SetTerminalSize(tc.cols, tc.rows)

			if err := emu.Run(); err != nil {
				t.Fatalf("Run: %v", err)
			}

			frames := splitFrames(out.String())
			if len(frames) < 2 {
				t.Fatalf("expected several frames, got %d", len(frames))
			}
			last := frames[len(frames)-1]
			if strings.HasSuffix(last, "\n") {
				t.Error("frame ends with a newline, which scrolls a full-height terminal")
			}
			assertCRLFRows(t, last)

			lines := frameLines(last)
			if len(lines) != tc.wantRows {
				t.Errorf("frame is %d rows in a %dx%d terminal, want %d",
					len(lines), tc.cols, tc.rows, tc.wantRows)
			}
			if len(lines) > tc.rows {
				t.Errorf("frame is taller than the terminal (%d > %d)", len(lines), tc.rows)
			}
			for i, line := range lines {
				if w := utf8.RuneCountInString(line); w > tc.cols {
					t.Errorf("row %d is %d wide in a %d column terminal", i, w, tc.cols)
				}
			}

			screen := strings.Join(lines, "\n")
			hasMessage := strings.Contains(screen, "1982 Sinclair Research Ltd")
			if hasMessage != tc.wantMessage {
				t.Errorf("message present = %v, want %v; screen:\n%s", hasMessage, tc.wantMessage, screen)
			}
		})
	}
}

// TestBootsRealROMInBlockMode runs the same ROM through the block renderer as a
// smoke test for the other render path.
func TestBootsRealROMInBlockMode(t *testing.T) {
	if _, err := os.Stat(realROMPath); err != nil {
		t.Skipf("ROM image not available at %s: %v", realROMPath, err)
	}

	var out bytes.Buffer
	emu := New(Config{
		ROMFile:      realROMPath,
		RenderMode:   ula.RenderBlock,
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
	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}

	// Block mode draws with quadrants, so look for ink: at least one non-space,
	// non-border cell must be present.
	frame := lastFrame(out.String())
	ink := 0
	for _, line := range frame {
		for _, r := range line {
			switch r {
			case ' ', '\n':
			default:
				ink++
			}
		}
	}
	if ink == 0 {
		t.Errorf("block renderer produced an empty frame:\n%s", strings.Join(frame, "\n"))
	}
}

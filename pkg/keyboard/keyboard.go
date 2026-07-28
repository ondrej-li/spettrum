// Package keyboard provides ZX Spectrum keyboard matrix emulation.
// Maps host keyboard input to the Spectrum's 8×5 key matrix.
package keyboard

import (
	"os"
	"time"
)

// ---------------------------------------------------------------------------
// Spectrum keyboard matrix (8 rows × 5 columns)
// ---------------------------------------------------------------------------

// SpectrumKey codes for special keys.
const (
	KeyCapsShift   = 0x10
	KeySymbolShift = 0x11
)

// Key matrix: row → [5]column keys
var keyMatrix = [8][5]byte{
	{0x10, 'Z', 'X', 'C', 'V'},           // Row 0: SHIFT, Z, X, C, V
	{'A', 'S', 'D', 'F', 'G'},             // Row 1: A, S, D, F, G
	{'Q', 'W', 'E', 'R', 'T'},             // Row 2: Q, W, E, R, T
	{'1', '2', '3', '4', '5'},             // Row 3: 1, 2, 3, 4, 5
	{'0', '9', '8', '7', '6'},             // Row 4: 0, 9, 8, 7, 6
	{'P', 'O', 'I', 'U', 'Y'},             // Row 5: P, O, I, U, Y
	{0x0D, 'L', 'K', 'J', 'H'},            // Row 6: ENTER, L, K, J, H
	{' ', KeySymbolShift, 'M', 'N', 'B'},  // Row 7: SPACE, SYMBOL SHIFT, M, N, B
}

const (
	maxPressedKeys = 64
	keyHoldTime    = 100 * time.Millisecond
)

// pressedKey tracks a currently-held key and its press time.
type pressedKey struct {
	key       byte
	timestamp time.Time
}

// State holds the keyboard state.
type State struct {
	pressed   [maxPressedKeys]pressedKey
	count     int
	rowSelect uint8 // current row selector (bits 0-7: 0 = active)
}

// New creates a new keyboard state.
func New() *State {
	return &State{rowSelect: 0xFF} // no row selected
}

// SetRowSelector sets the current keyboard row selector.
// The upper byte of port 0xFE selects which row(s) to scan.
func (s *State) SetRowSelector(sel uint8) {
	s.rowSelect = sel
}

// RowSelector returns the current row selector.
func (s *State) RowSelector() uint8 {
	return s.rowSelect
}

// addKey adds a key to the pressed set (deduplicates).
func (s *State) addKey(key byte) {
	// Check if already pressed
	for i := 0; i < s.count; i++ {
		if s.pressed[i].key == key {
			return // already pressed
		}
	}
	if s.count < maxPressedKeys {
		s.pressed[s.count] = pressedKey{key: key, timestamp: time.Now()}
		s.count++
	}
}

// isPressed checks if a key is currently in the pressed set.
func (s *State) isPressed(key byte) bool {
	for i := 0; i < s.count; i++ {
		if s.pressed[i].key == key {
			return true
		}
	}
	return false
}

// expireKeys removes keys that have been held longer than keyHoldTime.
func (s *State) expireKeys() {
	now := time.Now()
	j := 0
	for i := 0; i < s.count; i++ {
		if now.Sub(s.pressed[i].timestamp) < keyHoldTime {
			s.pressed[j] = s.pressed[i]
			j++
		}
	}
	s.count = j
}

// TranslateKey converts a host character into Spectrum key injection.
// Handles uppercase (CAPS SHIFT + letter), special chars (SYMBOL SHIFT), etc.
func (s *State) TranslateKey(ch byte) {
	switch {
	// Tab → CAPS SHIFT + SYMBOL SHIFT (Extended Mode)
	case ch == '\t' || ch == 0x09:
		s.addKey(KeyCapsShift)
		s.addKey(KeySymbolShift)

	// Backspace / DEL → CAPS SHIFT + '0'
	case ch == 0x08 || ch == 0x7F:
		s.addKey(KeyCapsShift)
		s.addKey('0')

	// Uppercase letters → CAPS SHIFT + lowercase
	case ch >= 'A' && ch <= 'Z':
		s.addKey(KeyCapsShift)
		s.addKey(ch + 32) // lowercase

	// Special characters requiring SYMBOL SHIFT
	case ch == ',':
		s.addKey(KeySymbolShift)
		s.addKey('n')
	case ch == '.':
		// Actually SYMBOL SHIFT + M, but handled below
		s.addKey(KeySymbolShift)
		s.addKey('m')
	case ch == ':':
		s.addKey(KeySymbolShift)
		s.addKey('z')
	case ch == ';':
		s.addKey(KeySymbolShift)
		s.addKey('o')
	case ch == '?':
		s.addKey(KeySymbolShift)
		s.addKey('c')
	case ch == '!':
		s.addKey(KeySymbolShift)
		s.addKey('1')
	case ch == '"':
		s.addKey(KeySymbolShift)
		s.addKey('p')
	case ch == '#':
		s.addKey(KeySymbolShift)
		s.addKey('3')
	case ch == '$':
		s.addKey(KeySymbolShift)
		s.addKey('4')
	case ch == '%':
		s.addKey(KeySymbolShift)
		s.addKey('5')
	case ch == '&':
		s.addKey(KeySymbolShift)
		s.addKey('6')
	case ch == '\'':
		s.addKey(KeySymbolShift)
		s.addKey('7')
	case ch == '(':
		s.addKey(KeySymbolShift)
		s.addKey('8')
	case ch == ')':
		s.addKey(KeySymbolShift)
		s.addKey('9')
	case ch == '*':
		s.addKey(KeySymbolShift)
		s.addKey('b')
	case ch == '+':
		s.addKey(KeySymbolShift)
		s.addKey('k')
	case ch == '-':
		s.addKey(KeySymbolShift)
		s.addKey('j')
	case ch == '/':
		s.addKey(KeySymbolShift)
		s.addKey('v')
	case ch == '=':
		s.addKey(KeySymbolShift)
		s.addKey('l')
	case ch == '<':
		s.addKey(KeySymbolShift)
		s.addKey('r')
	case ch == '>':
		s.addKey(KeySymbolShift)
		s.addKey('t')
	case ch == '@':
		s.addKey(KeySymbolShift)
		s.addKey('2')
	case ch == '^':
		s.addKey(KeySymbolShift)
		s.addKey('h')
	case ch == '_':
		s.addKey(KeySymbolShift)
		s.addKey('0')

	// Arrow keys (via escape sequences; simplified — just direct codes)
	case ch == 0x80: // Up
		s.addKey(KeyCapsShift)
		s.addKey('7')
	case ch == 0x81: // Down
		s.addKey(KeyCapsShift)
		s.addKey('6')
	case ch == 0x82: // Left
		s.addKey(KeyCapsShift)
		s.addKey('5')
	case ch == 0x83: // Right
		s.addKey(KeyCapsShift)
		s.addKey('8')

	// Enter
	case ch == '\r' || ch == '\n':
		s.addKey(0x0D)

	default:
		s.addKey(ch)
	}
}

// PollStdin reads any pending input from stdin (non-blocking) and translates to Spectrum keys.
func (s *State) PollStdin() {
	// Read available bytes from stdin (non-blocking because terminal is in raw mode)
	var buf [32]byte
	for {
		n, err := os.Stdin.Read(buf[:])
		if err != nil || n == 0 {
			break
		}
		for i := 0; i < n; i++ {
			s.TranslateKey(buf[i])
		}
	}
}

// ReadPort reads from a keyboard port. The port's upper byte contains the row selector.
// Returns the column state: 0 = key pressed, 1 = released. Bits 5-7 always 1.
func (s *State) ReadPort(port uint16) uint8 {
	s.expireKeys()
	s.PollStdin()

	rowSel := byte(port >> 8)
	result := uint8(0xFF) // all bits 1 = no key pressed

	for row := 0; row < 8; row++ {
		if rowSel&(1<<row) == 0 { // row is selected (active low)
			for col := 0; col < 5; col++ {
				if s.isPressed(keyMatrix[row][col]) {
					result &^= 1 << col // clear bit = key pressed
				}
			}
		}
	}

	return result
}

// InjectKey injects a key press programmatically (for testing/scripting).
func (s *State) InjectKey(ch byte) {
	s.addKey(ch)
}

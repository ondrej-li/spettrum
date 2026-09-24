// Package keyboard provides ZX Spectrum keyboard matrix emulation.
// Maps host keyboard input to the Spectrum's 8×5 key matrix.
package keyboard

import (
	"os"
	"sync"
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
	{'A', 'S', 'D', 'F', 'G'},            // Row 1: A, S, D, F, G
	{'Q', 'W', 'E', 'R', 'T'},            // Row 2: Q, W, E, R, T
	{'1', '2', '3', '4', '5'},            // Row 3: 1, 2, 3, 4, 5
	{'0', '9', '8', '7', '6'},            // Row 4: 0, 9, 8, 7, 6
	{'P', 'O', 'I', 'U', 'Y'},            // Row 5: P, O, I, U, Y
	{0x0D, 'L', 'K', 'J', 'H'},           // Row 6: ENTER, L, K, J, H
	{' ', KeySymbolShift, 'M', 'N', 'B'}, // Row 7: SPACE, SYMBOL SHIFT, M, N, B
}

const (
	maxPressedKeys = 64

	// keyHoldCycles is how long a host key press is held, measured in CPU
	// T-states so that it tracks emulated time rather than wall-clock time.
	// Raw terminal input only reports presses (never releases), so the emulator
	// has to release them itself; 250000 T-states is about 70ms at 3.5MHz.
	keyHoldCycles = 250_000
)

// pressedKey tracks a currently-held key and the T-state it was pressed at.
type pressedKey struct {
	key        byte
	pressCycle uint64
}

// State holds the keyboard state. It is safe for concurrent use: the input
// goroutine writes to it while the CPU goroutine reads it.
type State struct {
	mu      sync.Mutex
	pressed [maxPressedKeys]pressedKey
	count   int
	now     uint64 // current CPU T-state, advanced by Tick
	started bool
	quit    bool // set when the user asks the emulator to stop
}

// New creates a new keyboard state.
func New() *State {
	return &State{}
}

// Tick records the current CPU T-state. The emulator calls this once per frame
// so that key presses are timed in emulated time.
func (s *State) Tick(cycles uint64) {
	s.mu.Lock()
	s.now = cycles
	s.mu.Unlock()
}

// QuitRequested reports whether the user asked the emulator to stop. Raw mode
// disables the terminal's signal handling, so Ctrl+C arrives as a byte rather
// than as SIGINT and has to be turned into a shutdown request here.
func (s *State) QuitRequested() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.quit
}

// StartInput reads the host keyboard on a dedicated goroutine.
//
// This must never be done from the CPU's I/O path: a terminal read blocks until
// a key arrives, so polling stdin while emulating would stall the CPU for as
// long as the user is idle. The goroutine exits on stdin error or EOF.
func (s *State) StartInput() {
	s.mu.Lock()
	if s.started {
		s.mu.Unlock()
		return
	}
	s.started = true
	s.mu.Unlock()

	go func() {
		var buf [32]byte
		for {
			n, err := os.Stdin.Read(buf[:])
			if n > 0 {
				s.mu.Lock()
				for i := 0; i < n; i++ {
					s.translateKeyLocked(buf[i])
				}
				s.mu.Unlock()
			}
			if err != nil {
				return
			}
		}
	}()
}

// addKey adds a key to the pressed set (deduplicates).
// The caller must hold s.mu.
func (s *State) addKey(key byte) {
	// The matrix holds the key legends in upper case ('Z', 'X', CAPS SHIFT...), so a
	// letter has to be folded before it is looked up. Adding a lower-case letter
	// matches no key at all, which silently drops every letter and every SYMBOL
	// SHIFT combination that is a letter (SYMBOL SHIFT + M for '.', and so on).
	if key >= 'a' && key <= 'z' {
		key -= 32
	}

	// Check if already pressed
	for i := 0; i < s.count; i++ {
		if s.pressed[i].key == key {
			return // already pressed
		}
	}
	if s.count < maxPressedKeys {
		s.pressed[s.count] = pressedKey{key: key, pressCycle: s.now}
		s.count++
	}
}

// isPressed checks if a key is currently in the pressed set.
// The caller must hold s.mu.
func (s *State) isPressed(key byte) bool {
	for i := 0; i < s.count; i++ {
		if s.pressed[i].key == key {
			return true
		}
	}
	return false
}

// expireKeys releases keys that have been held for keyHoldCycles T-states.
// The caller must hold s.mu.
func (s *State) expireKeys() {
	j := 0
	for i := 0; i < s.count; i++ {
		if s.now-s.pressed[i].pressCycle < keyHoldCycles {
			s.pressed[j] = s.pressed[i]
			j++
		}
	}
	s.count = j
}

// TranslateKey converts a host character into Spectrum key injection.
// Handles uppercase (CAPS SHIFT + letter), special chars (SYMBOL SHIFT), etc.
// The caller must hold s.mu.
func (s *State) translateKeyLocked(ch byte) {
	switch {
	// Ctrl+C asks the emulator to stop.
	case ch == 0x03:
		s.quit = true

	// Tab → CAPS SHIFT + SYMBOL SHIFT (Extended Mode)
	case ch == '\t' || ch == 0x09:
		s.addKey(KeyCapsShift)
		s.addKey(KeySymbolShift)

	// Backspace / DEL → CAPS SHIFT + '0'
	case ch == 0x08 || ch == 0x7F:
		s.addKey(KeyCapsShift)
		s.addKey('0')

	// Uppercase letters → CAPS SHIFT + letter
	case ch >= 'A' && ch <= 'Z':
		s.addKey(KeyCapsShift)
		s.addKey(ch)

	// Lowercase letters → the letter on its own
	case ch >= 'a' && ch <= 'z':
		s.addKey(ch)

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

// ReadPort reads from a keyboard port. The port's upper byte contains the row selector.
// Returns the column state: 0 = key pressed, 1 = released. Bits 5-7 always 1.
// It performs no host I/O, so it is safe to call from the emulation loop.
func (s *State) ReadPort(port uint16) uint8 {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.expireKeys()

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

// TranslateKey converts a host character into Spectrum key injection.
// Handles uppercase (CAPS SHIFT + letter), special chars (SYMBOL SHIFT), etc.
func (s *State) TranslateKey(ch byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.translateKeyLocked(ch)
}

// InjectKey injects a key press programmatically (for testing/scripting).
// Raw matrix codes pass through unchanged; host characters are translated.
func (s *State) InjectKey(ch byte) {
	s.TranslateKey(ch)
}

/**
 * Spectrum Keyboard Handler Implementation
 *
 * Implements the authentic Spectrum keyboard matrix scanning:
 * - Port 0xFE controls row selection (upper byte = row bits)
 * - IN (C), B reads back column bits from selected row
 * - Active-low logic: bit is 0 when key pressed, 1 when released
 *
 * The keyboard matrix (8 rows x 5 columns):
 * Row 0 (0xFE): SHIFT, Z, X, C, V
 * Row 1 (0xFD): A, S, D, F, G
 * Row 2 (0xFB): Q, W, E, R, T
 * Row 3 (0xF7): 1, 2, 3, 4, 5
 * Row 4 (0xEF): 0, 9, 8, 7, 6
 * Row 5 (0xDF): P, O, I, U, Y
 * Row 6 (0xBF): ENTER, L, K, J, H
 * Row 7 (0x7F): SPACE, SYMBOL SHIFT, M, N, B
 *
 * Host keyboard is mapped to Spectrum keys via character matching.
 */

#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

// Key state buffer - tracks which ASCII/keycodes are currently pressed
#define MAX_PRESSED_KEYS 64
static unsigned char pressed_keys[MAX_PRESSED_KEYS];
static uint64_t key_timestamps[MAX_PRESSED_KEYS]; // Timestamp when key was added (for auto-release)
static int num_pressed_keys = 0;

// Key hold duration in milliseconds before auto-release
#define KEY_HOLD_TIME_MS 100

// Last row selector written via OUT (C), B
static uint8_t current_row_selector = 0xFF; // Start with no row selected

/**
 * Get current time in milliseconds
 */
static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Update the row selector (called when CPU executes OUT to port 0xFE)
 * The upper byte of the port address contains the row selector bits
 */
void keyboard_set_row_selector(uint8_t row_selector)
{
    current_row_selector = row_selector;
}

/**
 * Get current row selector
 */
uint8_t keyboard_get_row_selector(void)
{
    return current_row_selector;
}

/**
 * Check if a key is currently in the pressed set
 */
static int is_key_pressed_state(unsigned char key)
{
    for (int i = 0; i < num_pressed_keys; i++)
    {
        if (pressed_keys[i] == key)
            return 1;
    }
    return 0;
}

// Forward declaration
static void add_pressed_key(unsigned char key);

/**
 * Translate a character input into appropriate Spectrum key(s)
 * Handles uppercase (via CAPS SHIFT + letter), special characters (via SYMBOL SHIFT + key),
 * and special keys like backspace and arrows.
 */
static void translate_and_add_key(unsigned char ch)
{
    // Backspace: CAPS SHIFT + 0
    if (ch == 0x08 || ch == 0x7F) // Backspace or DEL
    {
        add_pressed_key(0x10); // CAPS SHIFT
        add_pressed_key('0');
        return;
    }

    // Arrow keys via CAPS SHIFT + number:
    // Up: CAPS SHIFT + 7, Down: CAPS SHIFT + 6, Left: CAPS SHIFT + 5, Right: CAPS SHIFT + 8
    // These are mapped to special key codes for easier terminal handling
    if (ch == 'A' - 'A' + 1 || ch == 27) // ESC or special escape marker
    {
        // This would need special handling for multi-byte escape sequences
        // For now, we support direct key codes for arrows (if passed from command-line)
        return;
    }

    // Direct arrow key support via special key codes (for programmatic input)
    // Up arrow: code 128, Down: 129, Left: 130, Right: 131
    if (ch == 128) // UP
    {
        add_pressed_key(0x10); // CAPS SHIFT
        add_pressed_key('7');
        return;
    }
    if (ch == 129) // DOWN
    {
        add_pressed_key(0x10); // CAPS SHIFT
        add_pressed_key('6');
        return;
    }
    if (ch == 130) // LEFT
    {
        add_pressed_key(0x10); // CAPS SHIFT
        add_pressed_key('5');
        return;
    }
    if (ch == 131) // RIGHT
    {
        add_pressed_key(0x10); // CAPS SHIFT
        add_pressed_key('8');
        return;
    }

    // Uppercase letters: add CAPS SHIFT (0x10) + the lowercase letter
    if (ch >= 'A' && ch <= 'Z')
    {
        add_pressed_key(0x10);    // CAPS SHIFT
        add_pressed_key(ch + 32); // Convert to lowercase
        return;
    }

    // Special characters mapped via SYMBOL SHIFT (0x11) + their key
    // Format: character -> lowercase letter key to press with SYMBOL SHIFT
    struct
    {
        unsigned char ch;
        unsigned char key;
    } special_chars[] = {
        {',', 'n'},  // Comma
        {'.', 'm'},  // Period
        {'-', 'j'},  // Minus/Dash
        {'=', 'l'},  // Equals
        {'_', 'a'},  // Underscore
        {':', 'z'},  // Colon
        {'?', 'c'},  // Question mark
        {'@', 'q'},  // At sign
        {'#', '3'},  // Hash
        {'$', '4'},  // Dollar
        {'~', '2'},  // Tilde
        {'^', 'h'},  // Caret
        {'&', '6'},  // Ampersand
        {'*', 'b'},  // Asterisk
        {'{', 'y'},  // Open brace
        {'}', 'u'},  // Close brace
        {'[', 'd'},  // Open bracket
        {']', 'g'},  // Close bracket
        {';', 'o'},  // Semicolon
        {'\'', 'p'}, // Quote
        {0, 0}       // Terminator
    };

    // Check special characters
    for (int i = 0; special_chars[i].ch != 0; i++)
    {
        if (ch == special_chars[i].ch)
        {
            add_pressed_key(0x11); // SYMBOL SHIFT (represented as 0x11)
            add_pressed_key(special_chars[i].key);
            return;
        }
    }

    // Regular character: add as-is (for lowercase letters, digits, space, etc.)
    add_pressed_key(ch);
}

/**
 * Update key states: remove any keys that have expired (held for KEY_HOLD_TIME_MS)
 */
static void update_key_states(void)
{
    uint64_t now = get_time_ms();
    int write_idx = 0;

    for (int i = 0; i < num_pressed_keys; i++)
    {
        // Check if this key has expired
        if (now - key_timestamps[i] < KEY_HOLD_TIME_MS)
        {
            // Keep this key
            pressed_keys[write_idx] = pressed_keys[i];
            key_timestamps[write_idx] = key_timestamps[i];
            write_idx++;
        }
    }

    num_pressed_keys = write_idx;
}

/**
 * Add a key to the pressed set (simulated key press)
 * Keys will auto-release after KEY_HOLD_TIME_MS
 */
static void add_pressed_key(unsigned char key)
{
    if (num_pressed_keys < MAX_PRESSED_KEYS)
    {
        // Check if already present
        if (!is_key_pressed_state(key))
        {
            pressed_keys[num_pressed_keys] = key;
            key_timestamps[num_pressed_keys] = get_time_ms();
            num_pressed_keys++;
        }
    }
}

/**
 * Poll stdin and update pressed key state (non-blocking)
 * This allows the emulator to simulate keyboard input from the terminal
 */
static void keyboard_poll_stdin(void)
{
    unsigned char ch;
    int nread;

    while (1)
    {
        nread = read(STDIN_FILENO, &ch, 1);
        if (nread != 1)
            break; // No more input available

        // Translate the input character to appropriate Spectrum key(s)
        translate_and_add_key(ch);
    }
}

/**
 * Initialize keyboard
 */
int keyboard_init(void)
{
    current_row_selector = 0xFF;
    num_pressed_keys = 0;

    return 0;
}

/**
 * Cleanup keyboard
 */
void keyboard_cleanup(void)
{
    num_pressed_keys = 0;
    current_row_selector = 0xFF;
}

/**
 * Set a simulated key for testing (for command-line key injection)
 * Translates the character input to appropriate Spectrum key(s) with support for:
 * - Uppercase letters (via CAPS SHIFT + letter)
 * - Special characters (via SYMBOL SHIFT + key)
 *
 * @param key The ASCII character to simulate as pressed
 */
void keyboard_set_simulated_key(char key)
{
    if (key != 0)
    {
        translate_and_add_key((unsigned char)key);
    }
}

/**
 * Get keyboard state for the specified row(s) (encoded in 16-bit port address)
 *
 * The Spectrum ROM reads keyboard using IN A,(0xFE) where the full 16-bit address is:
 *   - Low byte (0xFE): Selects the ULA port
 *   - High byte: Row selector bitmask (from A register)
 *
 * Row selector bits (active-low, each bit selects a row when LOW):
 *   Bit 0 low (0xFE): Row 0 - SHIFT, Z, X, C, V
 *   Bit 1 low (0xFD): Row 1 - A, S, D, F, G
 *   Bit 2 low (0xFB): Row 2 - Q, W, E, R, T
 *   Bit 3 low (0xF7): Row 3 - 1, 2, 3, 4, 5
 *   Bit 4 low (0xEF): Row 4 - 0, 9, 8, 7, 6
 *   Bit 5 low (0xDF): Row 5 - ENTER, L, K, J, H
 *   Bit 6 low (0xBF): Row 6 - SPACE, SYMBOL SHIFT, M, N, B
 *   Bit 7 low (0x7F): Row 7 - P, O, I, U, Y
 *
 * Multiple rows can be selected simultaneously by having multiple bits low.
 * Results from all selected rows are ANDed together.
 *
 * @param port Full 16-bit port address (high byte = row selector)
 * @return 8-bit value: bits 0-4 = key states (0=pressed), bits 5-7 = always 1
 */
uint8_t keyboard_read_port(uint16_t port)
{
    // Start with all keys released (0xFF = 0b11111111)
    uint8_t result = 0xFF;

    // Extract the row selector from the HIGH byte of the port address
    uint8_t selector = (port >> 8) & 0xFF;

    // Update key states: remove expired keys
    update_key_states();

    // Poll stdin for any new key input
    keyboard_poll_stdin();

    // Check each row - multiple rows can be selected at once (when bit is LOW)
    // Results from all selected rows are ANDed together

    // Row 0: SHIFT, Z, X, C, V (bit 0 is low)
    if (!(selector & 0x01))
    {
        if (is_key_pressed_state(16) || is_key_pressed_state('\x10'))
            result &= ~0x01; // SHIFT
        if (is_key_pressed_state('z') || is_key_pressed_state('Z'))
            result &= ~0x02; // Z
        if (is_key_pressed_state('x') || is_key_pressed_state('X'))
            result &= ~0x04; // X
        if (is_key_pressed_state('c') || is_key_pressed_state('C'))
            result &= ~0x08; // C
        if (is_key_pressed_state('v') || is_key_pressed_state('V'))
            result &= ~0x10; // V
    }

    // Row 1: A, S, D, F, G (bit 1 is low)
    if (!(selector & 0x02))
    {
        // Bit 0: A
        if (is_key_pressed_state('a') || is_key_pressed_state('A'))
        {
            result &= ~0x01;
        }
        // Bit 1: S
        if (is_key_pressed_state('s') || is_key_pressed_state('S'))
        {
            result &= ~0x02;
        }
        // Bit 2: D
        if (is_key_pressed_state('d') || is_key_pressed_state('D'))
        {
            result &= ~0x04;
        }
        // Bit 3: F
        if (is_key_pressed_state('f') || is_key_pressed_state('F'))
        {
            result &= ~0x08;
        }
        // Bit 4: G
        if (is_key_pressed_state('g') || is_key_pressed_state('G'))
        {
            result &= ~0x10;
        }
    }
    // Row 2: Q, W, E, R, T (bit 2 is low)
    if (!(selector & 0x04))
    {
        // Bit 0: Q
        if (is_key_pressed_state('q') || is_key_pressed_state('Q'))
        {
            result &= ~0x01;
        }
        // Bit 1: W
        if (is_key_pressed_state('w') || is_key_pressed_state('W'))
        {
            result &= ~0x02;
        }
        // Bit 2: E
        if (is_key_pressed_state('e') || is_key_pressed_state('E'))
        {
            result &= ~0x04;
        }
        // Bit 3: R
        if (is_key_pressed_state('r') || is_key_pressed_state('R'))
        {
            result &= ~0x08;
        }
        // Bit 4: T
        if (is_key_pressed_state('t') || is_key_pressed_state('T'))
        {
            result &= ~0x10;
        }
    }
    // Row 3: 1, 2, 3, 4, 5 (bit 3 is low)
    if (!(selector & 0x08))
    {
        // Bit 0: 1
        if (is_key_pressed_state('1'))
        {
            result &= ~0x01;
        }
        // Bit 1: 2
        if (is_key_pressed_state('2'))
        {
            result &= ~0x02;
        }
        // Bit 2: 3
        if (is_key_pressed_state('3'))
        {
            result &= ~0x04;
        }
        // Bit 3: 4
        if (is_key_pressed_state('4'))
        {
            result &= ~0x08;
        }
        // Bit 4: 5
        if (is_key_pressed_state('5'))
        {
            result &= ~0x10;
        }
    }
    // Row 4: 0, 9, 8, 7, 6 (bit 4 is low)
    if (!(selector & 0x10))
    {
        // Bit 0: 0
        if (is_key_pressed_state('0'))
        {
            result &= ~0x01;
        }
        // Bit 1: 9
        if (is_key_pressed_state('9'))
        {
            result &= ~0x02;
        }
        // Bit 2: 8
        if (is_key_pressed_state('8'))
        {
            result &= ~0x04;
        }
        // Bit 3: 7
        if (is_key_pressed_state('7'))
        {
            result &= ~0x08;
        }
        // Bit 4: 6
        if (is_key_pressed_state('6'))
        {
            result &= ~0x10;
        }
    }
    // Row 5: P, O, I, U, Y (bit 5 is low)
    if (!(selector & 0x20))
    {
        // Bit 0: P
        if (is_key_pressed_state('p') || is_key_pressed_state('P'))
        {
            result &= ~0x01;
        }
        // Bit 1: O
        if (is_key_pressed_state('o') || is_key_pressed_state('O'))
        {
            result &= ~0x02;
        }
        // Bit 2: I
        if (is_key_pressed_state('i') || is_key_pressed_state('I'))
        {
            result &= ~0x04;
        }
        // Bit 3: U
        if (is_key_pressed_state('u') || is_key_pressed_state('U'))
        {
            result &= ~0x08;
        }
        // Bit 4: Y
        if (is_key_pressed_state('y') || is_key_pressed_state('Y'))
        {
            result &= ~0x10;
        }
    }
    // Row 6: ENTER, L, K, J, H (bit 6 is low)
    if (!(selector & 0x40))
    {
        // Bit 0: ENTER
        if (is_key_pressed_state('\r') || is_key_pressed_state('\n') || is_key_pressed_state(13))
        {
            result &= ~0x01;
        }
        // Bit 1: L
        if (is_key_pressed_state('l') || is_key_pressed_state('L'))
        {
            result &= ~0x02;
        }
        // Bit 2: K
        if (is_key_pressed_state('k') || is_key_pressed_state('K'))
        {
            result &= ~0x04;
        }
        // Bit 3: J
        if (is_key_pressed_state('j') || is_key_pressed_state('J'))
        {
            result &= ~0x08;
        }
        // Bit 4: H
        if (is_key_pressed_state('h') || is_key_pressed_state('H'))
        {
            result &= ~0x10;
        }
    }
    // Row 7: SPACE, SYMBOL SHIFT, M, N, B (bit 7 is low)
    if (!(selector & 0x80))
    {
        // Bit 0: SPACE
        if (is_key_pressed_state(' '))
        {
            result &= ~0x01;
        }
        // Bit 1: SYMBOL SHIFT (CTRL on modern keyboards, or 0x11 from character translation)
        if (is_key_pressed_state(17) || is_key_pressed_state(0x11))
        {
            result &= ~0x02;
        }
        // Bit 2: M
        if (is_key_pressed_state('m') || is_key_pressed_state('M'))
        {
            result &= ~0x04;
        }
        // Bit 3: N
        if (is_key_pressed_state('n') || is_key_pressed_state('N'))
        {
            result &= ~0x08;
        }
        // Bit 4: B
        if (is_key_pressed_state('b') || is_key_pressed_state('B'))
        {
            result &= ~0x10;
        }
    }

    // Per Spectrum ROM spec, bits 5-7 should be set to 1
    result |= 0xE0;

    return result;
}

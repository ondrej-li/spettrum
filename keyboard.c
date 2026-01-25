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
 * Row 5 (0xDF): ENTER, L, K, J, H
 * Row 6 (0xBF): SPACE, SYMBOL SHIFT, M, N, B
 * Row 7 (0x7F): (alternate/duplicate row)
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

// Key state buffer - tracks which ASCII/keycodes are currently pressed
#define MAX_PRESSED_KEYS 64
static unsigned char pressed_keys[MAX_PRESSED_KEYS];
static int num_pressed_keys = 0;

// Last row selector written via OUT (C), B
static uint8_t current_row_selector = 0xFF; // Start with no row selected

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

/**
 * Add a key to the pressed set (simulated key press)
 */
static void add_pressed_key(unsigned char key)
{
    if (num_pressed_keys < MAX_PRESSED_KEYS)
    {
        // Check if already present
        if (!is_key_pressed_state(key))
        {
            pressed_keys[num_pressed_keys++] = key;
        }
    }
}

/**
 * Get human-readable name for a key
 */
static const char *keyboard_key_name(unsigned char key)
{
    switch (key)
    {
    case 13: // ASCII 13 = CR/ENTER
    case '\n':
        return "NEWLINE";
    case ' ':
        return "SPACE";
    case 16:
        return "SHIFT";
    case 17:
        return "CTRL/SYMBOL_SHIFT";
    default:
        if (key >= 32 && key < 127)
        {
            static char buf[2];
            buf[0] = (char)key;
            buf[1] = '\0';
            return buf;
        }
        return "UNKNOWN";
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

        // Sound bell and print debug info when key is pressed
        fprintf(stderr, "\a[KEY] Host keycode: 0x%02X (%3d) -> '%s'\n", ch, ch, keyboard_key_name(ch));
        fflush(stderr);

        // For now, treat any input as a key press
        // In a real implementation, you'd track key up/down separately
        add_pressed_key(ch);
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
 * Adds the key to the pressed set so it will be detected in subsequent scans
 *
 * @param key The ASCII character to simulate as pressed
 */
void keyboard_set_simulated_key(char key)
{
    if (key != 0)
    {
        add_pressed_key((unsigned char)key);
    }
}

/**
 * Get keyboard state for the current row selection
 *
 * The Spectrum ROM code OUT (C), B to select a row, then IN A, (C) to read it.
 * Each row returns bits 0-4 representing the 5 keys in that row.
 * Active-low: bit = 0 when key pressed, 1 when released.
 *
 * The row selector B contains:
 *   %11111110 (0xFE) selects Row 0: SHIFT, Z, X, C, V
 *   %11111101 (0xFD) selects Row 1: A, S, D, F, G
 *   %11111011 (0xFB) selects Row 2: Q, W, E, R, T
 *   %11110111 (0xF7) selects Row 3: 1, 2, 3, 4, 5
 *   %11101111 (0xEF) selects Row 4: 0, 9, 8, 7, 6
 *   %11011111 (0xDF) selects Row 5: ENTER, L, K, J, H
 *   %10111111 (0xBF) selects Row 6: SPACE, SYMBOL SHIFT, M, N, B
 *   %01111111 (0x7F) selects Row 7: (alternate/duplicate)
 *
 * Returns: 8-bit value (only bits 0-4 are used)
 *         (0 = key pressed, 1 = key released; bits 5-7 = 1 per ROM spec)
 */
uint8_t keyboard_read_port(uint8_t port)
{
    (void)port; // Not used; we use current_row_selector instead

    // Start with all keys released (0xFF = 0b11111111)
    uint8_t result = 0xFF;
    uint8_t selector = current_row_selector;

    // Poll stdin for any new key input
    keyboard_poll_stdin();

    // Check which row is selected (one bit low = active) and return key states
    // The ROM code rotates the selector, so check bit positions

    if (!(selector & 0x01)) // Row 0: SHIFT, Z, X, C, V (bit 0 is low)
    {
        // Bit 0: SHIFT
        if (is_key_pressed_state(16) || is_key_pressed_state('\x10'))
        {
            fprintf(stderr, "  -> SHIFT\n");
            result &= ~0x01;
        }

        // Bit 1: Z
        if (is_key_pressed_state('z') || is_key_pressed_state('Z'))
        {
            fprintf(stderr, "  -> Z\n");
            result &= ~0x02;
        }

        // Bit 2: X
        if (is_key_pressed_state('x') || is_key_pressed_state('X'))
        {
            fprintf(stderr, "  -> X\n");
            result &= ~0x04;
        }

        // Bit 3: C
        if (is_key_pressed_state('c') || is_key_pressed_state('C'))
        {
            fprintf(stderr, "  -> C\n");
            result &= ~0x08;
        }

        // Bit 4: V
        if (is_key_pressed_state('v') || is_key_pressed_state('V'))
        {
            fprintf(stderr, "  -> V\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x02)) // Row 1: A, S, D, F, G (bit 1 is low)
    {
        // Bit 0: A
        if (is_key_pressed_state('a') || is_key_pressed_state('A'))
        {
            fprintf(stderr, "  -> A\n");
            result &= ~0x01;
        }
        // Bit 1: S
        if (is_key_pressed_state('s') || is_key_pressed_state('S'))
        {
            fprintf(stderr, "  -> S\n");
            result &= ~0x02;
        }
        // Bit 2: D
        if (is_key_pressed_state('d') || is_key_pressed_state('D'))
        {
            fprintf(stderr, "  -> D\n");
            result &= ~0x04;
        }
        // Bit 3: F
        if (is_key_pressed_state('f') || is_key_pressed_state('F'))
        {
            fprintf(stderr, "  -> F\n");
            result &= ~0x08;
        }
        // Bit 4: G
        if (is_key_pressed_state('g') || is_key_pressed_state('G'))
        {
            fprintf(stderr, "  -> G\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x04)) // Row 2: Q, W, E, R, T (bit 2 is low)
    {
        // Bit 0: Q
        if (is_key_pressed_state('q') || is_key_pressed_state('Q'))
        {
            fprintf(stderr, "  -> Q\n");
            result &= ~0x01;
        }
        // Bit 1: W
        if (is_key_pressed_state('w') || is_key_pressed_state('W'))
        {
            fprintf(stderr, "  -> W\n");
            result &= ~0x02;
        }
        // Bit 2: E
        if (is_key_pressed_state('e') || is_key_pressed_state('E'))
        {
            fprintf(stderr, "  -> E\n");
            result &= ~0x04;
        }
        // Bit 3: R
        if (is_key_pressed_state('r') || is_key_pressed_state('R'))
        {
            fprintf(stderr, "  -> R\n");
            result &= ~0x08;
        }
        // Bit 4: T
        if (is_key_pressed_state('t') || is_key_pressed_state('T'))
        {
            fprintf(stderr, "  -> T\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x08)) // Row 3: 1, 2, 3, 4, 5 (bit 3 is low)
    {
        // Bit 0: 1
        if (is_key_pressed_state('1'))
        {
            fprintf(stderr, "  -> 1\n");
            result &= ~0x01;
        }
        // Bit 1: 2
        if (is_key_pressed_state('2'))
        {
            fprintf(stderr, "  -> 2\n");
            result &= ~0x02;
        }
        // Bit 2: 3
        if (is_key_pressed_state('3'))
        {
            fprintf(stderr, "  -> 3\n");
            result &= ~0x04;
        }
        // Bit 3: 4
        if (is_key_pressed_state('4'))
        {
            fprintf(stderr, "  -> 4\n");
            result &= ~0x08;
        }
        // Bit 4: 5
        if (is_key_pressed_state('5'))
        {
            fprintf(stderr, "  -> 5\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x10)) // Row 4: 0, 9, 8, 7, 6 (bit 4 is low)
    {
        // Bit 0: 0
        if (is_key_pressed_state('0'))
        {
            fprintf(stderr, "  -> 0\n");
            result &= ~0x01;
        }
        // Bit 1: 9
        if (is_key_pressed_state('9'))
        {
            fprintf(stderr, "  -> 9\n");
            result &= ~0x02;
        }
        // Bit 2: 8
        if (is_key_pressed_state('8'))
        {
            fprintf(stderr, "  -> 8\n");
            result &= ~0x04;
        }
        // Bit 3: 7
        if (is_key_pressed_state('7'))
        {
            fprintf(stderr, "  -> 7\n");
            result &= ~0x08;
        }
        // Bit 4: 6
        if (is_key_pressed_state('6'))
        {
            fprintf(stderr, "  -> 6\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x20)) // Row 5: ENTER, L, K, J, H (bit 5 is low)
    {
        // Bit 0: ENTER
        if (is_key_pressed_state('\r') || is_key_pressed_state('\n') || is_key_pressed_state(13))
        {
            fprintf(stderr, "  -> ENTER\n");
            result &= ~0x01;
        }
        // Bit 1: L
        if (is_key_pressed_state('l') || is_key_pressed_state('L'))
        {
            fprintf(stderr, "  -> L\n");
            result &= ~0x02;
        }
        // Bit 2: K
        if (is_key_pressed_state('k') || is_key_pressed_state('K'))
        {
            fprintf(stderr, "  -> K\n");
            result &= ~0x04;
        }
        // Bit 3: J
        if (is_key_pressed_state('j') || is_key_pressed_state('J'))
        {
            fprintf(stderr, "  -> J\n");
            result &= ~0x08;
        }
        // Bit 4: H
        if (is_key_pressed_state('h') || is_key_pressed_state('H'))
        {
            fprintf(stderr, "  -> H\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x40)) // Row 6: SPACE, SYMBOL SHIFT, M, N, B (bit 6 is low)
    {
        // Bit 0: SPACE
        if (is_key_pressed_state(' '))
        {
            fprintf(stderr, "  -> SPACE\n");
            result &= ~0x01;
        }
        // Bit 1: SYMBOL SHIFT (CTRL on modern keyboards)
        if (is_key_pressed_state(17))
        {
            fprintf(stderr, "  -> SYMBOL SHIFT\n");
            result &= ~0x02;
        }
        // Bit 2: M
        if (is_key_pressed_state('m') || is_key_pressed_state('M'))
        {
            fprintf(stderr, "  -> M\n");
            result &= ~0x04;
        }
        // Bit 3: N
        if (is_key_pressed_state('n') || is_key_pressed_state('N'))
        {
            fprintf(stderr, "  -> N\n");
            result &= ~0x08;
        }
        // Bit 4: B
        if (is_key_pressed_state('b') || is_key_pressed_state('B'))
        {
            fprintf(stderr, "  -> B\n");
            result &= ~0x10;
        }
    }
    else if (!(selector & 0x80)) // Row 7: duplicate/alternate (bit 7 is low)
    {
        // Row 7 typically mirrors row 6 in Spectrum
        // Bit 0: SPACE
        if (is_key_pressed_state(' '))
        {
            fprintf(stderr, "  -> SPACE\n");
            result &= ~0x01;
        }
        // Bit 1: SYMBOL SHIFT
        if (is_key_pressed_state(17))
        {
            fprintf(stderr, "  -> SYMBOL SHIFT\n");
            result &= ~0x02;
        }
        // Bit 2: M
        if (is_key_pressed_state('m') || is_key_pressed_state('M'))
        {
            fprintf(stderr, "  -> M\n");
            result &= ~0x04;
        }
        // Bit 3: N
        if (is_key_pressed_state('n') || is_key_pressed_state('N'))
        {
            fprintf(stderr, "  -> N\n");
            result &= ~0x08;
        }
        // Bit 4: B
        if (is_key_pressed_state('b') || is_key_pressed_state('B'))
        {
            fprintf(stderr, "  -> B\n");
            result &= ~0x10;
        }
    }

    // Per Spectrum ROM spec, bits 5-7 should be set to 1
    result |= 0xE0;
    return result;
}

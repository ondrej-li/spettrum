/**
 * Spectrum Keyboard Handler Implementation
 *
 * Defensive implementation that:
 * - Buffers keyboard input safely (non-destructive reading)
 * - Polls stdin non-blockingly
 * - Supports key injection from command line for testing
 *
 * Maps host keys to Spectrum keyboard matrix format.
 */

#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// Key buffer - store recently pressed keys
#define KEYBOARD_BUFFER_SIZE 256
static unsigned char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static int buffer_pos = 0;

// Simulated key injection (for testing)
static char simulated_key = 0;

/**
 * Set a simulated key for testing (from command line)
 */
void keyboard_set_simulated_key(char key)
{
    simulated_key = key;
    if (key != 0)
    {
        // Add to buffer so it gets picked up
        if (buffer_pos < KEYBOARD_BUFFER_SIZE - 1)
        {
            keyboard_buffer[buffer_pos++] = (unsigned char)key;
        }
    }
}

/**
 * Poll stdin and buffer any available characters
 */
static void keyboard_poll_stdin(void)
{
    unsigned char ch;
    int nread = 0;

    // Keep trying to read until stdin is empty
    while (buffer_pos < KEYBOARD_BUFFER_SIZE - 1)
    {
        nread = read(STDIN_FILENO, &ch, 1);

        if (nread == 1)
        {
            // Got a character, add to buffer
            keyboard_buffer[buffer_pos++] = ch;
        }
        else if (nread == 0 || (nread == -1 && errno == EAGAIN))
        {
            // No more input available
            break;
        }
        else
        {
            // Other error, stop
            break;
        }
    }
}

/**
 * Check if a key is in the buffer (non-destructive)
 */
static int is_key_in_buffer(unsigned char key)
{
    for (int i = 0; i < buffer_pos; i++)
    {
        if (keyboard_buffer[i] == key)
        {
            return 1;
        }
    }
    return 0;
}

/**
 * Initialize keyboard
 * Terminal setup is handled by ula.c - this module just polls stdin
 */
int keyboard_init(void)
{
    return 0;
}

/**
 * Cleanup keyboard
 * Terminal restoration is handled by ula.c
 */
void keyboard_cleanup(void)
{
    // Clear buffer
    buffer_pos = 0;
    simulated_key = 0;
}

/**
 * Check if a specific key is currently pressed
 * Returns 1 if pressed, 0 if not pressed
 *
 * Non-destructive - doesn't consume the key from buffer.
 */
static int is_key_pressed(unsigned char key)
{
    // First, poll stdin to get fresh input
    keyboard_poll_stdin();

    // Check buffer
    return is_key_in_buffer(key);
}

/**
 * Get keyboard state for port read
 *
 * The Spectrum keyboard is accessed via port 0xFE with the upper byte selecting
 * which row to read. Each row returns an 8-bit value where each bit represents
 * a key state (0 = pressed, 1 = released).
 *
 * Port address mapping (only upper byte matters for row selection):
 * 0xFEFE: Row 0 - SHIFT, Z, X, C, V
 * 0xFDFE: Row 1 - A, S, D, F, G
 * 0xFBFE: Row 2 - Q, W, E, R, T
 * 0xF7FE: Row 3 - 1, 2, 3, 4, 5
 * 0xEFFE: Row 4 - 0, 9, 8, 7, 6
 * 0xDFFE: Row 5 - ENTER, L, K, J, H
 * 0xBFFE: Row 6 - SPACE, SYMBOL SHIFT, M, N, B
 * 0x7FFE: Row 7 - (continued from row 6 pattern)
 *
 * Returns: 8-bit value where bit 0 = key 1, bit 1 = key 2, etc.
 *         (0 = pressed, 1 = released)
 */
uint8_t keyboard_read_port(uint8_t port)
{
    // Start with all keys released (0xFF = 0b11111111)
    uint8_t result = 0xFF;

    // Decode which row based on port address (only lower byte is used)
    switch (port)
    {
    // Port 0xFE (with upper byte bit 0 clear): Row 0 - SHIFT, Z, X, C, V
    case 0xFE:
        if (is_key_pressed(16)) // SHIFT key (keyCode 16)
            result &= ~0x01;    // Bit 0
        if (is_key_pressed('z') || is_key_pressed('Z'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('x') || is_key_pressed('X'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('c') || is_key_pressed('C'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('v') || is_key_pressed('V'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xFD: Row 1 - A, S, D, F, G
    case 0xFD:
        if (is_key_pressed('a') || is_key_pressed('A'))
            result &= ~0x01; // Bit 0
        if (is_key_pressed('s') || is_key_pressed('S'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('d') || is_key_pressed('D'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('f') || is_key_pressed('F'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('g') || is_key_pressed('G'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xFB: Row 2 - Q, W, E, R, T
    case 0xFB:
        if (is_key_pressed('q') || is_key_pressed('Q'))
            result &= ~0x01; // Bit 0
        if (is_key_pressed('w') || is_key_pressed('W'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('e') || is_key_pressed('E'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('r') || is_key_pressed('R'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('t') || is_key_pressed('T'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xF7: Row 3 - 1, 2, 3, 4, 5
    case 0xF7:
        if (is_key_pressed('1'))
            result &= ~0x01; // Bit 0
        if (is_key_pressed('2'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('3'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('4'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('5'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xEF: Row 4 - 0, 9, 8, 7, 6
    case 0xEF:
        if (is_key_pressed('0'))
            result &= ~0x01; // Bit 0
        if (is_key_pressed('9'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('8'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('7'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('6'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xDF: Row 5 - ENTER, L, K, J, H
    case 0xDF:
        if (is_key_pressed('\r') || is_key_pressed('\n') || is_key_pressed(13))
            result &= ~0x01; // Bit 0 - ENTER
        if (is_key_pressed('l') || is_key_pressed('L'))
            result &= ~0x02; // Bit 1
        if (is_key_pressed('k') || is_key_pressed('K'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('j') || is_key_pressed('J'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('h') || is_key_pressed('H'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0xBF: Row 6 - SPACE, SYMBOL SHIFT, M, N, B
    case 0xBF:
        if (is_key_pressed(' '))
            result &= ~0x01;    // Bit 0 - SPACE
        if (is_key_pressed(17)) // CTRL key (keyCode 17) = SYMBOL SHIFT
            result &= ~0x02;    // Bit 1
        if (is_key_pressed('m') || is_key_pressed('M'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('n') || is_key_pressed('N'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('b') || is_key_pressed('B'))
            result &= ~0x10; // Bit 4
        break;

    // Port 0x7F: Row 7 - continuation (same pattern as row 6 in some references)
    case 0x7F:
        if (is_key_pressed(' '))
            result &= ~0x01;    // Bit 0 - SPACE
        if (is_key_pressed(17)) // CTRL key = SYMBOL SHIFT
            result &= ~0x02;    // Bit 1
        if (is_key_pressed('m') || is_key_pressed('M'))
            result &= ~0x04; // Bit 2
        if (is_key_pressed('n') || is_key_pressed('N'))
            result &= ~0x08; // Bit 3
        if (is_key_pressed('b') || is_key_pressed('B'))
            result &= ~0x10; // Bit 4
        break;

    // Unknown port - return all released
    default:
        result = 0xFF;
        break;
    }

    return result;
}

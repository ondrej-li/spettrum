/**
 * Spectrum Keyboard Handler
 *
 * Stateless handler that queries the host keyboard state when called.
 * The Spectrum keyboard is organized by rows accessed via different I/O ports.
 *
 * When the Z80 executes IN (port 0xFE), the upper byte selects which row to read.
 * Active low: bits are 0 when key is pressed, 1 when released.
 *
 * Keyboard port mapping (upper byte of port address):
 * 0xFEFE: Row 0 - SHIFT, Z, X, C, V
 * 0xFDFE: Row 1 - A, S, D, F, G
 * 0xFBFE: Row 2 - Q, W, E, R, T
 * 0xF7FE: Row 3 - 1, 2, 3, 4, 5
 * 0xEFFE: Row 4 - 0, 9, 8, 7, 6
 * 0xDFFE: Row 5 - ENTER, L, K, J, H
 * 0xBFFE: Row 6 - SPACE, SYMBOL SHIFT, M, N, B (note: actually ENTER, L, K, J, H based on JS)
 * 0x7FFE: Row 7 - SPACE, SYMBOL SHIFT, M, N, B
 *
 * Return value: Each bit represents a key (0=pressed, 1=released)
 * Bit 0 = Key 1, Bit 1 = Key 2, etc.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/**
 * Initialize keyboard - sets up terminal for raw input
 * @return 0 on success, -1 on error
 */
int keyboard_init(void);

/**
 * Cleanup keyboard - restore terminal to normal mode
 */
void keyboard_cleanup(void);

/**
 * Get current keyboard state for a port read
 *
 * Reads the host keyboard and returns which keys are currently pressed
 * in the Spectrum keyboard matrix format for the specified port.
 *
 * @param port The I/O port (0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F)
 * @return 8-bit value where each bit represents a key state
 *         (0 = key pressed, 1 = key released)
 */
uint8_t keyboard_read_port(uint8_t port);

/**
 * Set a simulated key for testing (bypasses actual keyboard input)
 * Useful for command-line testing when keyboard input isn't available
 *
 * @param key The ASCII character to simulate as pressed
 */
void keyboard_set_simulated_key(char key);

#endif // KEYBOARD_H

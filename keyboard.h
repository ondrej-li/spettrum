/**
 * Spectrum Keyboard Handler
 *
 * Implements authentic Spectrum keyboard matrix scanning with active-low logic.
 *
 * How it works:
 * 1. CPU writes row selector to port 0xFE via OUT (C), B instruction
 *    - B contains a row mask with one bit low (e.g., 0xFE = row 0, 0xFD = row 1, etc.)
 *    - This selects which of 8 rows will be scanned
 *
 * 2. CPU reads port 0xFE via IN A, (C) instruction
 *    - Returns the key states for the selected row (bits 0-4)
 *    - Each bit = 0 when key pressed, 1 when released
 *    - Bits 5-7 are set to 1 (per Spectrum spec)
 *
 * Keyboard matrix (8 rows × 5 columns):
 *
 * Row 0 (0xFE - bit 0 low): SHIFT, Z, X, C, V
 * Row 1 (0xFD - bit 1 low): A, S, D, F, G
 * Row 2 (0xFB - bit 2 low): Q, W, E, R, T
 * Row 3 (0xF7 - bit 3 low): 1, 2, 3, 4, 5
 * Row 4 (0xEF - bit 4 low): 0, 9, 8, 7, 6
 * Row 5 (0xDF - bit 5 low): ENTER, L, K, J, H
 * Row 6 (0xBF - bit 6 low): SPACE, SYMBOL SHIFT, M, N, B
 * Row 7 (0x7F - bit 7 low): (alternate/duplicate)
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/**
 * Initialize keyboard - sets up internal state for keyboard scanning
 * @return 0 on success, -1 on error
 */
int keyboard_init(void);

/**
 * Cleanup keyboard - reset internal state
 */
void keyboard_cleanup(void);

/**
 * Set the current row selector (called when CPU executes OUT to port 0xFE)
 *
 * The row selector contains a row mask where one bit is low to select that row.
 * @param row_selector The 8-bit value written to port 0xFE
 */
void keyboard_set_row_selector(uint8_t row_selector);

/**
 * Get the current row selector
 * @return The current row selector byte
 */
uint8_t keyboard_get_row_selector(void);

/**
 * Read keyboard state for the currently selected row
 *
 * This is called when the CPU executes IN A, (C) to read port 0xFE.
 * Returns the key states for whichever row was most recently selected
 * via keyboard_set_row_selector().
 *
 * @param port The full 16-bit port address. For Spectrum keyboard:
 *             - Low byte: 0xFE (ULA port)
 *             - High byte: Row selector bitmask (active-low)
 *             The high byte determines which keyboard rows to scan.
 * @return 8-bit value representing key states in the selected row(s):
 *         - Bits 0-4: Key states (0=pressed, 1=released)
 *         - Bits 5-7: Always set to 1 (per Spectrum ROM spec)
 */
uint8_t keyboard_read_port(uint16_t port);

/**
 * Set a simulated key for testing (for command-line key injection)
 * Adds the key to the pressed set so it will be detected in subsequent scans
 *
 * @param key The ASCII character to simulate as pressed
 */
void keyboard_set_simulated_key(char key);

#endif // KEYBOARD_H

/**
 * TAP File Format Support for Spettrum Z80 Emulator
 *
 * The TAP format stores raw tape data blocks. Each block consists of:
 * - 2-byte length (little-endian): length of data that follows
 * - N bytes of data: the actual block content
 *
 * The data typically follows Spectrum tape encoding:
 * - Flag byte (first byte of block): determines block type
 *   - 0x00: PROGRAM/CODE header block
 *   - 0xFF: DATA block
 * - Data bytes
 * - Checksum byte (XOR of all bytes including flag)
 *
 * TAPE ENCODING (standard Spectrum ROM loading):
 * Each data byte is encoded as follows:
 * 1. PILOT TONE: 8063 pulses (header) or 3223 pulses (data) of equal length
 * 2. SYNC: 2 pulses of different lengths (667 and 735 T-states)
 * 3. DATA: Each bit encoded as two pulses:
 *    - Bit 0: pulse of 855 T-states + pulse of 855 T-states
 *    - Bit 1: pulse of 1710 T-states + pulse of 1710 T-states
 * 4. MSB first encoding per byte
 *
 * Timings are in Z80 T-states (clock cycles):
 * - Z80 runs at 3.5 MHz
 * - 1 T-state = 1/3,500,000 second
 */

#ifndef TAP_H
#define TAP_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/**
 * TAP block header (2 bytes)
 */
typedef struct
{
    uint16_t length; // Length of data following this header (little-endian)
} tap_block_header_t;

/**
 * TAP file context for iterating through blocks
 */
typedef struct
{
    FILE *file;          // File handle
    uint32_t file_size;  // Total file size
    uint32_t file_pos;   // Current position in file
    uint8_t *block_data; // Current block data buffer
    uint16_t block_len;  // Length of current block data
} tap_file_t;

/**
 * Tape player state machine - simulates cassette playback
 * Feeds pulse data to port 0xFE bit 6 (EAR input) for ROM loader
 */
typedef enum
{
    TAPE_STATE_IDLE,  // No tape loaded
    TAPE_STATE_PILOT, // Playing pilot tone
    TAPE_STATE_SYNC,  // Playing sync pulses
    TAPE_STATE_DATA,  // Playing data bits
    TAPE_STATE_END    // Tape finished
} tape_state_t;

/**
 * Tape player context
 */
typedef struct
{
    // File playback
    tap_file_t *tap_file;   // Current TAP file
    uint32_t current_block; // Current block index
    uint8_t *block_data;    // Current block being played
    uint16_t block_len;     // Length of current block
    uint32_t block_bit_pos; // Current bit position within block

    // Playback state
    tape_state_t state;       // Current playback state
    uint32_t pulse_count;     // Pulses remaining in current phase
    uint16_t pulse_length;    // Current pulse length in T-states
    uint8_t ear_level;        // Current EAR output level (0=low, 1=high)
    uint64_t cycle_count;     // Z80 cycles until next edge
    uint64_t last_edge_cycle; // Cycle count of last edge

    // Data bit encoding state (each bit = 2 pulses)
    uint8_t data_pulse_phase;  // 0 = first pulse of bit, 1 = second pulse
    uint8_t current_bit_value; // Current bit value (0 or 1) for second pulse

    // Timings (T-states)
    uint16_t pilot_length; // Pilot pulse length (2168)
    uint16_t pilot_count;  // Number of pilot pulses (8063 or 3223)
    uint16_t sync1_length; // First sync pulse (667)
    uint16_t sync2_length; // Second sync pulse (735)
    uint16_t zero_length;  // Zero bit pulse length (855)
    uint16_t one_length;   // One bit pulse length (1710)

    // Debug logging
    FILE *debug_log;     // Debug log file handle
    uint64_t read_count; // Number of times tape_player_read_ear was called
} tape_player_t;

/**
 * Open TAP file and return context
 * Returns NULL on error
 */
tap_file_t *tap_open(const char *filename);

/**
 * Close TAP file and free resources
 */
void tap_close(tap_file_t *tap);

/**
 * Read next block from TAP file
 * Returns 0 if successful, -1 if end of file or error
 * Block data pointer is valid until next call to tap_read_block()
 */
int tap_read_block(tap_file_t *tap, uint8_t **data, uint16_t *length);

/**
 * Load TAP file into memory starting at specified address
 * Loads all data blocks sequentially into memory
 * Returns 0 if successful, -1 on error
 */
int tap_load_to_memory(const char *filename, uint8_t *memory, size_t memory_size, uint16_t start_addr);

/**
 * Get TAP file information (number of blocks, total data size, etc.)
 * Returns 0 if successful, -1 on error
 */
int tap_get_info(const char *filename, uint32_t *block_count, uint32_t *total_data_size);

/**
 * AUTHENTIC TAPE LOADING - Initialize tape player with TAP file
 * Player will simulate cassette playback through port 0xFE
 * Returns NULL on error
 */
tape_player_t *tape_player_init(const char *filename);

/**
 * Close tape player and free resources
 */
void tape_player_close(tape_player_t *player);

/**
 * Advance tape player state and return current EAR bit (port 0xFE bit 6)
 * Call this whenever the ROM loader reads port 0xFE
 * Returns 0 or 1 for ear level
 */
uint8_t tape_player_read_ear(tape_player_t *player, uint64_t current_cycle);

/**
 * Check if tape playback is complete
 * Returns 1 if tape finished, 0 if still playing
 */
int tape_player_is_finished(tape_player_t *player);

#endif

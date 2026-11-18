/**
 * TAP File Loader Implementation
 *
 * Loads Spectrum tape image files (.TAP format) into emulator memory
 * Supports both quick-load (direct memory) and authentic tape loading
 * (via port 0xFE cassette EAR simulation)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tap.h"

/**
 * Open TAP file and return context
 */
tap_file_t *tap_open(const char *filename)
{
    if (!filename)
        return NULL;

    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(stderr, "Error: Cannot open TAP file '%s'\n", filename);
        return NULL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0)
    {
        fprintf(stderr, "Error: TAP file is empty or unreadable\n");
        fclose(file);
        return NULL;
    }

    tap_file_t *tap = (tap_file_t *)malloc(sizeof(tap_file_t));
    if (!tap)
    {
        fprintf(stderr, "Error: Memory allocation failed for TAP context\n");
        fclose(file);
        return NULL;
    }

    tap->file = file;
    tap->file_size = (uint32_t)file_size;
    tap->file_pos = 0;
    tap->block_data = NULL;
    tap->block_len = 0;

    return tap;
}

/**
 * Close TAP file and free resources
 */
void tap_close(tap_file_t *tap)
{
    if (!tap)
        return;

    if (tap->file)
        fclose(tap->file);

    if (tap->block_data)
        free(tap->block_data);

    free(tap);
}

/**
 * Read next block from TAP file
 * Returns 0 if successful, -1 if end of file or error
 */
int tap_read_block(tap_file_t *tap, uint8_t **data, uint16_t *length)
{
    if (!tap || !data || !length)
        return -1;

    // Check if we've reached end of file
    if (tap->file_pos >= tap->file_size)
        return -1;

    // Read block header (2 bytes, little-endian length)
    uint8_t header[2];
    if (fread(header, 1, 2, tap->file) != 2)
    {
        fprintf(stderr, "Error: Failed to read block header\n");
        return -1;
    }

    // Parse length (little-endian)
    uint16_t block_length = header[0] | (header[1] << 8);
    tap->file_pos += 2;

    // Sanity check: block cannot be larger than remaining file
    if (tap->file_pos + block_length > tap->file_size)
    {
        fprintf(stderr, "Error: Block length %d exceeds file size\n", block_length);
        return -1;
    }

    // Allocate or reallocate block buffer if needed
    if (tap->block_data == NULL || block_length > 65535)
    {
        if (tap->block_data)
            free(tap->block_data);
        tap->block_data = (uint8_t *)malloc(block_length);
        if (!tap->block_data)
        {
            fprintf(stderr, "Error: Failed to allocate buffer for TAP block (%d bytes)\n", block_length);
            return -1;
        }
    }

    // Read block data
    if (fread(tap->block_data, 1, block_length, tap->file) != (size_t)block_length)
    {
        fprintf(stderr, "Error: Failed to read block data\n");
        return -1;
    }

    tap->file_pos += block_length;
    tap->block_len = block_length;

    *data = tap->block_data;
    *length = block_length;

    return 0;
}

/**
 * Load TAP file into memory starting at specified address
 * Loads all data blocks sequentially into memory
 */
int tap_load_to_memory(const char *filename, uint8_t *memory, size_t memory_size, uint16_t start_addr)
{
    if (!filename || !memory)
        return -1;

    tap_file_t *tap = tap_open(filename);
    if (!tap)
        return -1;

    uint16_t current_addr = start_addr;
    uint32_t block_count = 0;
    uint32_t total_bytes = 0;

    // Read and load all blocks
    uint8_t *block_data;
    uint16_t block_len;

    while (tap_read_block(tap, &block_data, &block_len) == 0)
    {
        block_count++;

        // Check if data fits in memory (avoid uint16_t overflow)
        if ((size_t)current_addr + (size_t)block_len > memory_size)
        {
            fprintf(stderr, "Error: TAP data exceeds memory at address 0x%04X (block %u, %u bytes)\n",
                    current_addr, block_count, block_len);
            tap_close(tap);
            return -1;
        }

        // Copy block data to memory
        memcpy(memory + current_addr, block_data, block_len);
        current_addr += block_len;
        total_bytes += block_len;
    }

    tap_close(tap);

    if (block_count == 0)
    {
        fprintf(stderr, "Error: No valid blocks found in TAP file\n");
        return -1;
    }

    printf("Loaded TAP file: %u blocks, %u bytes (0x%04X-0x%04X)\n",
           block_count, total_bytes, start_addr, start_addr + total_bytes - 1);

    return 0;
}

/**
 * Get TAP file information
 */
int tap_get_info(const char *filename, uint32_t *block_count, uint32_t *total_data_size)
{
    if (!filename)
        return -1;

    tap_file_t *tap = tap_open(filename);
    if (!tap)
        return -1;

    uint32_t num_blocks = 0;
    uint32_t data_size = 0;

    uint8_t *block_data;
    uint16_t block_len;

    while (tap_read_block(tap, &block_data, &block_len) == 0)
    {
        num_blocks++;
        data_size += block_len;
    }

    tap_close(tap);

    if (block_count)
        *block_count = num_blocks;
    if (total_data_size)
        *total_data_size = data_size;

    return 0;
}

/**
 * AUTHENTIC TAPE LOADING
 *
 * Initialize tape player for ROM-based loading
 * The player simulates cassette playback by feeding pulses to port 0xFE
 */
tape_player_t *tape_player_init(const char *filename)
{
    if (!filename)
        return NULL;

    tape_player_t *player = (tape_player_t *)malloc(sizeof(tape_player_t));
    if (!player)
    {
        fprintf(stderr, "Error: Memory allocation failed for tape player\n");
        return NULL;
    }

    // Open TAP file
    player->tap_file = tap_open(filename);
    if (!player->tap_file)
    {
        free(player);
        return NULL;
    }

    // Initialize playback state
    player->current_block = 0;
    player->block_data = NULL;
    player->block_len = 0;
    player->block_bit_pos = 0;

    // Start in idle state - will transition to PILOT when first block is loaded
    player->state = TAPE_STATE_IDLE;
    player->pulse_count = 0;
    player->pulse_length = 0;
    player->ear_level = 0; // Start low
    player->cycle_count = 0;
    player->last_edge_cycle = 0;

    // Set standard Spectrum ROM timings (T-states at 3.5 MHz)
    player->pilot_length = 2168; // Pilot pulse length
    player->pilot_count = 8063;  // Header pilot count (will adjust for data blocks)
    player->sync1_length = 667;  // First sync pulse
    player->sync2_length = 735;  // Second sync pulse
    player->zero_length = 855;   // Zero bit pulse pair length
    player->one_length = 1710;   // One bit pulse pair length

    // Load first block
    if (tap_read_block(player->tap_file, &player->block_data, &player->block_len) == 0)
    {
        player->state = TAPE_STATE_PILOT;
        player->pulse_count = player->pilot_count;
        player->pulse_length = player->pilot_length;

        // Determine if header (flag=0x00) or data (flag=0xFF)
        // If data block (flag 0xFF), use shorter pilot
        if (player->block_data && player->block_data[0] == 0xFF)
        {
            player->pilot_count = 3223; // Data block pilot count
            player->pulse_count = 3223;
        }

        printf("Tape loaded: Starting playback from block %u (%u bytes)\n",
               player->current_block, player->block_len);
    }
    else
    {
        fprintf(stderr, "Error: Failed to load first TAP block\n");
        tap_close(player->tap_file);
        free(player);
        return NULL;
    }

    return player;
}

/**
 * Close tape player and free resources
 */
void tape_player_close(tape_player_t *player)
{
    if (!player)
        return;

    if (player->tap_file)
        tap_close(player->tap_file);

    free(player);
}

/**
 * Get next bit from current block data
 * Returns 0 or 1, advances bit position
 */
static uint8_t tape_get_next_bit(tape_player_t *player)
{
    if (player->block_bit_pos >= (uint32_t)(player->block_len * 8))
        return 0; // End of block

    uint32_t byte_pos = player->block_bit_pos / 8;
    uint8_t bit_pos = 7 - (player->block_bit_pos % 8); // MSB first
    uint8_t byte_val = player->block_data[byte_pos];
    uint8_t bit = (byte_val >> bit_pos) & 1;

    player->block_bit_pos++;
    return bit;
}

/**
 * Advance tape player state machine
 * Returns current EAR bit (port 0xFE bit 6) that ROM loader reads
 *
 * The tape encodes data as a series of pulses. Each pulse has a length in T-states.
 * When the ROM loader reads port 0xFE, it measures the pulse duration to decode bits.
 *
 * State machine flow:
 * PILOT → SYNC1 → SYNC2 → DATA_BIT[0..n] → next block or END
 */
uint8_t tape_player_read_ear(tape_player_t *player, uint64_t current_cycle)
{
    if (!player || player->state == TAPE_STATE_IDLE || player->state == TAPE_STATE_END)
        return 0; // Tape not loaded or finished

    // Check if time for next pulse edge
    if (current_cycle >= player->last_edge_cycle + player->cycle_count)
    {
        // Toggle ear level and advance to next pulse
        player->ear_level = !player->ear_level;
        player->last_edge_cycle = current_cycle;

        // Determine next pulse timing based on state
        switch (player->state)
        {
        case TAPE_STATE_PILOT:
            player->cycle_count = player->pilot_length;
            player->pulse_count--;

            if (player->pulse_count == 0)
            {
                // Pilot phase done, move to sync
                player->state = TAPE_STATE_SYNC;
                player->pulse_count = 2; // Two sync pulses
                player->ear_level = 0;   // Ensure we start low for sync
            }
            break;

        case TAPE_STATE_SYNC:
            if (player->pulse_count == 2)
            {
                player->cycle_count = player->sync1_length;
                player->pulse_count--;
            }
            else if (player->pulse_count == 1)
            {
                player->cycle_count = player->sync2_length;
                player->pulse_count--;
            }
            else
            {
                // Sync done, move to data
                player->state = TAPE_STATE_DATA;
                player->block_bit_pos = 0;
                player->ear_level = 0; // Start low
            }
            break;

        case TAPE_STATE_DATA:
        {
            // Each data bit is encoded as two pulses of same length
            // Bit 0: two pulses of 855 T-states each
            // Bit 1: two pulses of 1710 T-states each

            uint8_t data_bit = tape_get_next_bit(player);
            uint16_t bit_length = (data_bit == 0) ? player->zero_length : player->one_length;
            player->cycle_count = bit_length;

            if (player->block_bit_pos > player->block_len * 8)
            {
                // Block complete, try to load next block
                if (tap_read_block(player->tap_file, &player->block_data, &player->block_len) == 0)
                {
                    player->current_block++;
                    player->state = TAPE_STATE_PILOT;
                    player->pulse_count = player->pilot_count;
                    player->pulse_length = player->pilot_length;
                    player->block_bit_pos = 0;

                    // Check if data block (shorter pilot)
                    if (player->block_data && player->block_data[0] == 0xFF)
                        player->pulse_count = 3223;

                    fprintf(stderr, "Loading block %u...\n", player->current_block);
                }
                else
                {
                    // No more blocks
                    player->state = TAPE_STATE_END;
                    fprintf(stderr, "Tape playback complete\n");
                }
            }
            break;
        }

        case TAPE_STATE_IDLE:
        case TAPE_STATE_END:
            // Do nothing
            break;
        }
    }

    return player->ear_level;
}

/**
 * Check if tape playback is complete
 */
int tape_player_is_finished(tape_player_t *player)
{
    if (!player)
        return 1;

    return (player->state == TAPE_STATE_END) ? 1 : 0;
}

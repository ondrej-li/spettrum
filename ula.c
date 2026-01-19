#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <termios.h>
#include "ula.h"

// ZX Spectrum video RAM dimensions
#define SPECTRUM_WIDTH 256
#define SPECTRUM_HEIGHT 192
#define SPECTRUM_WIDTH_BYTES (SPECTRUM_WIDTH / 8)
#define SPECTRUM_VRAM_SIZE (SPECTRUM_WIDTH_BYTES * SPECTRUM_HEIGHT)
#define SPECTRUM_ATTR_SIZE (32 * 24)
#define SPECTRUM_RAM_SIZE (SPECTRUM_VRAM_SIZE + SPECTRUM_ATTR_SIZE)

// Output matrix dimensions (2x2 pixels -> 1 character)
#define OUTPUT_WIDTH (SPECTRUM_WIDTH / 2)
#define OUTPUT_HEIGHT (SPECTRUM_HEIGHT / 2)

// Block drawing characters indexed by pattern (TL, TR, BL, BR)
static const char *block_chars[] = {
    " ", // 0000 -> ' ' (empty)
    "▗", // 0001 -> ▗
    "▖", // 0010 -> ▖
    "▄", // 0011 -> ▄ (bottom row)
    "▝", // 0100 -> ▝
    "▐", // 0101 -> ▐ (right column)
    "▞", // 0110 -> ▞ (diagonal \)
    "▟", // 0111 -> ▟
    "▘", // 1000 -> ▘
    "▚", // 1001 -> ▚ (diagonal /)
    "▌", // 1010 -> ▌ (left column)
    "▙", // 1011 -> ▙
    "▀", // 1100 -> ▀ (top row)
    "▜", // 1101 -> ▜
    "▛", // 1110 -> ▛
    "█"  // 1111 -> █ (full)
};

// Thread-safe output matrix
typedef struct
{
    const char *matrix[OUTPUT_HEIGHT][OUTPUT_WIDTH];
    pthread_mutex_t lock;
} ula_matrix_t;

static ula_matrix_t ula_matrix = {
    .lock = PTHREAD_MUTEX_INITIALIZER};

/**
 * Get pixel value from video RAM at (x, y)
 * ZX Spectrum memory layout is NOT continuous - it's split into thirds
 * Formula: address = ((y / 8) * 2048) + ((y % 8) * 256) + (x / 8)
 * Each byte stores 8 horizontal pixels: MSB is leftmost pixel
 */
static int get_pixel(const uint8_t *vram, int x, int y)
{
    // Spectrum video RAM layout:
    // The 192 lines are divided into 3 sections of 64 lines each
    // Within each section, lines are stored in a special pattern
    int section = y / 64;                // Which third (0, 1, or 2)
    int line_in_section = y % 64;        // Line within that third
    int char_row = line_in_section / 8;  // Character row (0-7)
    int pixel_row = line_in_section % 8; // Pixel within character (0-7)
    int char_col = x / 8;                // Character column (0-31)

    // Calculate address
    int address = section * 2048 + char_row * 256 + pixel_row * 32 + char_col;

    // Ensure we don't read beyond VRAM
    if (address >= SPECTRUM_VRAM_SIZE)
        return 0;

    uint8_t byte = vram[address];
    int bit_index = 7 - (x % 8); // MSB is leftmost pixel
    return (byte >> bit_index) & 1;
}

/**
 * Convert 2x2 pixel block to block character
 * Pattern: TL TR BL BR (top-left, top-right, bottom-left, bottom-right)
 */
static const char *get_block_char(const uint8_t *vram, int x, int y)
{
    int pixel_x = x * 2;
    int pixel_y = y * 2;

    // Get the 4 pixels in 2x2 block
    int tl = get_pixel(vram, pixel_x, pixel_y);
    int tr = get_pixel(vram, pixel_x + 1, pixel_y);
    int bl = get_pixel(vram, pixel_x, pixel_y + 1);
    int br = get_pixel(vram, pixel_x + 1, pixel_y + 1);

    // Combine into pattern index (0-15)
    int pattern = (tl << 3) | (tr << 2) | (bl << 1) | br;

    return block_chars[pattern];
}

/**
 * Convert entire video RAM to matrix
 * This is the core ULA conversion function
 */
void convert_vram_to_matrix(const uint8_t *vram)
{
    for (int y = 0; y < OUTPUT_HEIGHT; y++)
    {
        for (int x = 0; x < OUTPUT_WIDTH; x++)
        {
            ula_matrix.matrix[y][x] = get_block_char(vram, x, y);
        }
    }
}

/**
 * Render the matrix to terminal with minimal flickering
 * Uses pre-rendered buffer and atomic screen updates for 50Hz
 */
void ula_render_to_terminal(void)
{
    // 50Hz = 20ms per frame
    const long FRAME_TIME_NS = 20000000; // 20ms in nanoseconds
    static char render_buffer[40960];    // Pre-allocated render buffer (40KB for UTF-8)
    static int first_frame = 1;

    struct timespec frame_start, frame_end;
    long elapsed_ns;

    // Start frame timer BEFORE any I/O
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    // Lock matrix for reading
    pthread_mutex_lock(&ula_matrix.lock);

    // Build entire frame in buffer (avoids multiple printf calls)
    int buffer_pos = 0;

    // Move cursor to home only on first frame, then use relative positioning
    if (first_frame)
    {
        // Use alternate screen buffer and clear it
        buffer_pos += sprintf(render_buffer + buffer_pos, "\033[?1049h\033[2J\033[H\033[?25l");
        first_frame = 0;
    }
    else
    {
        // Move cursor back to top-left for subsequent frames
        buffer_pos += sprintf(render_buffer + buffer_pos, "\033[H");
    }

    // Render each line directly into buffer
    for (int y = 0; y < OUTPUT_HEIGHT; y++)
    {
        for (int x = 0; x < OUTPUT_WIDTH; x++)
        {
            const char *ch = ula_matrix.matrix[y][x];
            // Handle UTF-8 multi-byte characters
            while (*ch && buffer_pos < 40950)
            {
                render_buffer[buffer_pos++] = *ch++;
            }
        }
        if (buffer_pos < 40959)
            render_buffer[buffer_pos++] = '\n';
    }

    pthread_mutex_unlock(&ula_matrix.lock);

    // Write entire buffer at once (atomic write)
    fwrite(render_buffer, 1, buffer_pos, stdout);
    fflush(stdout);

    // End frame timer AFTER I/O completes
    clock_gettime(CLOCK_MONOTONIC, &frame_end);

    // Calculate elapsed time
    elapsed_ns = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L +
                 (frame_end.tv_nsec - frame_start.tv_nsec);

    // Sleep for remaining time in frame
    if (elapsed_ns < FRAME_TIME_NS)
    {
        long sleep_ns = FRAME_TIME_NS - elapsed_ns;
        usleep(sleep_ns / 1000); // Convert nanoseconds to microseconds
    }
}

/**
 * Initialize terminal for rendering
 * Sets up alternate screen buffer and hides cursor
 */
void ula_term_init(void)
{
    // Enter alternate screen buffer, clear screen, home cursor, hide cursor
    fprintf(stdout, "\033[?1049h\033[2J\033[H\033[?25l");
    fflush(stdout);
}

/**
 * Cleanup terminal after rendering
 * Exits alternate screen buffer and shows cursor
 */
void ula_term_cleanup(void)
{
    // Exit alternate screen buffer and show cursor
    fprintf(stdout, "\033[?1049l\033[?25h");
    fflush(stdout);
}

/**
 * Initialize ULA display
 */
ula_t *ula_init(int width, int height)
{
    ula_t *ula = malloc(sizeof(ula_t));
    if (!ula)
        return NULL;

    ula->width = width;
    ula->height = height;
    ula->border_color = 0;
    memset(ula->vram, 0, SPECTRUM_RAM_SIZE);
    pthread_mutex_init(&ula->lock, NULL);

    return ula;
}

/**
 * Cleanup ULA display
 */
void ula_cleanup(ula_t *ula)
{
    if (!ula)
        return;

    pthread_mutex_destroy(&ula->lock);
    free(ula);
}

/**
 * Set border color
 */
void ula_set_border_color(ula_t *ula, uint8_t color)
{
    if (!ula)
        return;

    pthread_mutex_lock(&ula->lock);
    ula->border_color = color & 0x07;
    pthread_mutex_unlock(&ula->lock);
}

/**
 * Get border color
 */
uint8_t ula_get_border_color(const ula_t *ula)
{
    if (!ula)
        return 0;
    return ula->border_color;
}

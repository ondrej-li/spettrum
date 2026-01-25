#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "ula.h"

// ZX Spectrum video RAM dimensions
#define SPECTRUM_WIDTH 256
#define SPECTRUM_HEIGHT 192
#define SPECTRUM_WIDTH_BYTES (SPECTRUM_WIDTH / 8)
#define SPECTRUM_VRAM_SIZE (SPECTRUM_WIDTH_BYTES * SPECTRUM_HEIGHT)
#define SPECTRUM_ATTR_SIZE (32 * 24)
#define SPECTRUM_RAM_SIZE (SPECTRUM_VRAM_SIZE + SPECTRUM_ATTR_SIZE)

// Terminal state for raw mode
static struct termios orig_termios;
static int termios_saved = 0;

// Spectrum to ANSI color mapping
// Spectrum: 0=Black, 1=Blue, 2=Red, 3=Magenta, 4=Green, 5=Cyan, 6=Yellow, 7=White
// ANSI:     0=Black, 1=Red, 2=Green, 3=Yellow, 4=Blue, 5=Magenta, 6=Cyan, 7=White
static const int spectrum_to_ansi[8] = {0, 4, 1, 5, 2, 6, 3, 7};

// Attribute memory layout
#define SPECTRUM_ATTR_COLS 32
#define SPECTRUM_ATTR_ROWS 24

// Attribute byte bits
#define ATTR_INK_MASK 0x07    // Bits 0-2: foreground color
#define ATTR_PAPER_MASK 0x38  // Bits 3-5: background color
#define ATTR_BRIGHT_MASK 0x40 // Bit 6: brightness
#define ATTR_BLINK_MASK 0x80  // Bit 7: blink

// Color information for character output
typedef struct
{
    uint8_t ink;    // Foreground color (0-7)
    uint8_t paper;  // Background color (0-7)
    uint8_t bright; // Brightness (0 or 1)
} color_attr_t;

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
    color_attr_t matrix_colors[OUTPUT_HEIGHT][OUTPUT_WIDTH];
    char braille_matrix[BRAILLE_OUTPUT_HEIGHT][BRAILLE_OUTPUT_WIDTH * 4]; // UTF-8 braille takes 3 bytes + null
    color_attr_t braille_colors[BRAILLE_OUTPUT_HEIGHT][BRAILLE_OUTPUT_WIDTH];
    ula_render_mode_t render_mode;
    pthread_mutex_t lock;
} ula_matrix_t;

static ula_matrix_t ula_matrix = {
    .render_mode = ULA_RENDER_BRAILLE2X4,
    .lock = PTHREAD_MUTEX_INITIALIZER};

/**
 * Get attribute byte from video RAM
 * Attributes are stored linearly in the second half of VRAM
 * Address = SPECTRUM_VRAM_SIZE + (row * 32) + col
 * where row = y / 8, col = x / 8 (character grid: 32x24)
 */
static color_attr_t get_attribute(const uint8_t *vram, int x, int y)
{
    color_attr_t attr = {0, 7, 0}; // Default: black on white, not bright

    // Character position in 32x24 grid
    int char_col = (x / 8) % SPECTRUM_ATTR_COLS;
    int char_row = (y / 8) % SPECTRUM_ATTR_ROWS;

    // Calculate attribute address
    int attr_address = SPECTRUM_VRAM_SIZE + (char_row * SPECTRUM_ATTR_COLS) + char_col;

    // Ensure we don't read beyond VRAM
    if (attr_address >= (SPECTRUM_VRAM_SIZE + SPECTRUM_ATTR_SIZE))
        return attr;

    uint8_t attr_byte = vram[attr_address];
    attr.ink = attr_byte & ATTR_INK_MASK;
    attr.paper = (attr_byte & ATTR_PAPER_MASK) >> 3;
    attr.bright = (attr_byte & ATTR_BRIGHT_MASK) >> 6;

    return attr;
}

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

    // Calculate address - ZX Spectrum's interleaved scanline layout
    // Address = (section * 2048) + (pixel_row * 256) + (char_row * 32) + char_col
    int address = (section * 2048) + (pixel_row * 256) + (char_row * 32) + char_col;

    // Ensure we don't read beyond VRAM
    if (address >= SPECTRUM_VRAM_SIZE)
        return 0;

    uint8_t byte = vram[address];
    int bit_index = 7 - (x % 8); // MSB is leftmost pixel
    return (byte >> bit_index) & 1;
}

/**
 * Convert 2x2 pixel block to block character with color information
 * Pattern: TL TR BL BR (top-left, top-right, bottom-left, bottom-right)
 */
static const char *get_block_char(const uint8_t *vram, int x, int y, color_attr_t *out_color)
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

    // Get attribute for this block (based on top-left pixel position)
    *out_color = get_attribute(vram, pixel_x, pixel_y);

    return block_chars[pattern];
}

/**
 * Get braille character for 2x4 pixel block with color information
 * Braille Unicode: U+2800 + pattern
 *
 * Pixel layout (2 cols x 4 rows):    Braille dot positions:
 *   Col0 Col1                          0 3
 *   row0 row0                          1 4
 *   row1 row1                          2 5
 *   row2 row2                          6 7
 *   row3 row3
 *
 * Examples:
 *   11 11 11 11 (all filled)    -> pattern 0xFF -> U+28FF -> ⣿
 *   01 01 01 01 (right column)  -> pattern 0xB8 -> U+28B8 -> ⢸
 *   10 10 10 10 (left column)   -> pattern 0x47 -> U+2847 -> ⡇
 *   00 00 00 00 (empty)         -> pattern 0x00 -> U+2800 -> ⠀
 *
 * LIMITATION: Braille characters have visual gaps between rows. A vertical line
 * spanning 8 pixels renders as two braille characters with a visible gap:
 *   ⢸⡇  <- top 4 pixels (rows 0-3)
 *   ⠸⠇  <- bottom 4 pixels (rows 4-7)
 * This is inherent to braille character design (meant for tactile reading, not graphics).
 */
static void get_braille_char(const uint8_t *vram, int x, int y, char *out, color_attr_t *out_color)
{
    int pixel_x = x * 2;
    int pixel_y = y * 4;

    // Read 2x4 pixel block
    // Left column (col 0): rows 0,1,2,3
    int left_col_row0 = get_pixel(vram, pixel_x, pixel_y);
    int left_col_row1 = get_pixel(vram, pixel_x, pixel_y + 1);
    int left_col_row2 = get_pixel(vram, pixel_x, pixel_y + 2);
    int left_col_row3 = get_pixel(vram, pixel_x, pixel_y + 3);

    // Right column (col 1): rows 0,1,2,3
    int right_col_row0 = get_pixel(vram, pixel_x + 1, pixel_y);
    int right_col_row1 = get_pixel(vram, pixel_x + 1, pixel_y + 1);
    int right_col_row2 = get_pixel(vram, pixel_x + 1, pixel_y + 2);
    int right_col_row3 = get_pixel(vram, pixel_x + 1, pixel_y + 3);

    // Build braille pattern (each pixel maps to one bit)
    // Braille dots: 0,1,2,6 (left column) and 3,4,5,7 (right column)
    int pattern = 0;
    if (left_col_row0)
        pattern |= 0x01; // dot 0 (bit 0)
    if (left_col_row1)
        pattern |= 0x02; // dot 1 (bit 1)
    if (left_col_row2)
        pattern |= 0x04; // dot 2 (bit 2)
    if (right_col_row0)
        pattern |= 0x08; // dot 3 (bit 3)
    if (right_col_row1)
        pattern |= 0x10; // dot 4 (bit 4)
    if (right_col_row2)
        pattern |= 0x20; // dot 5 (bit 5)
    if (left_col_row3)
        pattern |= 0x40; // dot 6 (bit 6)
    if (right_col_row3)
        pattern |= 0x80; // dot 7 (bit 7)

    // Encode braille codepoint (U+2800 + pattern) as UTF-8 (3 bytes)
    // U+2800 = 0xE2 0xA0 0x80
    // U+28FF = 0xE2 0xA3 0xBF
    // The pattern bits are distributed across bytes 1 and 2:
    //   Byte 1: 0xA0 + (bits 7-6 of pattern)
    //   Byte 2: 0x80 + (bits 5-0 of pattern)
    out[0] = 0xE2;
    out[1] = 0xA0 + (pattern >> 6);
    out[2] = 0x80 + (pattern & 0x3F);
    out[3] = '\0';

    // Get attribute for this block (based on top-left pixel position)
    *out_color = get_attribute(vram, pixel_x, pixel_y);
}

/**
 * Convert entire video RAM to matrix with colors
 * This is the core ULA conversion function
 */
void convert_vram_to_matrix(const uint8_t *vram, ula_render_mode_t render_mode)
{
    ula_matrix.render_mode = render_mode;

    if (render_mode == ULA_RENDER_BRAILLE2X4)
    {
        // Braille mode: 2x4 pixels per character
        for (int y = 0; y < BRAILLE_OUTPUT_HEIGHT; y++)
        {
            for (int x = 0; x < BRAILLE_OUTPUT_WIDTH; x++)
            {
                get_braille_char(vram, x, y, &ula_matrix.braille_matrix[y][x * 4], &ula_matrix.braille_colors[y][x]);
            }
        }
    }
    else
    {
        // Block mode: 2x2 pixels per character
        for (int y = 0; y < OUTPUT_HEIGHT; y++)
        {
            for (int x = 0; x < OUTPUT_WIDTH; x++)
            {
                ula_matrix.matrix[y][x] = get_block_char(vram, x, y, &ula_matrix.matrix_colors[y][x]);
            }
        }
    }
}

/**
 * Render the matrix to terminal with minimal flickering
 * Uses pre-rendered buffer and atomic screen updates for 50Hz
 */
void ula_render_to_terminal(void)
{
#ifndef DISABLE_RENDERING
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

    // Render based on mode
    if (ula_matrix.render_mode == ULA_RENDER_BRAILLE2X4)
    {
        // Braille mode rendering with colors
        color_attr_t current_attr = {0, 0, 0}; // Track last color to minimize escape codes
        for (int y = 0; y < BRAILLE_OUTPUT_HEIGHT; y++)
        {
            current_attr.ink = 0xFF; // Force initial color code
            for (int x = 0; x < BRAILLE_OUTPUT_WIDTH; x++)
            {
                color_attr_t attr = ula_matrix.braille_colors[y][x];

                // Emit color code only if color changed
                if (attr.ink != current_attr.ink || attr.paper != current_attr.paper || attr.bright != current_attr.bright)
                {
                    current_attr = attr;
                    // Convert Spectrum colors to ANSI colors
                    int ansi_ink = spectrum_to_ansi[attr.ink & 7];
                    int ansi_paper = spectrum_to_ansi[attr.paper & 7];

                    // Apply foreground color: 30-37 for normal, 90-97 for bright
                    int fg_code = 30 + ansi_ink;
                    if (attr.bright)
                        fg_code += 60; // Convert to bright colors (90-97)

                    // Apply background color: 40-47
                    int bg_code = 40 + ansi_paper;

                    buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%d;%dm", fg_code, bg_code);
                }

                const char *ch = &ula_matrix.braille_matrix[y][x * 4];
                // Copy UTF-8 character (3 bytes)
                for (int i = 0; i < 3 && ch[i] && buffer_pos < 40950; i++)
                {
                    render_buffer[buffer_pos++] = ch[i];
                }
            }
            // Reset colors before newline
            if (buffer_pos < 40955)
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m");
            if (buffer_pos < 40959)
                render_buffer[buffer_pos++] = '\n';
        }
    }
    else
    {
        // Block mode rendering with colors
        color_attr_t current_attr = {0, 0, 0}; // Track last color to minimize escape codes
        for (int y = 0; y < OUTPUT_HEIGHT; y++)
        {
            current_attr.ink = 0xFF; // Force initial color code
            for (int x = 0; x < OUTPUT_WIDTH; x++)
            {
                color_attr_t attr = ula_matrix.matrix_colors[y][x];

                // Emit color code only if color changed
                if (attr.ink != current_attr.ink || attr.paper != current_attr.paper || attr.bright != current_attr.bright)
                {
                    current_attr = attr;
                    // Convert Spectrum colors to ANSI colors
                    int ansi_ink = spectrum_to_ansi[attr.ink & 7];
                    int ansi_paper = spectrum_to_ansi[attr.paper & 7];

                    // Apply foreground color: 30-37 for normal, 90-97 for bright
                    int fg_code = 30 + ansi_ink;
                    if (attr.bright)
                        fg_code += 60; // Convert to bright colors (90-97)

                    // Apply background color: 40-47
                    int bg_code = 40 + ansi_paper;

                    buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%d;%dm", fg_code, bg_code);
                }

                const char *ch = ula_matrix.matrix[y][x];
                // Handle UTF-8 multi-byte characters
                while (*ch && buffer_pos < 40950)
                {
                    render_buffer[buffer_pos++] = *ch++;
                }
            }
            // Reset colors before newline
            if (buffer_pos < 40955)
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m");
            if (buffer_pos < 40959)
                render_buffer[buffer_pos++] = '\n';
        }
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
#else
    // DISABLE_RENDERING: Skip terminal output, but maintain 50Hz frame timing
    const long FRAME_TIME_NS = 20000000; // 20ms in nanoseconds
    struct timespec frame_start, frame_end;
    long elapsed_ns;

    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    // Just sleep for frame time without rendering
    clock_gettime(CLOCK_MONOTONIC, &frame_end);
    elapsed_ns = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L +
                 (frame_end.tv_nsec - frame_start.tv_nsec);

    if (elapsed_ns < FRAME_TIME_NS)
    {
        long sleep_ns = FRAME_TIME_NS - elapsed_ns;
        usleep(sleep_ns / 1000);
    }
#endif
}

/**
 * Initialize terminal for rendering
 * Sets up alternate screen buffer and hides cursor
 */
void ula_term_init(void)
{
    // Save original terminal settings and set to raw mode
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0)
    {
        termios_saved = 1;
        struct termios raw = orig_termios;
        // Disable canonical mode and echo
        raw.c_lflag &= ~(ICANON | ECHO);
        // Set minimum characters to 0 (non-blocking reads)
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    // Check terminal width
    struct winsize w;
    if (ioctl(0, TIOCGWINSZ, &w) == 0)
    {
        if (w.ws_col < BRAILLE_OUTPUT_WIDTH)
        {
            fprintf(stderr, "\n⚠️  WARNING: Terminal width is %d columns, but needs %d for proper display!\n",
                    w.ws_col, BRAILLE_OUTPUT_WIDTH);
            fprintf(stderr, "Please resize your terminal to at least %d columns wide.\n\n",
                    BRAILLE_OUTPUT_WIDTH);
            sleep(2);
        }
    }

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

    // Restore original terminal settings
    if (termios_saved)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        termios_saved = 0;
    }
}

/**
 * Initialize ULA display
 */
ula_t *ula_init(int width, int height, uint8_t *vram, ula_render_mode_t render_mode)
{
    ula_t *ula = malloc(sizeof(ula_t));
    if (!ula)
        return NULL;

    ula->width = width;
    ula->height = height;
    ula->vram = vram;
    ula->border_color = 0;
    ula->render_mode = render_mode;
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

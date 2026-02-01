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

/**
 * Ensure terminal is restored even on abnormal exit
 */
static void ula_emergency_cleanup(void)
{
    // Force-restore terminal settings regardless of termios_saved flag
    // This handles cases where the program crashed before normal cleanup
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);

    // Try to exit alternate screen and show cursor
    fprintf(stdout, "\033[?1049l\033[?25h");
    fflush(stdout);
}

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

// Sinclair ZX Spectrum ROM character patterns (8x8 bitmap for each character)
// Characters 0-31 are control characters (mostly empty), 32-127 are printable ASCII
static const uint8_t sinclair_font[96][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  /* ASCII  32 (SPACE) */
    {0x00, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x00},  /* ASCII  33 ('!') */
    {0x00, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00},  /* ASCII  34 ('"') */
    {0x00, 0x24, 0x7E, 0x24, 0x24, 0x7E, 0x24, 0x00},  /* ASCII  35 ('#') */
    {0x00, 0x08, 0x3E, 0x28, 0x3E, 0x0A, 0x3E, 0x08},  /* ASCII  36 ('$') */
    {0x00, 0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00},  /* ASCII  37 ('%') */
    {0x00, 0x10, 0x28, 0x10, 0x2A, 0x44, 0x3A, 0x00},  /* ASCII  38 ('&') */
    {0x00, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00},  /* ASCII  39 (''') */
    {0x00, 0x04, 0x08, 0x08, 0x08, 0x08, 0x04, 0x00},  /* ASCII  40 ('(') */
    {0x00, 0x20, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00},  /* ASCII  41 (')') */
    {0x00, 0x00, 0x14, 0x08, 0x3E, 0x08, 0x14, 0x00},  /* ASCII  42 ('*') */
    {0x00, 0x00, 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00},  /* ASCII  43 ('+') */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x10},  /* ASCII  44 (',') */
    {0x00, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00},  /* ASCII  45 ('-') */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},  /* ASCII  46 ('.') */
    {0x00, 0x00, 0x02, 0x04, 0x08, 0x10, 0x20, 0x00},  /* ASCII  47 ('/') */
    {0x00, 0x3C, 0x46, 0x4A, 0x52, 0x62, 0x3C, 0x00},  /* ASCII  48 ('0') */
    {0x00, 0x18, 0x28, 0x08, 0x08, 0x08, 0x3E, 0x00},  /* ASCII  49 ('1') */
    {0x00, 0x3C, 0x42, 0x02, 0x3C, 0x40, 0x7E, 0x00},  /* ASCII  50 ('2') */
    {0x00, 0x3C, 0x42, 0x0C, 0x02, 0x42, 0x3C, 0x00},  /* ASCII  51 ('3') */
    {0x00, 0x08, 0x18, 0x28, 0x48, 0x7E, 0x08, 0x00},  /* ASCII  52 ('4') */
    {0x00, 0x7E, 0x40, 0x7C, 0x02, 0x42, 0x3C, 0x00},  /* ASCII  53 ('5') */
    {0x00, 0x3C, 0x40, 0x7C, 0x42, 0x42, 0x3C, 0x00},  /* ASCII  54 ('6') */
    {0x00, 0x7E, 0x02, 0x04, 0x08, 0x10, 0x10, 0x00},  /* ASCII  55 ('7') */
    {0x00, 0x3C, 0x42, 0x3C, 0x42, 0x42, 0x3C, 0x00},  /* ASCII  56 ('8') */
    {0x00, 0x3C, 0x42, 0x42, 0x3E, 0x02, 0x3C, 0x00},  /* ASCII  57 ('9') */
    {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00},  /* ASCII  58 (':') */
    {0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x10, 0x20},  /* ASCII  59 (';') */
    {0x00, 0x00, 0x04, 0x08, 0x10, 0x08, 0x04, 0x00},  /* ASCII  60 ('<') */
    {0x00, 0x00, 0x00, 0x3E, 0x00, 0x3E, 0x00, 0x00},  /* ASCII  61 ('=') */
    {0x00, 0x00, 0x10, 0x08, 0x04, 0x08, 0x10, 0x00},  /* ASCII  62 ('>') */
    {0x00, 0x3C, 0x42, 0x04, 0x08, 0x00, 0x08, 0x00},  /* ASCII  63 ('?') */
    {0x00, 0x3C, 0x4A, 0x56, 0x5E, 0x40, 0x3C, 0x00},  /* ASCII  64 ('@') */
    {0x00, 0x3C, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x00},  /* ASCII  65 ('A') */
    {0x00, 0x7C, 0x42, 0x7C, 0x42, 0x42, 0x7C, 0x00},  /* ASCII  66 ('B') */
    {0x00, 0x3C, 0x42, 0x40, 0x40, 0x42, 0x3C, 0x00},  /* ASCII  67 ('C') */
    {0x00, 0x78, 0x44, 0x42, 0x42, 0x44, 0x78, 0x00},  /* ASCII  68 ('D') */
    {0x00, 0x7E, 0x40, 0x7C, 0x40, 0x40, 0x7E, 0x00},  /* ASCII  69 ('E') */
    {0x00, 0x7E, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x00},  /* ASCII  70 ('F') */
    {0x00, 0x3C, 0x42, 0x40, 0x4E, 0x42, 0x3C, 0x00},  /* ASCII  71 ('G') */
    {0x00, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00},  /* ASCII  72 ('H') */
    {0x00, 0x3E, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00},  /* ASCII  73 ('I') */
    {0x00, 0x02, 0x02, 0x02, 0x42, 0x42, 0x3C, 0x00},  /* ASCII  74 ('J') */
    {0x00, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00},  /* ASCII  75 ('K') */
    {0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E, 0x00},  /* ASCII  76 ('L') */
    {0x00, 0x42, 0x66, 0x5A, 0x42, 0x42, 0x42, 0x00},  /* ASCII  77 ('M') */
    {0x00, 0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x00},  /* ASCII  78 ('N') */
    {0x00, 0x3C, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00},  /* ASCII  79 ('O') */
    {0x00, 0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x00},  /* ASCII  80 ('P') */
    {0x00, 0x3C, 0x42, 0x42, 0x52, 0x4A, 0x3C, 0x00},  /* ASCII  81 ('Q') */
    {0x00, 0x7C, 0x42, 0x42, 0x7C, 0x44, 0x42, 0x00},  /* ASCII  82 ('R') */
    {0x00, 0x3C, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00},  /* ASCII  83 ('S') */
    {0x00, 0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00},  /* ASCII  84 ('T') */
    {0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00},  /* ASCII  85 ('U') */
    {0x00, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00},  /* ASCII  86 ('V') */
    {0x00, 0x42, 0x42, 0x42, 0x42, 0x5A, 0x24, 0x00},  /* ASCII  87 ('W') */
    {0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00},  /* ASCII  88 ('X') */
    {0x00, 0x82, 0x44, 0x28, 0x10, 0x10, 0x10, 0x00},  /* ASCII  89 ('Y') */
    {0x00, 0x7E, 0x04, 0x08, 0x10, 0x20, 0x7E, 0x00},  /* ASCII  90 ('Z') */
    {0x00, 0x0E, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00},  /* ASCII  91 ('[') */
    {0x00, 0x00, 0x40, 0x20, 0x10, 0x08, 0x04, 0x00},  /* ASCII  92 ('\') */
    {0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00},  /* ASCII  93 (']') */
    {0x00, 0x10, 0x38, 0x54, 0x10, 0x10, 0x10, 0x00},  /* ASCII  94 ('^') */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  /* ASCII  95 ('_') */
    {0x00, 0x1C, 0x22, 0x78, 0x20, 0x20, 0x7E, 0x00},  /* ASCII  96 ('`') */
    {0x00, 0x00, 0x38, 0x04, 0x3C, 0x44, 0x3C, 0x00},  /* ASCII  97 ('a') */
    {0x00, 0x20, 0x20, 0x3C, 0x22, 0x22, 0x3C, 0x00},  /* ASCII  98 ('b') */
    {0x00, 0x00, 0x1C, 0x20, 0x20, 0x20, 0x1C, 0x00},  /* ASCII  99 ('c') */
    {0x00, 0x04, 0x04, 0x3C, 0x44, 0x44, 0x3C, 0x00},  /* ASCII 100 ('d') */
    {0x00, 0x00, 0x38, 0x44, 0x78, 0x40, 0x3C, 0x00},  /* ASCII 101 ('e') */
    {0x00, 0x0C, 0x10, 0x18, 0x10, 0x10, 0x10, 0x00},  /* ASCII 102 ('f') */
    {0x00, 0x00, 0x3C, 0x44, 0x44, 0x3C, 0x04, 0x38},  /* ASCII 103 ('g') */
    {0x00, 0x40, 0x40, 0x78, 0x44, 0x44, 0x44, 0x00},  /* ASCII 104 ('h') */
    {0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x38, 0x00},  /* ASCII 105 ('i') */
    {0x00, 0x04, 0x00, 0x04, 0x04, 0x04, 0x24, 0x18},  /* ASCII 106 ('j') */
    {0x00, 0x20, 0x28, 0x30, 0x30, 0x28, 0x24, 0x00},  /* ASCII 107 ('k') */
    {0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0C, 0x00},  /* ASCII 108 ('l') */
    {0x00, 0x00, 0x68, 0x54, 0x54, 0x54, 0x54, 0x00},  /* ASCII 109 ('m') */
    {0x00, 0x00, 0x78, 0x44, 0x44, 0x44, 0x44, 0x00},  /* ASCII 110 ('n') */
    {0x00, 0x00, 0x38, 0x44, 0x44, 0x44, 0x38, 0x00},  /* ASCII 111 ('o') */
    {0x00, 0x00, 0x78, 0x44, 0x44, 0x78, 0x40, 0x40},  /* ASCII 112 ('p') */
    {0x00, 0x00, 0x3C, 0x44, 0x44, 0x3C, 0x04, 0x06},  /* ASCII 113 ('q') */
    {0x00, 0x00, 0x1C, 0x20, 0x20, 0x20, 0x20, 0x00},  /* ASCII 114 ('r') */
    {0x00, 0x00, 0x38, 0x40, 0x38, 0x04, 0x78, 0x00},  /* ASCII 115 ('s') */
    {0x00, 0x10, 0x38, 0x10, 0x10, 0x10, 0x0C, 0x00},  /* ASCII 116 ('t') */
    {0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x38, 0x00},  /* ASCII 117 ('u') */
    {0x00, 0x00, 0x44, 0x44, 0x28, 0x28, 0x10, 0x00},  /* ASCII 118 ('v') */
    {0x00, 0x00, 0x44, 0x54, 0x54, 0x54, 0x28, 0x00},  /* ASCII 119 ('w') */
    {0x00, 0x00, 0x44, 0x28, 0x10, 0x28, 0x44, 0x00},  /* ASCII 120 ('x') */
    {0x00, 0x00, 0x44, 0x44, 0x44, 0x3C, 0x04, 0x38},  /* ASCII 121 ('y') */
    {0x00, 0x00, 0x7C, 0x08, 0x10, 0x20, 0x7C, 0x00},  /* ASCII 122 ('z') */
    {0x00, 0x0E, 0x08, 0x30, 0x08, 0x08, 0x0E, 0x00},  /* ASCII 123 ('{') */
    {0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00},  /* ASCII 124 ('|') */
    {0x00, 0x70, 0x10, 0x0C, 0x10, 0x10, 0x70, 0x00},  /* ASCII 125 ('}') */
    {0x00, 0x14, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00},  /* ASCII 126 ('~') */
    {0x3C, 0x42, 0x99, 0xA1, 0xA1, 0x99, 0x42, 0x3C}  /* ASCII 127 */
};

// Thread-safe output matrix
typedef struct
{
    const char *matrix[OUTPUT_HEIGHT][OUTPUT_WIDTH];
    color_attr_t matrix_colors[OUTPUT_HEIGHT][OUTPUT_WIDTH];
    char braille_matrix[BRAILLE_OUTPUT_HEIGHT][BRAILLE_OUTPUT_WIDTH * 4]; // UTF-8 braille takes 3 bytes + null
    color_attr_t braille_colors[BRAILLE_OUTPUT_HEIGHT][BRAILLE_OUTPUT_WIDTH];
    char ocr_matrix[OCR_OUTPUT_HEIGHT][OCR_OUTPUT_WIDTH + 1]; // OCR text + null terminator per row
    color_attr_t ocr_colors[OCR_OUTPUT_HEIGHT][OCR_OUTPUT_WIDTH];
    ula_render_mode_t render_mode;
    uint8_t border_color;
    pthread_mutex_t lock;
} ula_matrix_t;

static ula_matrix_t ula_matrix = {
    .render_mode = ULA_RENDER_BRAILLE2X4,
    .border_color = 0,
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
 * Compare two 8x8 bitmaps for similarity using Hamming distance
 * Returns: number of differing pixels (0-64), lower is more similar
 */
static int bitmap_distance(const uint8_t bitmap1[8], const uint8_t bitmap2[8])
{
    int distance = 0;
    for (int i = 0; i < 8; i++)
    {
        uint8_t xor = bitmap1[i] ^ bitmap2[i];
        // Count number of 1 bits in xor (Hamming weight)
        while (xor)
        {
            distance += xor & 1;
            xor >>= 1;
        }
    }
    return distance;
}

/**
 * Extract 8x8 bitmap for character block from VRAM
 * char_col and char_row are 0-31 and 0-23 (character grid positions)
 */
static void extract_char_bitmap(const uint8_t *vram, int char_col, int char_row, uint8_t bitmap[8])
{
    int pixel_x = char_col * 8;
    int pixel_y = char_row * 8;

    for (int row = 0; row < 8; row++)
    {
        uint8_t byte = 0;
        for (int col = 0; col < 8; col++)
        {
            if (get_pixel(vram, pixel_x + col, pixel_y + row))
            {
                byte |= (0x80 >> col); // Set bit from MSB to LSB
            }
        }
        bitmap[row] = byte;
    }
}

/**
 * Recognize character from 8x8 bitmap using font matching
 * Returns: ASCII character (32-126) or space (32) if not recognized
 */
static char recognize_character(const uint8_t bitmap[8])
{
    int best_distance = 999;
    int best_char = 0; // Space character

    // Compare against all printable ASCII characters (32-126)
    for (int i = 0; i < 96; i++)
    {
        int distance = bitmap_distance(bitmap, sinclair_font[i]);

        // Found a perfect or near-perfect match
        if (distance < best_distance)
        {
            best_distance = distance;
            best_char = i;

            // If exact match found, stop searching
            if (distance == 0)
                break;
        }
    }

    // Threshold: only accept characters with reasonable match (less than 20% different)
    if (best_distance > 12) // 12 out of 64 bits = ~19% difference
        return ' ';         // Return space for unrecognized characters

    return (char)(32 + best_char);
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
    else if (render_mode == ULA_RENDER_OCR)
    {
        // OCR mode: 32x24 character matrix with text recognition
        uint8_t bitmap[8];
        for (int char_row = 0; char_row < OCR_OUTPUT_HEIGHT; char_row++)
        {
            for (int char_col = 0; char_col < OCR_OUTPUT_WIDTH; char_col++)
            {
                // Extract 8x8 bitmap for this character position
                extract_char_bitmap(vram, char_col, char_row, bitmap);

                // Recognize character from bitmap
                ula_matrix.ocr_matrix[char_row][char_col] = recognize_character(bitmap);

                // Get attribute color for this character
                ula_matrix.ocr_colors[char_row][char_col] = get_attribute(vram, char_col * 8, char_row * 8);
            }
            // Null-terminate each row for string operations
            ula_matrix.ocr_matrix[char_row][OCR_OUTPUT_WIDTH] = '\0';
        }
    }
    else
    {
        // Block mode (default): 2x2 pixels per character
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
 * Get terminal dimensions
 * Returns terminal width and height in characters
 */
static void get_terminal_size(int *width, int *height)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
    {
        *width = w.ws_col;
        *height = w.ws_row;
    }
    else
    {
        *width = 80;
        *height = 24;
    }
}

/**
 * Render the matrix to terminal with minimal flickering
 * Uses pre-rendered buffer and atomic screen updates for 50Hz
 * Includes border rendering with centering if screen size allows
 */
void ula_render_to_terminal(void)
{
#ifndef DISABLE_RENDERING
    // 50Hz = 20ms per frame
    const long FRAME_TIME_NS = 20000000; // 20ms in nanoseconds
    static char render_buffer[65536];    // Pre-allocated render buffer (64KB for UTF-8 with borders)
    static int first_frame = 1;

    struct timespec frame_start, frame_end;
    long elapsed_ns;

    // Get terminal dimensions for centering
    int term_width, term_height;
    get_terminal_size(&term_width, &term_height);

    // Calculate content dimensions based on render mode
    int content_height;
    int content_width;
    if (ula_matrix.render_mode == ULA_RENDER_BRAILLE2X4)
    {
        content_height = BRAILLE_OUTPUT_HEIGHT;
        content_width = BRAILLE_OUTPUT_WIDTH;
    }
    else if (ula_matrix.render_mode == ULA_RENDER_OCR)
    {
        content_height = OCR_OUTPUT_HEIGHT;
        content_width = OCR_OUTPUT_WIDTH;
    }
    else
    {
        content_height = OUTPUT_HEIGHT;
        content_width = OUTPUT_WIDTH;
    }

    // Calculate border and padding sizes for centering
    // Clamp padding to 0 if content is wider than terminal
    int left_padding = (term_width > content_width) ? (term_width - content_width) / 2 : 0;
    int right_padding = (term_width > content_width) ? term_width - content_width - left_padding : 0;
    // Always try to render at least 1 line of border top/bottom if there's room
    int border_height = (term_height > content_height + 2) ? 1 : 0;

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

    // Get border color and convert to ANSI
    uint8_t border_color = ula_matrix.border_color & 0x07;
    int ansi_border_color = spectrum_to_ansi[border_color];
    // Background colors: 40-47 for standard, 100-107 for bright
    int border_bg_code = 40 + ansi_border_color;

    // Render top border if space available
    if (border_height > 0)
    {
        for (int b = 0; b < border_height; b++)
        {
            // Top border with border color (full width)
            buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
            for (int i = 0; i < term_width && buffer_pos < 65520; i++)
                render_buffer[buffer_pos++] = ' ';
            // Reset and newline
            buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m\n");
        }
    }

    // Render based on mode
    if (ula_matrix.render_mode == ULA_RENDER_BRAILLE2X4)
    {
        // Braille mode rendering with colors and border
        color_attr_t current_attr = {0, 0, 0}; // Track last color to minimize escape codes
        for (int y = 0; y < BRAILLE_OUTPUT_HEIGHT; y++)
        {
            // Left padding with border color
            if (left_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < left_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }

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
                for (int i = 0; i < 3 && ch[i] && buffer_pos < 65520; i++)
                {
                    render_buffer[buffer_pos++] = ch[i];
                }
            }
            // Right padding with border color
            if (right_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < right_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }
            // Reset colors before newline
            if (buffer_pos < 65530)
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m");
            if (buffer_pos < 65534)
                render_buffer[buffer_pos++] = '\n';
        }
    }
    else if (ula_matrix.render_mode == ULA_RENDER_OCR)
    {
        // OCR mode rendering - text-based output with colors
        color_attr_t current_attr = {0, 0, 0}; // Track last color to minimize escape codes
        for (int y = 0; y < OCR_OUTPUT_HEIGHT; y++)
        {
            // Left padding with border color
            if (left_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < left_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }

            current_attr.ink = 0xFF; // Force initial color code
            for (int x = 0; x < OCR_OUTPUT_WIDTH; x++)
            {
                color_attr_t attr = ula_matrix.ocr_colors[y][x];

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

                // Render recognized character
                char ch = ula_matrix.ocr_matrix[y][x];
                if (ch && buffer_pos < 65520)
                {
                    render_buffer[buffer_pos++] = ch;
                }
            }
            // Right padding with border color
            if (right_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < right_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }
            // Reset colors before newline
            if (buffer_pos < 65530)
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m");
            if (buffer_pos < 65534)
                render_buffer[buffer_pos++] = '\n';
        }
    }
    else
    {
        // Block mode rendering with colors and border
        color_attr_t current_attr = {0, 0, 0}; // Track last color to minimize escape codes
        for (int y = 0; y < OUTPUT_HEIGHT; y++)
        {
            // Left padding with border color
            if (left_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < left_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }

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
                while (*ch && buffer_pos < 65520)
                {
                    render_buffer[buffer_pos++] = *ch++;
                }
            }
            // Right padding with border color
            if (right_padding > 0)
            {
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
                for (int p = 0; p < right_padding && buffer_pos < 65520; p++)
                    render_buffer[buffer_pos++] = ' ';
            }
            // Reset colors before newline
            if (buffer_pos < 65530)
                buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m");
            if (buffer_pos < 65534)
                render_buffer[buffer_pos++] = '\n';
        }
    }

    // Render bottom border if space available
    if (border_height > 0)
    {
        for (int b = 0; b < border_height; b++)
        {
            // Bottom border with border color (full width)
            buffer_pos += sprintf(render_buffer + buffer_pos, "\033[%dm", border_bg_code);
            for (int i = 0; i < term_width && buffer_pos < 65520; i++)
                render_buffer[buffer_pos++] = ' ';
            // Reset and newline
            buffer_pos += sprintf(render_buffer + buffer_pos, "\033[0m\n");
        }
    }

    pthread_mutex_unlock(&ula_matrix.lock);

    // Write entire buffer at once (atomic write)
    if (buffer_pos > 0 && buffer_pos < 65536)
    {
        fwrite(render_buffer, 1, buffer_pos, stdout);
        fflush(stdout);
    }

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
    // Register emergency cleanup handler to ensure terminal is restored even on crash
    atexit(ula_emergency_cleanup);

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
    else
    {
        // Fallback: try to restore anyway
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
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

    uint8_t color_val = color & 0x07;

    pthread_mutex_lock(&ula->lock);
    ula->border_color = color_val;
    pthread_mutex_unlock(&ula->lock);

    // Also update the global matrix border color for rendering
    pthread_mutex_lock(&ula_matrix.lock);
    ula_matrix.border_color = color_val;
    pthread_mutex_unlock(&ula_matrix.lock);
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

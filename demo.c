#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ula.h"

// Global state
static uint8_t vram[SPECTRUM_RAM_SIZE];

// Simple 8x8 bitmap font for ASCII characters (space, 0-9, <, -, >)
// Each character is 8 bytes (one byte per pixel row)
static const uint8_t font_bitmaps[][8] = {
    // Space (0x20)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},

    // '-' (0x2D)
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},

    // '<' (0x3C)
    {0x00, 0x08, 0x10, 0x20, 0x10, 0x08, 0x00, 0x00},

    // '>' (0x3E)
    {0x00, 0x20, 0x10, 0x08, 0x10, 0x20, 0x00, 0x00},

    // '0' (0x30)
    {0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00},

    // '1' (0x31)
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},

    // '2' (0x32)
    {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00},

    // '3' (0x33)
    {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00},

    // '4' (0x34)
    {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00},

    // '5' (0x35)
    {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00},

    // '6' (0x36)
    {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00},

    // '7' (0x37)
    {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x00},

    // '8' (0x38)
    {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00},

    // '9' (0x39)
    {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00},

    // 'l' (0x6C)
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},

    // 'i' (0x69)
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},

    // 'n' (0x6E)
    {0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00},

    // 'e' (0x65)
    {0x00, 0x3C, 0x66, 0x7E, 0x60, 0x66, 0x3C, 0x00},
};

// Char indices in font_bitmaps
#define CHAR_SPACE 0
#define CHAR_MINUS 1
#define CHAR_LT 2
#define CHAR_GT 3
#define CHAR_0 4
#define CHAR_9 (4 + 9)

/**
 * Set attribute byte for a character cell (8x8 pixel block)
 * char_x: character column (0-31)
 * char_y: character row (0-23)
 * ink: foreground color (0-7)
 * paper: background color (0-7)
 * bright: brightness flag (0 or 1)
 */
static void set_attr(uint8_t *vram, int char_x, int char_y, uint8_t ink, uint8_t paper, uint8_t bright)
{
    if (char_x < 0 || char_x >= 32 || char_y < 0 || char_y >= 24)
        return;

    int attr_address = SPECTRUM_VRAM_SIZE + (char_y * 32) + char_x;
    if (attr_address >= SPECTRUM_RAM_SIZE)
        return;

    // Encode attribute byte: BLINK | BRIGHT | PAPER (3 bits) | INK (3 bits)
    uint8_t attr_byte = (ink & 0x07) | ((paper & 0x07) << 3) | ((bright & 0x01) << 6);
    vram[attr_address] = attr_byte;
}

/**
 * Draw an 8x8 character bitmap at position (char_x, char_y) in characters
 * Position is in 8x8 character blocks
 */
static void draw_char(uint8_t *vram, int char_x, int char_y, const uint8_t *bitmap)
{
    if (char_x < 0 || char_x >= SPECTRUM_WIDTH / 8 || char_y < 0 || char_y >= SPECTRUM_HEIGHT / 8)
        return;

    // For each row of the character
    for (int row = 0; row < 8; row++)
    {
        // Calculate VRAM address for this row
        // Spectrum layout: address = (y/8) * 2048 + (y%8) * 256 + x
        int y = char_y * 8 + row;
        int section = y / 64;
        int line_in_section = y % 64;
        int char_row = line_in_section / 8;
        int pixel_row = line_in_section % 8;
        int address = section * 2048 + char_row * 256 + pixel_row * 32 + char_x;

        if (address < SPECTRUM_RAM_SIZE)
        {
            vram[address] = bitmap[row];
        }
    }
}

/**
 * Render a string at character position (start_x, start_y) with optional color
 * If ink is 0xFF, no color is applied (use default)
 */
static void draw_text(uint8_t *vram, int start_x, int start_y, const char *text, uint8_t ink, uint8_t paper, uint8_t bright)
{
    for (int i = 0; text[i] != '\0'; i++)
    {
        char c = text[i];
        int font_index = -1;

        // Map character to font index
        if (c == ' ')
            font_index = CHAR_SPACE;
        else if (c == '-')
            font_index = CHAR_MINUS;
        else if (c == '<')
            font_index = CHAR_LT;
        else if (c == '>')
            font_index = CHAR_GT;
        else if (c >= '0' && c <= '9')
            font_index = CHAR_0 + (c - '0');
        else if (c == 'l')
            font_index = CHAR_0 + 10; // 'l' index
        else if (c == 'i')
            font_index = CHAR_0 + 11; // 'i' index
        else if (c == 'n')
            font_index = CHAR_0 + 12; // 'n' index
        else if (c == 'e')
            font_index = CHAR_0 + 13; // 'e' index

        if (font_index >= 0 && font_index < (int)(sizeof(font_bitmaps) / sizeof(font_bitmaps[0])))
        {
            int char_x = start_x + i;
            draw_char(vram, char_x, start_y, font_bitmaps[font_index]);

            // Set color attribute if ink is not 0xFF (no color)
            if (ink != 0xFF)
            {
                set_attr(vram, char_x, start_y, ink, paper, bright);
            }
        }
    }
}

/**
 * Update VRAM with text display
 */
void display_text_frame(void)
{
    // This function can be extended later
}

int main(int argc, char *argv[])
{
    // Default to braille mode
    ula_render_mode_t render_mode = ULA_RENDER_BRAILLE2X4;

    // Parse command line arguments
    if (argc > 1)
    {
        if (strcmp(argv[1], "block") == 0 || strcmp(argv[1], "2x2") == 0)
        {
            render_mode = ULA_RENDER_BLOCK2X2;
            printf("Using 2x2 block character mode\n");
        }
        else if (strcmp(argv[1], "braille") == 0 || strcmp(argv[1], "2x4") == 0)
        {
            render_mode = ULA_RENDER_BRAILLE2X4;
            printf("Using 2x4 braille character mode\n");
        }
        else
        {
            printf("Usage: %s [block|braille] (default: braille)\n", argv[0]);
            printf("  block/2x2   - Use 2x2 block characters (96x96 output)\n");
            printf("  braille/2x4 - Use 2x4 braille characters (128x48 output)\n");
            return 1;
        }
    }
    else
    {
        printf("ULA Text Renderer Demo (Braille mode)\n");
    }

    printf("Press Ctrl+C to exit\n");
    printf("Initializing...\n\n");
    fflush(stdout);

    // Initialize terminal for rendering (enables alternate screen buffer)
    ula_term_init();

    // Initialize VRAM
    memset(vram, 0, SPECTRUM_RAM_SIZE);

    // ZX Spectrum colors: 0=black, 1=blue, 2=red, 3=magenta, 4=green, 5=cyan, 6=yellow, 7=white
    // Create rainbow with colors cycling through: blue, red, magenta, green, cyan, yellow, white
    const uint8_t rainbow_colors[] = {1, 2, 3, 4, 5, 6, 7};
    const int num_colors = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

    // Draw all 24 lines into VRAM with rainbow colors
    for (int i = 0; i < 24; i++)
    {
        // Build string "<---line x--->" where x is the current line number
        char line_text[32];
        snprintf(line_text, sizeof(line_text), "<---line %d--->", i);

        // Get color for this line (cycle through rainbow)
        uint8_t color = rainbow_colors[i % num_colors];

        // Draw text at character row i with white paper background and bright foreground
        draw_text(vram, 1, i, line_text, color, 0, 1); // ink=color, paper=black, bright=1
    }

    // Render loop - keep displaying all 24 lines continuously
    for (int frame = 0; frame < 120; frame++) // Display for ~2.4 seconds
    {
        convert_vram_to_matrix(vram, render_mode);
        ula_render_to_terminal();
        usleep(20000); // 20ms = 50Hz
    }

    // Restore terminal to normal mode
    ula_term_cleanup();

    printf("Demo finished.\n");
    fflush(stdout);

    return 0;
}

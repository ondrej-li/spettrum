#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

// Include the ULA module for testing
// We need to include the source directly or link against it
#include "../ula.c"

// Test helper macros
#define TEST_ASSERT(condition, message)                                 \
    do                                                                  \
    {                                                                   \
        if (!(condition))                                               \
        {                                                               \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            return 0;                                                   \
        }                                                               \
    } while (0)

#define TEST_ASSERT_CHAR(expected, actual, message)                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (strcmp((expected), (actual)) != 0)                                                                         \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s - Expected '%s' but got '%s' (line %d)\n", message, expected, actual, __LINE__); \
            return 0;                                                                                                  \
        }                                                                                                              \
    } while (0)

// Helper to set a pixel in VRAM
static void set_pixel(uint8_t *vram, int x, int y, int value)
{
    int byte_offset = y * SPECTRUM_WIDTH_BYTES + (x / 8);
    int bit_index = 7 - (x % 8);

    if (value)
    {
        vram[byte_offset] |= (1 << bit_index);
    }
    else
    {
        vram[byte_offset] &= ~(1 << bit_index);
    }
}

/**
 * Test 1: Empty VRAM produces spaces
 */
static int test_empty_vram(void)
{
    printf("Test 1: Empty VRAM...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // Check first row is all spaces
    for (int x = 0; x < 5; x++)
    {
        TEST_ASSERT_CHAR(" ", ula_matrix.matrix[0][x], "Empty VRAM should produce spaces");
    }

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 2: Single pixel set produces ▗ (bottom-right)
 */
static int test_single_pixel_br(void)
{
    printf("Test 2: Single pixel (bottom-right)...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set only bottom-right pixel of first 2x2 block (0,0)
    // This corresponds to pixel (1,1)
    set_pixel(vram, 1, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    TEST_ASSERT_CHAR("▗", ula_matrix.matrix[0][0], "BR pixel should produce ▗");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 3: Two pixels set produces correct character
 */
static int test_two_pixels(void)
{
    printf("Test 3: Two pixels (bottom and top)...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set TL (0,0) and BR (1,1)
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 1, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // TL=1, TR=0, BL=0, BR=1 -> pattern 1001 (9) -> ▚ (diagonal /)
    TEST_ASSERT_CHAR("▚", ula_matrix.matrix[0][0], "TL+BR should produce ▚");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 4: Full block (all pixels set)
 */
static int test_full_block(void)
{
    printf("Test 4: Full block...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set all 4 pixels of first block
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 1, 0, 1);
    set_pixel(vram, 0, 1, 1);
    set_pixel(vram, 1, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    TEST_ASSERT_CHAR("█", ula_matrix.matrix[0][0], "All pixels should produce █");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 5: Top row pattern
 */
static int test_top_row(void)
{
    printf("Test 5: Top row pattern...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set TL and TR (top row)
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 1, 0, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // TL=1, TR=1, BL=0, BR=0 -> pattern 1100 (12) -> ▀
    TEST_ASSERT_CHAR("▀", ula_matrix.matrix[0][0], "Top row should produce ▀");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 6: Bottom row pattern
 */
static int test_bottom_row(void)
{
    printf("Test 6: Bottom row pattern...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set BL and BR (bottom row)
    set_pixel(vram, 0, 1, 1);
    set_pixel(vram, 1, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // TL=0, TR=0, BL=1, BR=1 -> pattern 0011 (3) -> ▄
    TEST_ASSERT_CHAR("▄", ula_matrix.matrix[0][0], "Bottom row should produce ▄");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 7: Left column pattern
 */
static int test_left_column(void)
{
    printf("Test 7: Left column pattern...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set TL and BL (left column)
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 0, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // TL=1, TR=0, BL=1, BR=0 -> pattern 1010 (10) -> ▌
    TEST_ASSERT_CHAR("▌", ula_matrix.matrix[0][0], "Left column should produce ▌");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 8: Right column pattern
 */
static int test_right_column(void)
{
    printf("Test 8: Right column pattern...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set TR and BR (right column)
    set_pixel(vram, 1, 0, 1);
    set_pixel(vram, 1, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // TL=0, TR=1, BL=0, BR=1 -> pattern 0101 (5) -> ▐
    TEST_ASSERT_CHAR("▐", ula_matrix.matrix[0][0], "Right column should produce ▐");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 9: Multiple blocks
 */
static int test_multiple_blocks(void)
{
    printf("Test 9: Multiple blocks...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // First block: full
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 1, 0, 1);
    set_pixel(vram, 0, 1, 1);
    set_pixel(vram, 1, 1, 1);

    // Second block (x=1): empty
    // Already 0

    // Third block (x=2): BR only
    set_pixel(vram, 5, 1, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    TEST_ASSERT_CHAR("█", ula_matrix.matrix[0][0], "First block should be full");
    TEST_ASSERT_CHAR(" ", ula_matrix.matrix[0][1], "Second block should be empty");
    TEST_ASSERT_CHAR("▗", ula_matrix.matrix[0][2], "Third block should be BR");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 10: Different rows
 */
static int test_different_rows(void)
{
    printf("Test 10: Different rows...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // First output row
    set_pixel(vram, 0, 0, 1);
    set_pixel(vram, 1, 0, 1);

    // Second output row (pixel y=2,3)
    set_pixel(vram, 0, 2, 1);
    set_pixel(vram, 0, 3, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    TEST_ASSERT_CHAR("▀", ula_matrix.matrix[0][0], "First row top should be ▀");
    TEST_ASSERT_CHAR("▌", ula_matrix.matrix[1][0], "Second row left should be ▌");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Helper to set an attribute byte in VRAM
 */
static void set_attribute(uint8_t *vram, int char_col, int char_row, uint8_t ink, uint8_t paper, uint8_t bright)
{
    int attr_address = SPECTRUM_VRAM_SIZE + (char_row * SPECTRUM_ATTR_COLS) + char_col;
    uint8_t attr_byte = (ink & ATTR_INK_MASK) | ((paper & 0x07) << 3) | ((bright & 0x01) << 6);
    vram[attr_address] = attr_byte;
}

/**
 * Test 11: Attributes - Default values (when uninitialized)
 */
static int test_attributes_default(void)
{
    printf("Test 11: Attributes - Default values...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set a pixel to make the block visible
    set_pixel(vram, 0, 0, 1);

    // Don't set any attribute bytes - they will be 0x00
    // In ZX Spectrum, 0x00 = black ink (0) on black paper (0)
    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    // Check the color of the first block
    color_attr_t attr = ula_matrix.matrix_colors[0][0];
    TEST_ASSERT(attr.ink == 0, "Default ink should be 0 (black)");
    TEST_ASSERT(attr.paper == 0, "Default paper should be 0 (black) when uninitialized");
    TEST_ASSERT(attr.bright == 0, "Default bright should be 0");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 12: Attributes - Set custom colors
 */
static int test_attributes_custom(void)
{
    printf("Test 12: Attributes - Custom colors...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set a pixel
    set_pixel(vram, 0, 0, 1);

    // Set attribute: yellow (ink=6) on cyan (paper=5), bright
    set_attribute(vram, 0, 0, 6, 5, 1);

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    color_attr_t attr = ula_matrix.matrix_colors[0][0];
    TEST_ASSERT(attr.ink == 6, "Ink should be 6 (yellow)");
    TEST_ASSERT(attr.paper == 5, "Paper should be 5 (cyan)");
    TEST_ASSERT(attr.bright == 1, "Bright should be 1");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 13: Attributes - Different blocks have different colors
 */
static int test_attributes_multiple_blocks(void)
{
    printf("Test 13: Attributes - Multiple blocks...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set pixels for two blocks
    set_pixel(vram, 0, 0, 1); // Block (0,0) in character grid (0,0)
    set_pixel(vram, 8, 0, 1); // Block (1,0) in character grid (1,0)

    // Set different attributes for these character cells
    set_attribute(vram, 0, 0, 2, 4, 0); // Red on magenta
    set_attribute(vram, 1, 0, 3, 1, 1); // Magenta on blue, bright

    convert_vram_to_matrix(vram, ULA_RENDER_BLOCK2X2);

    color_attr_t attr1 = ula_matrix.matrix_colors[0][0];
    color_attr_t attr2 = ula_matrix.matrix_colors[0][4]; // 8 pixels / 2 = 4

    TEST_ASSERT(attr1.ink == 2, "First block ink should be 2");
    TEST_ASSERT(attr1.paper == 4, "First block paper should be 4");
    TEST_ASSERT(attr1.bright == 0, "First block bright should be 0");

    TEST_ASSERT(attr2.ink == 3, "Second block ink should be 3");
    TEST_ASSERT(attr2.paper == 1, "Second block paper should be 1");
    TEST_ASSERT(attr2.bright == 1, "Second block bright should be 1");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test 14: Attributes - Braille mode colors
 */
static int test_attributes_braille(void)
{
    printf("Test 14: Attributes - Braille mode...\n");

    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, sizeof(uint8_t));
    TEST_ASSERT(vram != NULL, "VRAM allocation failed");

    // Set a pixel in braille block
    set_pixel(vram, 0, 0, 1);

    // Set attribute
    set_attribute(vram, 0, 0, 1, 3, 0); // Blue on magenta

    convert_vram_to_matrix(vram, ULA_RENDER_BRAILLE2X4);

    color_attr_t attr = ula_matrix.braille_colors[0][0];
    TEST_ASSERT(attr.ink == 1, "Braille block ink should be 1");
    TEST_ASSERT(attr.paper == 3, "Braille block paper should be 3");
    TEST_ASSERT(attr.bright == 0, "Braille block bright should be 0");

    free(vram);
    printf("  PASS\n");
    return 1;
}

/**
 * Test: Blink attribute extraction
 */
static int test_blink_attribute(void)
{
    printf("Test: Blink attribute extraction...\n");

    // Create test VRAM with different attribute combinations
    uint8_t *vram = calloc(SPECTRUM_RAM_SIZE, 1);

    // Set attribute at (0,0) with blink bit set
    // Attribute format: [BLINK][BRIGHT][PAPER2][PAPER1][PAPER0][INK2][INK1][INK0]
    // Position (0,0) is at attribute address SPECTRUM_VRAM_SIZE + 0
    vram[SPECTRUM_VRAM_SIZE + 0] = 0x87; // Blink=1, Bright=0, Paper=0 (black), Ink=7 (white)

    // Set attribute at (8,8) without blink bit
    vram[SPECTRUM_VRAM_SIZE + 33] = 0x47; // Blink=0, Bright=1, Paper=0 (black), Ink=7 (white)

    // Set attribute at (16,0) with blink and bright
    vram[SPECTRUM_VRAM_SIZE + 2] = 0xC2; // Blink=1, Bright=1, Paper=0 (black), Ink=2 (red)

    // Test attribute extraction at (0,0) - should have blink
    color_attr_t attr1 = get_attribute(vram, 0, 0);
    TEST_ASSERT(attr1.blink == 1, "Blink should be set at (0,0)");
    TEST_ASSERT(attr1.ink == 7, "Ink should be white (7) at (0,0)");
    TEST_ASSERT(attr1.paper == 0, "Paper should be black (0) at (0,0)");
    TEST_ASSERT(attr1.bright == 0, "Bright should be 0 at (0,0)");

    // Test attribute extraction at (8,8) - should not have blink
    color_attr_t attr2 = get_attribute(vram, 8, 8);
    TEST_ASSERT(attr2.blink == 0, "Blink should not be set at (8,8)");
    TEST_ASSERT(attr2.bright == 1, "Bright should be 1 at (8,8)");

    // Test attribute extraction at (16,0) - should have both blink and bright
    color_attr_t attr3 = get_attribute(vram, 16, 0);
    TEST_ASSERT(attr3.blink == 1, "Blink should be set at (16,0)");
    TEST_ASSERT(attr3.bright == 1, "Bright should be set at (16,0)");
    TEST_ASSERT(attr3.ink == 2, "Ink should be red (2) at (16,0)");

    free(vram);
    return 1;
}

/**
 * Run all tests
 */
int main(void)
{
    printf("=== ULA Module Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++;
    if (test_empty_vram())
        passed++;

    total++;
    if (test_single_pixel_br())
        passed++;

    total++;
    if (test_two_pixels())
        passed++;

    total++;
    if (test_full_block())
        passed++;

    total++;
    if (test_top_row())
        passed++;

    total++;
    if (test_bottom_row())
        passed++;

    total++;
    if (test_left_column())
        passed++;

    total++;
    if (test_right_column())
        passed++;

    total++;
    if (test_multiple_blocks())
        passed++;

    total++;
    if (test_different_rows())
        passed++;

    total++;
    if (test_attributes_default())
        passed++;

    total++;
    if (test_attributes_custom())
        passed++;

    total++;
    if (test_attributes_multiple_blocks())
        passed++;

    total++;
    if (test_attributes_braille())
        passed++;

    total++;
    if (test_blink_attribute())
        passed++;

    printf("\n=== Results ===\n");
    printf("Passed: %d/%d\n", passed, total);

    if (passed == total)
    {
        printf("All tests passed!\n");
        return 0;
    }
    else
    {
        printf("Some tests failed!\n");
        return 1;
    }
}

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

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

    convert_vram_to_matrix(vram);

    TEST_ASSERT_CHAR("▀", ula_matrix.matrix[0][0], "First row top should be ▀");
    TEST_ASSERT_CHAR("▌", ula_matrix.matrix[1][0], "Second row left should be ▌");

    free(vram);
    printf("  PASS\n");
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

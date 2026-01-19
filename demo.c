#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "ula.h"

// Global state
static volatile int running = 1;
static uint8_t vram[SPECTRUM_RAM_SIZE];

/**
 * Set pixel in VRAM using Spectrum's memory layout
 * Formula: address = ((y / 8) * 2048) + ((y % 8) * 256) + (x / 8)
 */
static void set_pixel(uint8_t *vram, int x, int y)
{
    // Spectrum video RAM layout
    int section = y / 64;                // Which third (0, 1, or 2)
    int line_in_section = y % 64;        // Line within that third
    int char_row = line_in_section / 8;  // Character row (0-7)
    int pixel_row = line_in_section % 8; // Pixel within character (0-7)
    int char_col = x / 8;                // Character column (0-31)

    // Calculate address
    int address = section * 2048 + char_row * 256 + pixel_row * 32 + char_col;

    if (address >= SPECTRUM_RAM_SIZE)
        return;

    int bit_index = 7 - (x % 8);
    vram[address] |= (1 << bit_index);
}

/**
 * Update VRAM with animation
 */
void *animation_thread_func(void *arg)
{
    (void)arg;

    for (int frame = 0; running; frame++)
    {
        // Clear VRAM
        memset(vram, 0, SPECTRUM_RAM_SIZE);

        // Draw animated pattern
        int frame_offset = frame % 100;

        // Draw a vertical bar moving across screen
        for (int y = 0; y < SPECTRUM_HEIGHT; y++)
        {
            int x = (frame_offset * 2) % SPECTRUM_WIDTH;
            set_pixel(vram, x, y);
        }

        // Draw a horizontal bar moving down
        int y = (frame_offset * 2) % SPECTRUM_HEIGHT;
        for (int x = 0; x < SPECTRUM_WIDTH; x++)
        {
            set_pixel(vram, x, y);
        }

        // Update matrix from VRAM and render
        convert_vram_to_matrix(vram);
        ula_render_to_terminal();

        // Small sleep to avoid overwhelming CPU
        usleep(20000); // ~50Hz for animation
    }

    return NULL;
}

int main(void)
{
    printf("ULA Terminal Renderer Demo\n");
    printf("Press Ctrl+C to exit\n");
    printf("Initializing...\n\n");
    fflush(stdout);

    pthread_t anim_thread;

    // Initialize terminal for rendering (enables alternate screen buffer)
    ula_term_init();

    // Initialize VRAM with some pattern
    memset(vram, 0, SPECTRUM_RAM_SIZE);

    // Convert initial VRAM to matrix
    convert_vram_to_matrix(vram);
    ula_render_to_terminal();

    // Create animation thread only (rendering happens in main loop)
    pthread_create(&anim_thread, NULL, animation_thread_func, NULL);

    // Main rendering loop
    for (int i = 0; i < 50 && running; i++) // ~1 second at 50Hz
    {
        ula_render_to_terminal();
        usleep(20000);
    }

    // Cleanup
    running = 0;
    pthread_cancel(anim_thread);
    pthread_join(anim_thread, NULL);

    // Restore terminal to normal mode
    ula_term_cleanup();

    printf("Demo finished.\n");
    fflush(stdout);

    return 0;
}

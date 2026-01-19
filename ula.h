#ifndef ULA_H
#define ULA_H

#include <stdint.h>
#include <pthread.h>

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

// ULA display structure
typedef struct
{
    int width;
    int height;
    uint8_t vram[SPECTRUM_RAM_SIZE];
    uint8_t border_color;
    pthread_mutex_t lock;
} ula_t;

/**
 * Initialize ULA display
 * @param width Display width in pixels
 * @param height Display height in pixels
 * @return Pointer to ULA structure, or NULL on error
 */
ula_t *ula_init(int width, int height);

/**
 * Cleanup ULA display
 * @param ula ULA structure to clean up
 */
void ula_cleanup(ula_t *ula);

/**
 * Set border color
 * @param ula ULA structure
 * @param color Border color (0-7)
 */
void ula_set_border_color(ula_t *ula, uint8_t color);

/**
 * Get current border color
 * @param ula ULA structure
 * @return Border color value
 */
uint8_t ula_get_border_color(const ula_t *ula);

/**
 * Write to video RAM
 * @param ula ULA structure
 * @param addr Address in video RAM
 * @param value Byte to write
 */
void ula_write_vram(ula_t *ula, uint16_t addr, uint8_t value);

/**
 * Read from video RAM
 * @param ula ULA structure
 * @param addr Address in video RAM
 * @return Byte at address
 */
uint8_t ula_read_vram(const ula_t *ula, uint16_t addr);

/**
 * Convert entire video RAM to matrix
 * This is the core ULA conversion function
 */
void convert_vram_to_matrix(const uint8_t *vram);

/**
 * ULA thread function
 * Continuously monitors video RAM and updates matrix
 */
void *ula_thread_func(void *arg);

/**
 * Get current matrix with thread safety
 * Caller should free the returned pointer
 */
char **ula_get_matrix(void);

/**
 * Free matrix allocated by ula_get_matrix
 */
void ula_free_matrix(char **matrix);

/**
 * Render the matrix to terminal at 50Hz
 * Moves cursor to top-left, renders all lines, handles frame timing
 */
void ula_render_to_terminal(void);

/**
 * Render loop function - continuously renders at 50Hz
 * Useful for threading with display output
 */
void *ula_render_thread_func(void *arg);

/**
 * Get pointer to matrix lock for synchronization
 */
pthread_mutex_t *ula_get_matrix_lock(void);

/**
 * Get the character at specific matrix position
 */
const char *ula_get_char(int x, int y);

/**
 * Initialize terminal for rendering
 * Sets up alternate screen buffer and disables echo for performance
 */
void ula_term_init(void);

/**
 * Cleanup terminal after rendering
 * Restores normal mode and shows cursor
 */
void ula_term_cleanup(void);

#endif // ULA_H

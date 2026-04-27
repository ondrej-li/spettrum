#ifndef BEEPER_H
#define BEEPER_H

#include <stdint.h>
#include <stdbool.h>

// Forward declaration of opaque beeper state
typedef struct beeper_state_s beeper_state_t;

// Beeper statistics for debugging
typedef struct
{
    uint64_t events_queued;     // Total events added to buffer
    uint64_t events_rendered;   // Total events processed by audio thread
    uint64_t buffer_overruns;   // Times we had to drop events (buffer full)
    uint64_t buffer_underruns;  // Times audio thread had no data
    uint32_t buffer_usage_peak; // Peak buffer usage (for tuning)
} beeper_stats_t;

/**
 * Initialize the beeper audio system
 *
 * @param cpu_clock_hz Z80 CPU clock rate in Hz (typically 3500000 for Spectrum)
 * @param sample_rate Audio output sample rate in Hz (typically 44100)
 * @param enabled Whether audio should be enabled at startup
 * @return Pointer to beeper state, or NULL on failure
 */
beeper_state_t *beeper_init(uint32_t cpu_clock_hz, uint32_t sample_rate, bool enabled);

/**
 * Destroy the beeper audio system and free resources
 *
 * @param beeper Beeper state to destroy
 */
void beeper_destroy(beeper_state_t *beeper);

/**
 * Start audio playback (spawns audio thread)
 *
 * @param beeper Beeper state
 * @return true on success, false on failure
 */
bool beeper_start(beeper_state_t *beeper);

/**
 * Stop audio playback
 *
 * @param beeper Beeper state
 */
void beeper_stop(beeper_state_t *beeper);

/**
 * Update beeper state when port 0xFE is written
 * Called from emulator thread when OUT (0xFE), A is executed
 *
 * @param beeper Beeper state
 * @param cpu_cycle Current CPU cycle count (for timing)
 * @param mic_bit State of MIC bit (bit 3 of port 0xFE)
 * @param beeper_bit State of beeper bit (bit 4 of port 0xFE)
 */
void beeper_update(beeper_state_t *beeper, uint64_t cpu_cycle, uint8_t mic_bit, uint8_t beeper_bit);

/**
 * Set audio output volume
 *
 * @param beeper Beeper state
 * @param volume Volume level (0-100)
 */
void beeper_set_volume(beeper_state_t *beeper, uint8_t volume);

/**
 * Enable or disable audio output on the fly
 *
 * @param beeper Beeper state
 * @param enabled true to enable, false to disable
 */
void beeper_set_enabled(beeper_state_t *beeper, bool enabled);

/**
 * Get beeper statistics for debugging/monitoring
 *
 * @param beeper Beeper state
 * @param stats Pointer to stats structure to fill
 */
void beeper_get_stats(beeper_state_t *beeper, beeper_stats_t *stats);

/**
 * Reset beeper state (clear buffer, reset statistics)
 *
 * @param beeper Beeper state
 */
void beeper_reset(beeper_state_t *beeper);

#endif // BEEPER_H

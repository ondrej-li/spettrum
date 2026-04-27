#include "beeper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#endif

// Debug logging (define BEEPER_DEBUG to enable)
#ifdef BEEPER_DEBUG
static FILE *beeper_debug_log = NULL;
#define BEEPER_LOG(...)                                        \
    do                                                         \
    {                                                          \
        if (!beeper_debug_log)                                 \
            beeper_debug_log = fopen("beeper_debug.log", "w"); \
        if (beeper_debug_log)                                  \
        {                                                      \
            fprintf(beeper_debug_log, __VA_ARGS__);            \
            fflush(beeper_debug_log);                          \
        }                                                      \
    } while (0)
#else
#define BEEPER_LOG(...) ((void)0)
#endif

// Ring buffer size for beeper events (must be power of 2 for efficient modulo)
// Smaller buffer = lower latency
#define BEEPER_RING_BUFFER_SIZE 4096
#define BEEPER_RING_BUFFER_MASK (BEEPER_RING_BUFFER_SIZE - 1)

// Audio configuration
#define BEEPER_CHANNELS 2 // Stereo output

// Beeper event: records state change with CPU cycle timestamp
typedef struct
{
    uint64_t cpu_cycle;
    uint8_t mic_bit;
    uint8_t beeper_bit;
} beeper_event_t;

// Lock-free ring buffer for passing events from emulator thread to audio thread
typedef struct
{
    beeper_event_t events[BEEPER_RING_BUFFER_SIZE];
    atomic_uint_fast32_t head; // Write index (emulator thread)
    atomic_uint_fast32_t tail; // Read index (audio thread)
} beeper_ring_buffer_t;

// Beeper state (opaque to users)
struct beeper_state_s
{
#ifdef __APPLE__
    AudioComponentInstance audio_unit;
#endif
    beeper_ring_buffer_t ring_buffer;
    beeper_stats_t stats;

    uint32_t cpu_clock_hz; // Z80 CPU clock rate
    uint32_t sample_rate;  // Audio sample rate
    float volume;          // Volume scaling (0.0 to 1.0)
    bool enabled;          // Audio output enabled
    bool running;          // Audio playback active

    // Audio thread state (updated in render callback)
    uint64_t rendered_cpu_cycle; // Last CPU cycle processed by audio thread
    uint8_t rendered_mic_bit;    // Last MIC state rendered
    uint8_t rendered_beeper_bit; // Last beeper state rendered

    // Emulator thread state (updated in beeper_update)
    uint8_t queued_mic_bit;    // Last MIC state queued
    uint8_t queued_beeper_bit; // Last beeper state queued

    // Cross-thread: latest CPU cycle seen by beeper_update (emulator thread writes, audio thread reads)
    atomic_uint_fast64_t latest_cpu_cycle;
};

// Ring buffer functions
static inline uint32_t ring_buffer_count(beeper_ring_buffer_t *rb)
{
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (head - tail) & BEEPER_RING_BUFFER_MASK;
}

static inline bool ring_buffer_push(beeper_ring_buffer_t *rb, beeper_event_t *event)
{
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t next_head = (head + 1) & BEEPER_RING_BUFFER_MASK;
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    // Check if buffer is full
    if (next_head == tail)
    {
        return false; // Buffer full
    }

    rb->events[head] = *event;
    atomic_store_explicit(&rb->head, next_head, memory_order_release);
    return true;
}

static inline bool ring_buffer_pop(beeper_ring_buffer_t *rb, beeper_event_t *event)
{
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    // Check if buffer is empty
    if (tail == head)
    {
        return false; // Buffer empty
    }

    *event = rb->events[tail];
    uint32_t next_tail = (tail + 1) & BEEPER_RING_BUFFER_MASK;
    atomic_store_explicit(&rb->tail, next_tail, memory_order_release);
    return true;
}

static inline bool ring_buffer_peek(beeper_ring_buffer_t *rb, beeper_event_t *event)
{
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    if (tail == head)
        return false;
    *event = rb->events[tail];
    return true;
}

static inline void ring_buffer_clear(beeper_ring_buffer_t *rb)
{
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->head, 0, memory_order_release);
}

#ifdef __APPLE__
// CoreAudio render callback - called by audio thread to fill audio buffer
//
// The audio clock (rendered_cpu_cycle) advances at a steady rate matching
// real-time: window_len CPU cycles per callback. Events are consumed as
// the clock reaches them. If the CPU runs ahead in bursts, events queue
// up and drain naturally over subsequent callbacks at the correct pace.
static OSStatus audio_render_callback(
    void *inRefCon,
    AudioUnitRenderActionFlags *ioActionFlags,
    const AudioTimeStamp *inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList *ioData)
{
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;

    beeper_state_t *beeper = (beeper_state_t *)inRefCon;

    // Safety / disabled check — output silence
    if (!beeper || !beeper->enabled || !beeper->running)
    {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++)
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        return noErr;
    }

    if (ioData->mNumberBuffers < 1)
        return noErr;
    float *out = (float *)ioData->mBuffers[0].mData;
    if (!out)
        return noErr;

    double cps = (double)beeper->cpu_clock_hz / (double)beeper->sample_rate;
    uint64_t window_len = (uint64_t)(inNumberFrames * cps);
    uint64_t window_start = beeper->rendered_cpu_cycle;

    // Seed the audio clock from the first available event
    if (window_start == 0)
    {
        beeper_event_t peek;
        if (ring_buffer_peek(&beeper->ring_buffer, &peek))
        {
            window_start = peek.cpu_cycle;
        }
        else
        {
            // No events yet — silence, don't advance clock
            for (UInt32 f = 0; f < inNumberFrames; f++)
            {
                out[f * 2] = 0.0f;
                out[f * 2 + 1] = 0.0f;
            }
            return noErr;
        }
    }

    uint64_t window_end = window_start + window_len;

    // If we've fallen far behind the CPU (>1 frame), skip forward.
    // This handles long pauses or large bursts.
    beeper_event_t peek;
    if (ring_buffer_peek(&beeper->ring_buffer, &peek))
    {
        uint64_t one_frame = beeper->cpu_clock_hz / 50; // ~70000 cycles
        if (peek.cpu_cycle > window_end + one_frame)
        {
            // Drain stale events, keeping last state
            beeper_event_t ev;
            uint64_t target = peek.cpu_cycle - window_len;
            while (ring_buffer_peek(&beeper->ring_buffer, &ev) &&
                   ev.cpu_cycle < target)
            {
                ring_buffer_pop(&beeper->ring_buffer, &ev);
                beeper->rendered_beeper_bit = ev.beeper_bit;
                beeper->stats.events_rendered++;
            }
            window_start = target;
            window_end = window_start + window_len;
        }
    }

    // Render sample-by-sample, consuming events from the ring buffer
    // as the audio clock reaches each event's timestamp.
    uint8_t current_beeper = beeper->rendered_beeper_bit;

    for (UInt32 frame = 0; frame < inNumberFrames; frame++)
    {
        uint64_t frame_cycle = window_start + (uint64_t)(frame * cps);

        beeper_event_t ev;
        while (ring_buffer_peek(&beeper->ring_buffer, &ev) &&
               ev.cpu_cycle <= frame_cycle)
        {
            ring_buffer_pop(&beeper->ring_buffer, &ev);
            current_beeper = ev.beeper_bit;
            beeper->stats.events_rendered++;
        }

        float amp = current_beeper ? beeper->volume : -beeper->volume;
        out[frame * 2] = amp;
        out[frame * 2 + 1] = amp;
    }

    // Advance the steady audio clock
    beeper->rendered_cpu_cycle = window_end;
    beeper->rendered_beeper_bit = current_beeper;

    // If the ring buffer is now empty, the tone has ended.
    // Snap the audio clock forward to the CPU's current position
    // so we don't keep replaying the last beeper state while catching up.
    if (!ring_buffer_peek(&beeper->ring_buffer, &peek))
    {
        uint64_t cpu_now = atomic_load_explicit(&beeper->latest_cpu_cycle,
                                                memory_order_relaxed);
        if (cpu_now > beeper->rendered_cpu_cycle)
        {
            beeper->rendered_cpu_cycle = cpu_now;
        }
    }

    return noErr;
}
#endif

beeper_state_t *beeper_init(uint32_t cpu_clock_hz, uint32_t sample_rate, bool enabled)
{
    BEEPER_LOG("Beeper: Initializing (cpu_clock=%u, sample_rate=%u, enabled=%d)\n",
               cpu_clock_hz, sample_rate, enabled);

    beeper_state_t *beeper = calloc(1, sizeof(beeper_state_t));
    if (!beeper)
    {
        fprintf(stderr, "Failed to allocate beeper state\n");
        return NULL;
    }

    beeper->cpu_clock_hz = cpu_clock_hz;
    beeper->sample_rate = sample_rate;
    beeper->volume = 0.5f; // Default 50% volume
    beeper->enabled = enabled;
    beeper->running = false;

    // Initialize state to invalid values to force first update to be queued
    beeper->queued_mic_bit = 0xFF;
    beeper->queued_beeper_bit = 0xFF;
    beeper->rendered_mic_bit = 0;
    beeper->rendered_beeper_bit = 0;
    beeper->rendered_cpu_cycle = 0;

    ring_buffer_clear(&beeper->ring_buffer);
    memset(&beeper->stats, 0, sizeof(beeper->stats));

#ifdef __APPLE__
    // Use HALOutput audio unit with the system default output device
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component)
    {
        fprintf(stderr, "beeper: failed to find HALOutput audio component\n");
        free(beeper);
        return NULL;
    }

    OSStatus status = AudioComponentInstanceNew(component, &beeper->audio_unit);
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to create audio unit: %d\n", (int)status);
        free(beeper);
        return NULL;
    }

    // Enable output on bus 0 (speaker)
    UInt32 enableOutput = 1;
    status = AudioUnitSetProperty(
        beeper->audio_unit,
        kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Output,
        0, // bus 0 = output
        &enableOutput,
        sizeof(enableOutput));
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to enable output: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    // Disable input on bus 1 (microphone — not needed)
    UInt32 disableInput = 0;
    status = AudioUnitSetProperty(
        beeper->audio_unit,
        kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Input,
        1, // bus 1 = input
        &disableInput,
        sizeof(disableInput));
    if (status != noErr)
    {
        // Non-fatal — input may already be disabled
        fprintf(stderr, "beeper: warning: failed to disable input: %d\n", (int)status);
    }

    // Set the default output device
    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    AudioDeviceID outputDevice = 0;
    UInt32 deviceSize = sizeof(outputDevice);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop,
                                        0, NULL, &deviceSize, &outputDevice);
    if (status != noErr || outputDevice == 0)
    {
        fprintf(stderr, "beeper: failed to get default output device: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    status = AudioUnitSetProperty(
        beeper->audio_unit,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &outputDevice,
        sizeof(outputDevice));
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to set output device: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    // Set our desired stream format on bus 0 input scope (what we feed to the unit)
    AudioStreamBasicDescription audio_format = {0};
    audio_format.mSampleRate = sample_rate;
    audio_format.mFormatID = kAudioFormatLinearPCM;
    audio_format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    audio_format.mBytesPerPacket = sizeof(float) * BEEPER_CHANNELS;
    audio_format.mFramesPerPacket = 1;
    audio_format.mBytesPerFrame = sizeof(float) * BEEPER_CHANNELS;
    audio_format.mChannelsPerFrame = BEEPER_CHANNELS;
    audio_format.mBitsPerChannel = sizeof(float) * 8;

    status = AudioUnitSetProperty(
        beeper->audio_unit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        0, // bus 0
        &audio_format,
        sizeof(audio_format));
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to set stream format: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    // Set render callback
    AURenderCallbackStruct cb = {0};
    cb.inputProc = audio_render_callback;
    cb.inputProcRefCon = beeper;

    status = AudioUnitSetProperty(
        beeper->audio_unit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        0, // bus 0
        &cb,
        sizeof(cb));
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to set render callback: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    // Initialize the audio unit
    status = AudioUnitInitialize(beeper->audio_unit);
    if (status != noErr)
    {
        fprintf(stderr, "beeper: failed to initialize audio unit: %d\n", (int)status);
        AudioComponentInstanceDispose(beeper->audio_unit);
        free(beeper);
        return NULL;
    }

    fprintf(stderr, "beeper: audio unit initialized (HALOutput, device=%u, %u Hz, %d ch)\n",
            (unsigned)outputDevice, sample_rate, BEEPER_CHANNELS);
#endif

    BEEPER_LOG("Beeper: Initialization complete\n");
    return beeper;
}

void beeper_destroy(beeper_state_t *beeper)
{
    if (!beeper)
        return;

    BEEPER_LOG("Beeper: Destroying\n");
    beeper_stop(beeper);

#ifdef __APPLE__
    if (beeper->audio_unit)
    {
        AudioUnitUninitialize(beeper->audio_unit);
        AudioComponentInstanceDispose(beeper->audio_unit);
    }
#endif

    free(beeper);
    BEEPER_LOG("Beeper: Destroyed\n");
#ifdef BEEPER_DEBUG
    if (beeper_debug_log)
    {
        fclose(beeper_debug_log);
        beeper_debug_log = NULL;
    }
#endif
}

bool beeper_start(beeper_state_t *beeper)
{
    if (!beeper || beeper->running)
        return false;

    // Set running BEFORE starting audio unit so the render callback
    // sees running=true from its very first invocation
    beeper->running = true;
    beeper->enabled = true;

#ifdef __APPLE__
    OSStatus status = AudioOutputUnitStart(beeper->audio_unit);
    if (status != noErr)
    {
        beeper->running = false;
        fprintf(stderr, "Failed to start audio output: %d\n", (int)status);
        return false;
    }
#endif

    return true;
}

void beeper_stop(beeper_state_t *beeper)
{
    if (!beeper || !beeper->running)
    {
        return;
    }

#ifdef __APPLE__
    AudioOutputUnitStop(beeper->audio_unit);
#endif

    beeper->running = false;
}

void beeper_update(beeper_state_t *beeper, uint64_t cpu_cycle, uint8_t mic_bit, uint8_t beeper_bit)
{
    if (!beeper || !beeper->enabled)
    {
        return;
    }

    // Only queue event if state actually changed (check against last QUEUED state)
    if (mic_bit == beeper->queued_mic_bit && beeper_bit == beeper->queued_beeper_bit)
    {
        return;
    }

    beeper_event_t event;
    event.cpu_cycle = cpu_cycle;
    event.mic_bit = mic_bit;
    event.beeper_bit = beeper_bit;

    if (ring_buffer_push(&beeper->ring_buffer, &event))
    {
        // Update last queued state to avoid queueing duplicates
        beeper->queued_mic_bit = mic_bit;
        beeper->queued_beeper_bit = beeper_bit;

        // Track latest CPU cycle for audio thread sync
        atomic_store_explicit(&beeper->latest_cpu_cycle, cpu_cycle,
                              memory_order_relaxed);

        beeper->stats.events_queued++;

        // Track peak buffer usage
        uint32_t usage = ring_buffer_count(&beeper->ring_buffer);
        if (usage > beeper->stats.buffer_usage_peak)
        {
            beeper->stats.buffer_usage_peak = usage;
        }
    }
    else
    {
        // Buffer full - overrun
        beeper->stats.buffer_overruns++;
    }
}

void beeper_set_volume(beeper_state_t *beeper, uint8_t volume)
{
    if (!beeper)
        return;

    // Clamp to 0-100 range and convert to 0.0-1.0
    if (volume > 100)
        volume = 100;
    beeper->volume = volume / 100.0f;
    BEEPER_LOG("Beeper: Set volume to %d (%f)\n", volume, beeper->volume);
}

void beeper_set_enabled(beeper_state_t *beeper, bool enabled)
{
    if (!beeper)
        return;
    BEEPER_LOG("Beeper: Set enabled to %d\n", enabled);
    beeper->enabled = enabled;
}

void beeper_get_stats(beeper_state_t *beeper, beeper_stats_t *stats)
{
    if (!beeper || !stats)
        return;
    *stats = beeper->stats;
}

void beeper_reset(beeper_state_t *beeper)
{
    if (!beeper)
        return;

    ring_buffer_clear(&beeper->ring_buffer);
    memset(&beeper->stats, 0, sizeof(beeper->stats));
    beeper->rendered_cpu_cycle = 0;
    beeper->rendered_mic_bit = 0;
    beeper->rendered_beeper_bit = 0;
    beeper->queued_mic_bit = 0;
    beeper->queued_beeper_bit = 0;
}

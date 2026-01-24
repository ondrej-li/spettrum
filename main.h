#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>

// Version information
#define SPETTRUM_VERSION_MAJOR 0
#define SPETTRUM_VERSION_MINOR 1
#define SPETTRUM_VERSION_PATCH 0

// Memory configuration
#define SPETTRUM_ROM_SIZE (16 * 1024)     // 16 KB ROM
#define SPETTRUM_RAM_SIZE (48 * 1024)     // 48 KB RAM
#define SPETTRUM_TOTAL_MEMORY (64 * 1024) // 64 KB total
#define SPETTRUM_VRAM_START 0x4000        // Video RAM starts at 0x4000
#define SPETTRUM_VRAM_SIZE 6912           // Video RAM is 6912 bytes (256x192 pixels + attributes)

#include "z80.h"
#include "ula.h"

// Emulator state
typedef struct
{
    z80_emulator_t *cpu;
    ula_t *display;
    uint8_t memory[SPETTRUM_TOTAL_MEMORY];
    volatile int running;
    FILE *disasm_file;          // File for disassembly output
    volatile int dump_memory;   // Flag to trigger memory dump
    int dump_count;             // Counter for dump filenames
    volatile int paused;        // Pause state
    volatile int speed_delay;   // Delay in microseconds (0 = full speed)
    volatile int step_mode;     // Step mode: execute one instruction at a time
    const char *simulated_keys; // Keys to simulate (injected automatically)

    // Debug tracking
    uint16_t last_pc[10];        // Last 10 PC values
    uint8_t last_opcode[10];     // Last 10 opcodes
    int history_index;           // Circular buffer index
    uint64_t total_instructions; // Total instructions executed

    // Anomaly tracking
    uint64_t warnings_pc_in_vram; // Count of PC in VRAM warnings
    uint64_t warnings_sp_in_vram; // Count of SP in video RAM warnings
    uint16_t last_warn_pc;        // Last PC that triggered VRAM warning
    uint16_t last_warn_sp;        // Last SP that triggered VRAM warning
    uint16_t warn_pc_history[5];  // Last 5 PC values before VRAM execution
    uint16_t warn_sp_at_fault;    // SP value when PC-in-VRAM occurred
    uint16_t warn_pc_at_sp_fault; // PC value when SP-in-VRAM occurred

    // Warning buffer for display after emulation completes
    char *warning_buffer;       // Dynamically allocated buffer for warnings
    size_t warning_buffer_size; // Total size of buffer
    size_t warning_buffer_pos;  // Current position in buffer
} spettrum_emulator_t;

#endif
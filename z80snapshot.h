/**
 * Z80 Snapshot File Handler
 *
 * Implements support for loading .z80 snapshot files as defined by:
 * https://worldofspectrum.org/faq/reference/z80format.htm
 *
 * Supports:
 * - Version 1 (48K only): 30-byte header + compressed memory
 * - Version 2/3 (48K, 128K, +3, etc.): Extended header + multiple memory blocks
 * - CPU state restoration (registers, flags, interrupts)
 * - Memory state restoration (VRAM and RAM)
 * - Compression/decompression (RLE: ED ED xx yy format)
 */

#ifndef Z80_SNAPSHOT_H
#define Z80_SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>
#include "z80.h"

// Z80 Snapshot File Format Constants
#define Z80_HEADER_SIZE 30
#define Z80_V1_MEMORY_SIZE (48 * 1024) // 48KB
#define Z80_V2_EXTRA_HEADER_MIN_SIZE 2 // At least length field
#define Z80_MEMORY_BLOCK_HEADER_SIZE 3 // Length (2 bytes) + Page (1 byte)

// Version identifiers
#define Z80_VERSION_1 1
#define Z80_VERSION_2 2
#define Z80_VERSION_3 3

// Hardware modes (version 2/3 field at byte 34)
typedef enum
{
    Z80_HARDWARE_48K = 0,
    Z80_HARDWARE_48K_IF1 = 1,
    Z80_HARDWARE_SAMRAM = 2,
    Z80_HARDWARE_48K_MGT = 3,
    Z80_HARDWARE_128K = 4,
    Z80_HARDWARE_128K_IF1 = 5,
    Z80_HARDWARE_128K_MGT = 6,
    Z80_HARDWARE_PLUS3 = 7,
    Z80_HARDWARE_PLUS2A = 13,
    Z80_HARDWARE_PENTAGON = 9,
    Z80_HARDWARE_SCORPION = 10,
} z80_hardware_mode_t;

// Memory block page numbers for 48K mode
typedef enum
{
    Z80_PAGE_48K_ROM = 0,  // 0x0000-0x3FFF (ROM)
    Z80_PAGE_48K_VRAM = 8, // 0x4000-0x7FFF (Video RAM)
    Z80_PAGE_48K_RAM4 = 4, // 0x8000-0xBFFF (RAM page 4)
    Z80_PAGE_48K_RAM5 = 5, // 0xC000-0xFFFF (RAM page 5)
} z80_memory_page_48k_t;

// Version 1 header (30 bytes)
typedef struct
{
    uint8_t a, f;           // A and F registers (0-1)
    uint16_t bc, hl;        // BC and HL register pairs (2-5)
    uint16_t pc;            // Program counter (6-7)
    uint16_t sp;            // Stack pointer (8-9)
    uint8_t i, r;           // I and R registers (10-11)
    uint8_t flags;          // Bit flags (12)
    uint16_t de;            // DE register pair (13-14)
    uint16_t bc_, de_, hl_; // Alternate register pairs (15-20)
    uint8_t a_, f_;         // Alternate A and F (21-22)
    uint16_t iy, ix;        // IY and IX registers (23-26)
    uint8_t iff1, iff2;     // Interrupt flip-flops (27-28)
    uint8_t im;             // Interrupt mode (29)
} z80_v1_header_t;

// Version 2/3 extended header (starts at byte 30)
typedef struct
{
    uint16_t extra_header_len;   // Length of extended header (30-31)
    uint16_t pc;                 // Program counter (32-33, overrides V1 if non-zero)
    uint8_t hardware_mode;       // Hardware type (34)
    uint8_t paging_register;     // OUT to 0x7ffd or equivalent (35)
    uint8_t interface_rom;       // Interface ROM flag (36)
    uint8_t emulation_flags;     // Emulation flags (37)
    uint8_t sound_register;      // Last OUT to 0xfffd (38)
    uint8_t sound_registers[16]; // Sound chip registers (39-54)
    // Additional fields in V3 only...
} z80_v2_header_t;

// Memory block header (part of compressed memory)
typedef struct
{
    uint16_t compressed_length; // Length of compressed data
    uint8_t page_number;        // Memory page number
} z80_memory_block_header_t;

/**
 * Load and restore CPU state from a Z80 snapshot file
 *
 * @param filename      Path to .z80 snapshot file
 * @param cpu           Z80 CPU emulator to restore
 * @param memory        64KB memory buffer to restore
 * @return              0 on success, -1 on error
 */
int z80_snapshot_load(const char *filename, z80_emulator_t *cpu, uint8_t *memory);

/**
 * Decompress RLE-encoded memory block
 *
 * @param compressed    Compressed data buffer
 * @param comp_len      Length of compressed data
 * @param decompressed  Output buffer for decompressed data
 * @param decomp_len    Expected decompressed length
 * @return              Actual decompressed length, or -1 on error
 */
int z80_decompress_block(const uint8_t *compressed, size_t comp_len,
                         uint8_t *decompressed, size_t decomp_len);

/**
 * Determine Z80 snapshot version from file header
 *
 * @param filename      Path to .z80 file
 * @return              Z80_VERSION_1, Z80_VERSION_2, Z80_VERSION_3, or -1 on error
 */
int z80_snapshot_get_version(const char *filename);

#endif // Z80_SNAPSHOT_H

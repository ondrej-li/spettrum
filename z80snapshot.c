/**
 * Z80 Snapshot File Handler - Implementation
 *
 * Loads .z80 snapshot files (versions 1, 2, and 3) and restores CPU and memory state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "z80snapshot.h"

/**
 * Decompress RLE-encoded memory block
 *
 * Z80 format uses: ED ED xx yy = byte yy repeated xx times
 * Only sequences of 5+ identical bytes are compressed.
 * Exceptions:
 *   - ED sequences (even 2 EDs): ED ED 02 ED
 *   - Single ED followed by data: not taken into block
 * End marker: 00 ED ED 00
 */
int z80_decompress_block(const uint8_t *compressed, size_t comp_len,
                         uint8_t *decompressed, size_t decomp_len)
{
    if (!compressed || !decompressed || comp_len < 4)
        return -1;

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < comp_len && out_pos < decomp_len)
    {
        uint8_t byte = compressed[in_pos++];

        // Check for RLE marker (ED ED)
        if (byte == 0xED && in_pos < comp_len && compressed[in_pos] == 0xED)
        {
            in_pos++; // Skip second ED

            if (in_pos + 1 >= comp_len)
                break;

            uint8_t repeat_count = compressed[in_pos++];
            uint8_t repeat_byte = compressed[in_pos++];

            // Check for end marker (00 ED ED 00)
            if (repeat_count == 0x00 && repeat_byte == 0x00)
            {
                // End of block reached
                break;
            }

            // Expand the repeated byte
            if (out_pos + repeat_count > decomp_len)
            {
                // Would overflow, just fill what we can
                repeat_count = decomp_len - out_pos;
            }

            memset(decompressed + out_pos, repeat_byte, repeat_count);
            out_pos += repeat_count;
        }
        else
        {
            // Regular byte, just copy it
            decompressed[out_pos++] = byte;
        }
    }

    return out_pos;
}

/**
 * Determine Z80 snapshot version from file
 * Version 1: PC is non-zero (program counter at 6-7)
 * Version 2/3: PC is zero (signal for extended format), followed by extra header
 */
int z80_snapshot_get_version(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
        return -1;

    uint8_t header[Z80_HEADER_SIZE];
    size_t bytes_read = fread(header, 1, Z80_HEADER_SIZE, file);
    fclose(file);

    if (bytes_read < Z80_HEADER_SIZE)
        return -1;

    // Read PC from bytes 6-7 (little-endian)
    uint16_t pc = header[6] | (header[7] << 8);

    if (pc != 0)
    {
        // Version 1: PC is non-zero
        return Z80_VERSION_1;
    }

    // PC is zero, could be V2 or V3. Check if extra header exists.
    // Try to read extra header length at byte 30-31
    uint8_t extra_check[2];
    if (fseek(file, 30, SEEK_SET) == 0 && fread(extra_check, 1, 2, file) == 2)
    {
        uint16_t extra_len = extra_check[0] | (extra_check[1] << 8);

        // V2 has extra_len == 23, V3 has 54 or 55
        if (extra_len == 23)
            return Z80_VERSION_2;
        else if (extra_len == 54 || extra_len == 55)
            return Z80_VERSION_3;
    }

    // Ambiguous, default to V1
    return Z80_VERSION_1;
}

/**
 * Load and restore V1 format snapshot (48K only)
 */
static int load_v1_snapshot(FILE *file, z80_emulator_t *cpu, uint8_t *memory)
{
    z80_v1_header_t header;

    // Read header
    uint8_t header_bytes[Z80_HEADER_SIZE];
    if (fread(header_bytes, 1, Z80_HEADER_SIZE, file) != Z80_HEADER_SIZE)
    {
        fprintf(stderr, "Error: Failed to read Z80 V1 header\n");
        return -1;
    }

    // Parse header
    header.a = header_bytes[0];
    header.f = header_bytes[1];
    header.bc = header_bytes[2] | (header_bytes[3] << 8);
    header.hl = header_bytes[4] | (header_bytes[5] << 8);
    header.pc = header_bytes[6] | (header_bytes[7] << 8);
    header.sp = header_bytes[8] | (header_bytes[9] << 8);
    header.i = header_bytes[10];
    header.r = header_bytes[11];
    uint8_t flags_byte = header_bytes[12];
    header.de = header_bytes[13] | (header_bytes[14] << 8);
    header.bc_ = header_bytes[15] | (header_bytes[16] << 8);
    header.de_ = header_bytes[17] | (header_bytes[18] << 8);
    header.hl_ = header_bytes[19] | (header_bytes[20] << 8);
    header.a_ = header_bytes[21];
    header.f_ = header_bytes[22];
    header.iy = header_bytes[23] | (header_bytes[24] << 8);
    header.ix = header_bytes[25] | (header_bytes[26] << 8);
    header.iff1 = header_bytes[27];
    header.iff2 = header_bytes[28];
    header.im = header_bytes[29] & 0x03;

    // Restore CPU state
    cpu->regs.a = header.a;
    cpu->regs.b = header.bc >> 8;
    cpu->regs.c = header.bc & 0xFF;
    cpu->regs.d = header.de >> 8;
    cpu->regs.e = header.de & 0xFF;
    cpu->regs.h = header.hl >> 8;
    cpu->regs.l = header.hl & 0xFF;
    cpu->regs.pc = header.pc;
    cpu->regs.sp = header.sp;
    cpu->regs.i = header.i;
    cpu->regs.r = header.r & 0x7F; // Bit 7 not significant
    if (flags_byte & 0x01)
        cpu->regs.r |= 0x80; // Restore bit 7 from flags

    // Restore alternate registers
    cpu->regs.a_ = header.a_;
    cpu->regs.b_ = header.bc_ >> 8;
    cpu->regs.c_ = header.bc_ & 0xFF;
    cpu->regs.d_ = header.de_ >> 8;
    cpu->regs.e_ = header.de_ & 0xFF;
    cpu->regs.h_ = header.hl_ >> 8;
    cpu->regs.l_ = header.hl_ & 0xFF;
    cpu->regs.f_ = header.f_;

    // Restore special registers
    cpu->regs.ix = header.ix;
    cpu->regs.iy = header.iy;
    cpu->regs.im = header.im;
    cpu->regs.iff1 = header.iff1 ? 1 : 0;
    cpu->regs.iff2 = header.iff2 ? 1 : 0;

    // Restore F register from flags_byte
    cpu->regs.cf = (header.f >> 0) & 1;
    cpu->regs.nf = (header.f >> 1) & 1;
    cpu->regs.pf = (header.f >> 2) & 1;
    cpu->regs.hf = (header.f >> 4) & 1;
    cpu->regs.zf = (header.f >> 6) & 1;
    cpu->regs.sf = (header.f >> 7) & 1;
    // Note: yf and xf bits are undocumented, not restored

    // Check if memory is compressed (flags_byte bit 5)
    bool compressed = (flags_byte >> 5) & 1;

    // Read memory (48KB)
    uint8_t memory_buffer[Z80_V1_MEMORY_SIZE];

    if (compressed)
    {
        // Read compressed data until end marker
        uint8_t compressed_data[Z80_V1_MEMORY_SIZE * 2]; // Enough for worst case
        size_t comp_pos = 0;

        while (comp_pos < sizeof(compressed_data) - 4)
        {
            int byte = fgetc(file);
            if (byte == EOF)
                break;

            compressed_data[comp_pos++] = (uint8_t)byte;

            // Check for end marker (00 ED ED 00)
            if (comp_pos >= 4 &&
                compressed_data[comp_pos - 4] == 0x00 &&
                compressed_data[comp_pos - 3] == 0xED &&
                compressed_data[comp_pos - 2] == 0xED &&
                compressed_data[comp_pos - 1] == 0x00)
            {
                break;
            }
        }

        if (z80_decompress_block(compressed_data, comp_pos, memory_buffer, Z80_V1_MEMORY_SIZE) < 0)
        {
            fprintf(stderr, "Error: Failed to decompress memory block\n");
            return -1;
        }
    }
    else
    {
        // Read uncompressed data
        if (fread(memory_buffer, 1, Z80_V1_MEMORY_SIZE, file) != Z80_V1_MEMORY_SIZE)
        {
            fprintf(stderr, "Error: Failed to read uncompressed memory\n");
            return -1;
        }
    }

    // Copy memory: 0x0000-0x3FFF (ROM), 0x4000-0xFFFF (RAM)
    // In 48K mode, all 48KB of RAM is loaded
    memcpy(memory + 0x4000, memory_buffer, Z80_V1_MEMORY_SIZE);

    printf("Loaded Z80 V1 snapshot: PC=0x%04X SP=0x%04X A=0x%02X\n",
           cpu->regs.pc, cpu->regs.sp, cpu->regs.a);

    return 0;
}

/**
 * Load and restore V2/V3 format snapshot
 */
static int load_v23_snapshot(FILE *file, z80_emulator_t *cpu, uint8_t *memory, int version)
{
    z80_v1_header_t header;
    z80_v2_header_t v2_header;

    // Read V1 header (first 30 bytes)
    uint8_t header_bytes[Z80_HEADER_SIZE];
    if (fread(header_bytes, 1, Z80_HEADER_SIZE, file) != Z80_HEADER_SIZE)
    {
        fprintf(stderr, "Error: Failed to read Z80 V2/3 base header\n");
        return -1;
    }

    // Parse V1 header fields (same as V1 but PC will be overridden)
    header.a = header_bytes[0];
    header.f = header_bytes[1];
    header.bc = header_bytes[2] | (header_bytes[3] << 8);
    header.hl = header_bytes[4] | (header_bytes[5] << 8);
    header.sp = header_bytes[8] | (header_bytes[9] << 8);
    header.i = header_bytes[10];
    header.r = header_bytes[11];
    uint8_t flags_byte = header_bytes[12];
    header.de = header_bytes[13] | (header_bytes[14] << 8);
    header.bc_ = header_bytes[15] | (header_bytes[16] << 8);
    header.de_ = header_bytes[17] | (header_bytes[18] << 8);
    header.hl_ = header_bytes[19] | (header_bytes[20] << 8);
    header.a_ = header_bytes[21];
    header.f_ = header_bytes[22];
    header.iy = header_bytes[23] | (header_bytes[24] << 8);
    header.ix = header_bytes[25] | (header_bytes[26] << 8);
    header.iff1 = header_bytes[27];
    header.iff2 = header_bytes[28];
    header.im = header_bytes[29] & 0x03;

    // Read extra header
    uint8_t extra_header[2];
    if (fread(extra_header, 1, 2, file) != 2)
    {
        fprintf(stderr, "Error: Failed to read extended header length\n");
        return -1;
    }

    uint16_t extra_len = extra_header[0] | (extra_header[1] << 8);
    uint8_t *extra_data = malloc(extra_len);
    if (!extra_data)
    {
        fprintf(stderr, "Error: Failed to allocate extended header buffer\n");
        return -1;
    }

    if (fread(extra_data, 1, extra_len, file) != extra_len)
    {
        fprintf(stderr, "Error: Failed to read extended header\n");
        free(extra_data);
        return -1;
    }

    // Parse extended header fields (at minimum, bytes 0-5)
    v2_header.pc = extra_data[0] | (extra_data[1] << 8);
    v2_header.hardware_mode = (extra_len > 2) ? extra_data[2] : 0;
    v2_header.paging_register = (extra_len > 3) ? extra_data[3] : 0;
    v2_header.interface_rom = (extra_len > 4) ? extra_data[4] : 0;
    v2_header.emulation_flags = (extra_len > 5) ? extra_data[5] : 0;

    free(extra_data);

    // Restore CPU state from V1 header
    cpu->regs.a = header.a;
    cpu->regs.b = header.bc >> 8;
    cpu->regs.c = header.bc & 0xFF;
    cpu->regs.d = header.de >> 8;
    cpu->regs.e = header.de & 0xFF;
    cpu->regs.h = header.hl >> 8;
    cpu->regs.l = header.hl & 0xFF;
    cpu->regs.pc = v2_header.pc; // Use PC from extended header
    cpu->regs.sp = header.sp;
    cpu->regs.i = header.i;
    cpu->regs.r = header.r & 0x7F;
    if (flags_byte & 0x01)
        cpu->regs.r |= 0x80;

    // Restore alternate registers
    cpu->regs.a_ = header.a_;
    cpu->regs.b_ = header.bc_ >> 8;
    cpu->regs.c_ = header.bc_ & 0xFF;
    cpu->regs.d_ = header.de_ >> 8;
    cpu->regs.e_ = header.de_ & 0xFF;
    cpu->regs.h_ = header.hl_ >> 8;
    cpu->regs.l_ = header.hl_ & 0xFF;
    cpu->regs.f_ = header.f_;

    // Restore special registers
    cpu->regs.ix = header.ix;
    cpu->regs.iy = header.iy;
    cpu->regs.im = header.im;
    cpu->regs.iff1 = header.iff1 ? 1 : 0;
    cpu->regs.iff2 = header.iff2 ? 1 : 0;

    // Restore F register
    cpu->regs.cf = (header.f >> 0) & 1;
    cpu->regs.nf = (header.f >> 1) & 1;
    cpu->regs.pf = (header.f >> 2) & 1;
    cpu->regs.hf = (header.f >> 4) & 1;
    cpu->regs.zf = (header.f >> 6) & 1;
    cpu->regs.sf = (header.f >> 7) & 1;

    // Read memory blocks for 48K mode (pages 4, 5, 8)
    // For now, support only 48K mode
    while (!feof(file))
    {
        uint8_t block_header[Z80_MEMORY_BLOCK_HEADER_SIZE];
        if (fread(block_header, 1, Z80_MEMORY_BLOCK_HEADER_SIZE, file) != Z80_MEMORY_BLOCK_HEADER_SIZE)
        {
            break; // End of file
        }

        uint16_t comp_length = block_header[0] | (block_header[1] << 8);
        uint8_t page_number = block_header[2];

        // Read compressed data for this block
        uint8_t *block_data = malloc(comp_length);
        if (!block_data)
        {
            fprintf(stderr, "Error: Failed to allocate memory block buffer\n");
            return -1;
        }

        if (fread(block_data, 1, comp_length, file) != comp_length)
        {
            fprintf(stderr, "Error: Failed to read memory block\n");
            free(block_data);
            return -1;
        }

        // Decompress block
        uint8_t decompressed[16384]; // 16KB max for one page
        int decomp_len = comp_length;
        if (comp_length != 0xFFFF)
        {
            decomp_len = z80_decompress_block(block_data, comp_length, decompressed, sizeof(decompressed));
            if (decomp_len < 0)
            {
                fprintf(stderr, "Error: Failed to decompress memory block page %d\n", page_number);
                free(block_data);
                return -1;
            }
        }
        else
        {
            // Uncompressed: length 0xFFFF means 16384 bytes raw data
            memcpy(decompressed, block_data, 16384);
            decomp_len = 16384;
        }

        // Map page to memory address for 48K mode
        uint16_t target_addr = 0;
        switch (page_number)
        {
        case Z80_PAGE_48K_ROM:
            target_addr = 0x0000; // ROM
            break;
        case Z80_PAGE_48K_VRAM:
            target_addr = 0x4000; // Video RAM
            break;
        case Z80_PAGE_48K_RAM4:
            target_addr = 0x8000; // RAM page 4
            break;
        case Z80_PAGE_48K_RAM5:
            target_addr = 0xC000; // RAM page 5
            break;
        default:
            // Skip unknown pages
            fprintf(stderr, "Warning: Skipping unknown memory page %d\n", page_number);
            free(block_data);
            continue;
        }

        // Copy decompressed data to memory
        memcpy(memory + target_addr, decompressed, decomp_len);

        free(block_data);
    }

    printf("Loaded Z80 V%d snapshot: PC=0x%04X SP=0x%04X A=0x%02X\n",
           version, cpu->regs.pc, cpu->regs.sp, cpu->regs.a);

    return 0;
}

/**
 * Load and restore Z80 snapshot
 */
int z80_snapshot_load(const char *filename, z80_emulator_t *cpu, uint8_t *memory)
{
    if (!filename || !cpu || !memory)
        return -1;

    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(stderr, "Error: Cannot open Z80 snapshot file '%s'\n", filename);
        return -1;
    }

    // Determine version
    int version = z80_snapshot_get_version(filename);
    if (version < 0)
    {
        fprintf(stderr, "Error: Failed to determine Z80 file version\n");
        fclose(file);
        return -1;
    }

    // Reset file pointer
    rewind(file);

    int result = -1;
    if (version == Z80_VERSION_1)
    {
        result = load_v1_snapshot(file, cpu, memory);
    }
    else if (version == Z80_VERSION_2)
    {
        result = load_v23_snapshot(file, cpu, memory, Z80_VERSION_2);
    }
    else if (version == Z80_VERSION_3)
    {
        result = load_v23_snapshot(file, cpu, memory, Z80_VERSION_3);
    }
    else
    {
        fprintf(stderr, "Error: Unsupported Z80 file version\n");
    }

    fclose(file);
    return result;
}

/**
 * Spettrum - Z80 Emulator Main Entry Point
 *
 * Initializes emulator components (Z80 CPU, ULA graphics), processes command-line
 * arguments, and starts the main emulation loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

// Include Z80 implementation directly
#include "z80.c"
#include "ula.h"

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

// Emulator state
typedef struct
{
    z80_emulator_t *cpu;
    ula_t *display;
    uint8_t memory[SPETTRUM_TOTAL_MEMORY];
    volatile int running;
    FILE *disasm_file;        // File for disassembly output
    volatile int dump_memory; // Flag to trigger memory dump
    int dump_count;           // Counter for dump filenames
} spettrum_emulator_t;

// Global emulator reference for signal handling
static spettrum_emulator_t *g_emulator = NULL;

/**
 * ULA render thread - continuously renders display
 */
static void *ula_render_thread(void *arg)
{
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)arg;

    // Render loop - runs while emulator is running
    while (emulator->running)
    {
        // Convert VRAM to character matrix
        convert_vram_to_matrix(&emulator->memory[SPETTRUM_VRAM_START]);

        // Render matrix to terminal
        ula_render_to_terminal();
    }

    return NULL;
}

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig)
{
    (void)sig; // Unused
    if (g_emulator)
        g_emulator->running = 0;
}

/**
 * Decode CB prefix instruction
 */
static const char *decode_cb_instruction(uint8_t opcode)
{
    static char buf[32];

    uint8_t operation = (opcode >> 6) & 0x03;
    uint8_t bit_num = (opcode >> 3) & 0x07;
    uint8_t reg_idx = opcode & 0x07;

    const char *regs[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    const char *ops[] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};

    if (operation == 0)
    {
        // Shift/rotate operations: 00xxxyyy
        snprintf(buf, sizeof(buf), "%s %s", ops[bit_num], regs[reg_idx]);
    }
    else if (operation == 1)
    {
        // BIT operations: 01bbbyyy
        snprintf(buf, sizeof(buf), "BIT %d, %s", bit_num, regs[reg_idx]);
    }
    else if (operation == 2)
    {
        // RES operations: 10bbbyyy
        snprintf(buf, sizeof(buf), "RES %d, %s", bit_num, regs[reg_idx]);
    }
    else
    {
        // SET operations: 11bbbyyy
        snprintf(buf, sizeof(buf), "SET %d, %s", bit_num, regs[reg_idx]);
    }

    return buf;
}

/**
 * Decode ED prefix instruction
 */
static const char *decode_ed_instruction(uint8_t opcode)
{
    static char buf[32];

    switch (opcode)
    {
    case 0x40:
        return "IN B, (C)";
    case 0x41:
        return "OUT (C), B";
    case 0x42:
        return "SBC HL, BC";
    case 0x43:
        return "LD (nn), BC";
    case 0x44:
        return "NEG";
    case 0x45:
        return "RETN";
    case 0x46:
        return "IM 0";
    case 0x47:
        return "LD I, A";
    case 0x48:
        return "IN C, (C)";
    case 0x49:
        return "OUT (C), C";
    case 0x4A:
        return "ADC HL, BC";
    case 0x4B:
        return "LD BC, (nn)";
    case 0x4C:
        return "NEG";
    case 0x4D:
        return "RETI";
    case 0x4E:
        return "IM 0";
    case 0x4F:
        return "LD R, A";
    case 0x50:
        return "IN D, (C)";
    case 0x51:
        return "OUT (C), D";
    case 0x52:
        return "SBC HL, DE";
    case 0x53:
        return "LD (nn), DE";
    case 0x54:
        return "NEG";
    case 0x55:
        return "RETN";
    case 0x56:
        return "IM 1";
    case 0x57:
        return "LD A, I";
    case 0x58:
        return "IN E, (C)";
    case 0x59:
        return "OUT (C), E";
    case 0x5A:
        return "ADC HL, DE";
    case 0x5B:
        return "LD DE, (nn)";
    case 0x5C:
        return "NEG";
    case 0x5D:
        return "RETN";
    case 0x5E:
        return "IM 2";
    case 0x5F:
        return "LD A, R";
    case 0x60:
        return "IN H, (C)";
    case 0x61:
        return "OUT (C), H";
    case 0x62:
        return "SBC HL, HL";
    case 0x63:
        return "LD (nn), HL";
    case 0x64:
        return "NEG";
    case 0x65:
        return "RETN";
    case 0x66:
        return "IM 0";
    case 0x67:
        return "RRD";
    case 0x68:
        return "IN L, (C)";
    case 0x69:
        return "OUT (C), L";
    case 0x6A:
        return "ADC HL, HL";
    case 0x6B:
        return "LD HL, (nn)";
    case 0x6C:
        return "NEG";
    case 0x6D:
        return "RETN";
    case 0x6E:
        return "IM 0";
    case 0x6F:
        return "RLD";
    case 0x70:
        return "IN (C)";
    case 0x71:
        return "OUT (C), 0";
    case 0x72:
        return "SBC HL, SP";
    case 0x73:
        return "LD (nn), SP";
    case 0x74:
        return "NEG";
    case 0x75:
        return "RETN";
    case 0x76:
        return "IM 1";
    case 0x78:
        return "IN A, (C)";
    case 0x79:
        return "OUT (C), A";
    case 0x7A:
        return "ADC HL, SP";
    case 0x7B:
        return "LD SP, (nn)";
    case 0x7C:
        return "NEG";
    case 0x7D:
        return "RETN";
    case 0x7E:
        return "IM 2";
    case 0xA0:
        return "LDI";
    case 0xA1:
        return "CPI";
    case 0xA2:
        return "INI";
    case 0xA3:
        return "OUTI";
    case 0xA8:
        return "LDD";
    case 0xA9:
        return "CPD";
    case 0xAA:
        return "IND";
    case 0xAB:
        return "OUTD";
    case 0xB0:
        return "LDIR";
    case 0xB1:
        return "CPIR";
    case 0xB2:
        return "INIR";
    case 0xB3:
        return "OTIR";
    case 0xB8:
        return "LDDR";
    case 0xB9:
        return "CPDR";
    case 0xBA:
        return "INDR";
    case 0xBB:
        return "OTDR";
    default:
        snprintf(buf, sizeof(buf), "ED %02X (unknown)", opcode);
        return buf;
    }
}

/**
 * Log instruction disassembly to file with register state and actual operand values
 */
static void log_instruction_disassembly(spettrum_emulator_t *emulator, uint16_t pc, uint8_t opcode)
{
    if (!emulator || !emulator->disasm_file)
        return;

    z80_registers_t regs = emulator->cpu->regs;
    char instr_buf[32] = "???";

    // Read operand bytes for immediate values and addresses
    uint8_t operand = 0;
    uint16_t addr = 0;
    if (pc + 1 < SPETTRUM_TOTAL_MEMORY)
        operand = emulator->memory[pc + 1];
    if (pc + 2 < SPETTRUM_TOTAL_MEMORY)
        addr = emulator->memory[pc + 1] | (emulator->memory[pc + 2] << 8);

    // Decode instruction based on opcode with actual values
    switch (opcode)
    {
    // Root Instructions
    case 0x00:
        snprintf(instr_buf, sizeof(instr_buf), "NOP");
        break;
    case 0x01:
        snprintf(instr_buf, sizeof(instr_buf), "LD BC, %04X", addr);
        break;
    case 0x02:
        snprintf(instr_buf, sizeof(instr_buf), "LD (BC), A");
        break;
    case 0x03:
        snprintf(instr_buf, sizeof(instr_buf), "INC BC");
        break;
    case 0x04:
        snprintf(instr_buf, sizeof(instr_buf), "INC B");
        break;
    case 0x05:
        snprintf(instr_buf, sizeof(instr_buf), "DEC B");
        break;
    case 0x06:
        snprintf(instr_buf, sizeof(instr_buf), "LD B, %02X", operand);
        break;
    case 0x07:
        snprintf(instr_buf, sizeof(instr_buf), "RLCA");
        break;
    case 0x08:
        snprintf(instr_buf, sizeof(instr_buf), "EX AF, AF'");
        break;
    case 0x09:
        snprintf(instr_buf, sizeof(instr_buf), "ADD HL, BC");
        break;
    case 0x0A:
        snprintf(instr_buf, sizeof(instr_buf), "LD A, (BC)");
        break;
    case 0x0B:
        snprintf(instr_buf, sizeof(instr_buf), "DEC BC");
        break;
    case 0x0C:
        snprintf(instr_buf, sizeof(instr_buf), "INC C");
        break;
    case 0x0D:
        snprintf(instr_buf, sizeof(instr_buf), "DEC C");
        break;
    case 0x0E:
        snprintf(instr_buf, sizeof(instr_buf), "LD C, %02X", operand);
        break;
    case 0x0F:
        snprintf(instr_buf, sizeof(instr_buf), "RRCA");
        break;
    case 0x10:
        snprintf(instr_buf, sizeof(instr_buf), "DJNZ %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x11:
        snprintf(instr_buf, sizeof(instr_buf), "LD DE, %04X", addr);
        break;
    case 0x12:
        snprintf(instr_buf, sizeof(instr_buf), "LD (DE), A");
        break;
    case 0x13:
        snprintf(instr_buf, sizeof(instr_buf), "INC DE");
        break;
    case 0x14:
        snprintf(instr_buf, sizeof(instr_buf), "INC D");
        break;
    case 0x15:
        snprintf(instr_buf, sizeof(instr_buf), "DEC D");
        break;
    case 0x16:
        snprintf(instr_buf, sizeof(instr_buf), "LD D, %02X", operand);
        break;
    case 0x17:
        snprintf(instr_buf, sizeof(instr_buf), "RLA");
        break;
    case 0x18:
        snprintf(instr_buf, sizeof(instr_buf), "JR %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x19:
        snprintf(instr_buf, sizeof(instr_buf), "ADD HL, DE");
        break;
    case 0x1A:
        snprintf(instr_buf, sizeof(instr_buf), "LD A, (DE)");
        break;
    case 0x1B:
        snprintf(instr_buf, sizeof(instr_buf), "DEC DE");
        break;
    case 0x1C:
        snprintf(instr_buf, sizeof(instr_buf), "INC E");
        break;
    case 0x1D:
        snprintf(instr_buf, sizeof(instr_buf), "DEC E");
        break;
    case 0x1E:
        snprintf(instr_buf, sizeof(instr_buf), "LD E, %02X", operand);
        break;
    case 0x1F:
        snprintf(instr_buf, sizeof(instr_buf), "RRA");
        break;
    case 0x20:
        snprintf(instr_buf, sizeof(instr_buf), "JR NZ, %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x21:
        snprintf(instr_buf, sizeof(instr_buf), "LD HL, %04X", addr);
        break;
    case 0x22:
        snprintf(instr_buf, sizeof(instr_buf), "LD (%04X), HL", addr);
        break;
    case 0x23:
        snprintf(instr_buf, sizeof(instr_buf), "INC HL");
        break;
    case 0x24:
        snprintf(instr_buf, sizeof(instr_buf), "INC H");
        break;
    case 0x25:
        snprintf(instr_buf, sizeof(instr_buf), "DEC H");
        break;
    case 0x26:
        snprintf(instr_buf, sizeof(instr_buf), "LD H, %02X", operand);
        break;
    case 0x27:
        snprintf(instr_buf, sizeof(instr_buf), "DAA");
        break;
    case 0x28:
        snprintf(instr_buf, sizeof(instr_buf), "JR Z, %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x29:
        snprintf(instr_buf, sizeof(instr_buf), "ADD HL, HL");
        break;
    case 0x2A:
        snprintf(instr_buf, sizeof(instr_buf), "LD HL, (%04X)", addr);
        break;
    case 0x2B:
        snprintf(instr_buf, sizeof(instr_buf), "DEC HL");
        break;
    case 0x2C:
        snprintf(instr_buf, sizeof(instr_buf), "INC L");
        break;
    case 0x2D:
        snprintf(instr_buf, sizeof(instr_buf), "DEC L");
        break;
    case 0x2E:
        snprintf(instr_buf, sizeof(instr_buf), "LD L, %02X", operand);
        break;
    case 0x2F:
        snprintf(instr_buf, sizeof(instr_buf), "CPL");
        break;
    case 0x30:
        snprintf(instr_buf, sizeof(instr_buf), "JR NC, %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x31:
        snprintf(instr_buf, sizeof(instr_buf), "LD SP, %04X", addr);
        break;
    case 0x32:
        snprintf(instr_buf, sizeof(instr_buf), "LD (%04X), A", addr);
        break;
    case 0x33:
        snprintf(instr_buf, sizeof(instr_buf), "INC SP");
        break;
    case 0x34:
        snprintf(instr_buf, sizeof(instr_buf), "INC (HL)");
        break;
    case 0x35:
        snprintf(instr_buf, sizeof(instr_buf), "DEC (HL)");
        break;
    case 0x36:
        snprintf(instr_buf, sizeof(instr_buf), "LD (HL), %02X", operand);
        break;
    case 0x37:
        snprintf(instr_buf, sizeof(instr_buf), "SCF");
        break;
    case 0x38:
        snprintf(instr_buf, sizeof(instr_buf), "JR C, %04X", pc + 2 + (int8_t)operand);
        break;
    case 0x39:
        snprintf(instr_buf, sizeof(instr_buf), "ADD HL, SP");
        break;
    case 0x3A:
        snprintf(instr_buf, sizeof(instr_buf), "LD A, (%04X)", addr);
        break;
    case 0x3B:
        snprintf(instr_buf, sizeof(instr_buf), "DEC SP");
        break;
    case 0x3C:
        snprintf(instr_buf, sizeof(instr_buf), "INC A");
        break;
    case 0x3D:
        snprintf(instr_buf, sizeof(instr_buf), "DEC A");
        break;
    case 0x3E:
        snprintf(instr_buf, sizeof(instr_buf), "LD A, %02X", operand);
        break;
    case 0x3F:
        snprintf(instr_buf, sizeof(instr_buf), "CCF");
        break;

    // LD r, r' instructions (0x40-0x7F)
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F:
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6B:
    case 0x6C:
    case 0x6D:
    case 0x6E:
    case 0x6F:
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
    {
        const char *dests[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
        const char *srcs[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
        uint8_t dest = (opcode >> 3) & 0x07;
        uint8_t src = opcode & 0x07;

        // Special case: HALT
        if (opcode == 0x76)
        {
            snprintf(instr_buf, sizeof(instr_buf), "HALT");
        }
        else
        {
            snprintf(instr_buf, sizeof(instr_buf), "LD %s, %s", dests[dest], srcs[src]);
        }
        break;
    }

    // ALU operations (0x80-0xBF)
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
    case 0x8C:
    case 0x8D:
    case 0x8E:
    case 0x8F:
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0x9D:
    case 0x9E:
    case 0x9F:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
    case 0xA6:
    case 0xA7:
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB7:
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBE:
    case 0xBF:
    {
        const char *ops[] = {"ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"};
        const char *regs[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
        uint8_t op = (opcode >> 3) & 0x07;
        uint8_t reg = opcode & 0x07;
        snprintf(instr_buf, sizeof(instr_buf), "%s A, %s", ops[op], regs[reg]);
        break;
    }

    // Control flow and jumps (0xC0-0xFF)
    case 0xC0:
        snprintf(instr_buf, sizeof(instr_buf), "RET NZ");
        break;
    case 0xC1:
        snprintf(instr_buf, sizeof(instr_buf), "POP BC");
        break;
    case 0xC2:
        snprintf(instr_buf, sizeof(instr_buf), "JP NZ, %04X", addr);
        break;
    case 0xC3:
        snprintf(instr_buf, sizeof(instr_buf), "JP %04X", addr);
        break;
    case 0xC4:
        snprintf(instr_buf, sizeof(instr_buf), "CALL NZ, %04X", addr);
        break;
    case 0xC5:
        snprintf(instr_buf, sizeof(instr_buf), "PUSH BC");
        break;
    case 0xC6:
        snprintf(instr_buf, sizeof(instr_buf), "ADD A, %02X", operand);
        break;
    case 0xC7:
        snprintf(instr_buf, sizeof(instr_buf), "RST 00");
        break;
    case 0xC8:
        snprintf(instr_buf, sizeof(instr_buf), "RET Z");
        break;
    case 0xC9:
        snprintf(instr_buf, sizeof(instr_buf), "RET");
        break;
    case 0xCA:
        snprintf(instr_buf, sizeof(instr_buf), "JP Z, %04X", addr);
        break;
    case 0xCB:
    {
        uint8_t cb_opcode = operand;
        snprintf(instr_buf, sizeof(instr_buf), "CB %s", decode_cb_instruction(cb_opcode));
        break;
    }
    case 0xCC:
        snprintf(instr_buf, sizeof(instr_buf), "CALL Z, %04X", addr);
        break;
    case 0xCD:
        snprintf(instr_buf, sizeof(instr_buf), "CALL %04X", addr);
        break;
    case 0xCE:
        snprintf(instr_buf, sizeof(instr_buf), "ADC A, %02X", operand);
        break;
    case 0xCF:
        snprintf(instr_buf, sizeof(instr_buf), "RST 08");
        break;
    case 0xD0:
        snprintf(instr_buf, sizeof(instr_buf), "RET NC");
        break;
    case 0xD1:
        snprintf(instr_buf, sizeof(instr_buf), "POP DE");
        break;
    case 0xD2:
        snprintf(instr_buf, sizeof(instr_buf), "JP NC, %04X", addr);
        break;
    case 0xD3:
        snprintf(instr_buf, sizeof(instr_buf), "OUT %02X, A", operand);
        break;
    case 0xD4:
        snprintf(instr_buf, sizeof(instr_buf), "CALL NC, %04X", addr);
        break;
    case 0xD5:
        snprintf(instr_buf, sizeof(instr_buf), "PUSH DE");
        break;
    case 0xD6:
        snprintf(instr_buf, sizeof(instr_buf), "SUB %02X", operand);
        break;
    case 0xD7:
        snprintf(instr_buf, sizeof(instr_buf), "RST 10");
        break;
    case 0xD8:
        snprintf(instr_buf, sizeof(instr_buf), "RET C");
        break;
    case 0xD9:
        snprintf(instr_buf, sizeof(instr_buf), "EXX");
        break;
    case 0xDA:
        snprintf(instr_buf, sizeof(instr_buf), "JP C, %04X", addr);
        break;
    case 0xDB:
        snprintf(instr_buf, sizeof(instr_buf), "IN A, %02X", operand);
        break;
    case 0xDC:
        snprintf(instr_buf, sizeof(instr_buf), "CALL C, %04X", addr);
        break;
    case 0xDD:
        snprintf(instr_buf, sizeof(instr_buf), "DD prefix (IX)");
        break;
    case 0xDE:
        snprintf(instr_buf, sizeof(instr_buf), "SBC A, %02X", operand);
        break;
    case 0xDF:
        snprintf(instr_buf, sizeof(instr_buf), "RST 18");
        break;
    case 0xE0:
        snprintf(instr_buf, sizeof(instr_buf), "RET PO");
        break;
    case 0xE1:
        snprintf(instr_buf, sizeof(instr_buf), "POP HL");
        break;
    case 0xE2:
        snprintf(instr_buf, sizeof(instr_buf), "JP PO, %04X", addr);
        break;
    case 0xE3:
        snprintf(instr_buf, sizeof(instr_buf), "EX (SP), HL");
        break;
    case 0xE4:
        snprintf(instr_buf, sizeof(instr_buf), "CALL PO, %04X", addr);
        break;
    case 0xE5:
        snprintf(instr_buf, sizeof(instr_buf), "PUSH HL");
        break;
    case 0xE6:
        snprintf(instr_buf, sizeof(instr_buf), "AND %02X", operand);
        break;
    case 0xE7:
        snprintf(instr_buf, sizeof(instr_buf), "RST 20");
        break;
    case 0xE8:
        snprintf(instr_buf, sizeof(instr_buf), "RET PE");
        break;
    case 0xE9:
        snprintf(instr_buf, sizeof(instr_buf), "JP (HL)");
        break;
    case 0xEA:
        snprintf(instr_buf, sizeof(instr_buf), "JP PE, %04X", addr);
        break;
    case 0xEB:
        snprintf(instr_buf, sizeof(instr_buf), "EX DE, HL");
        break;
    case 0xEC:
        snprintf(instr_buf, sizeof(instr_buf), "CALL PE, %04X", addr);
        break;
    case 0xED:
    {
        uint8_t ed_opcode = operand;
        const char *ed_instr = decode_ed_instruction(ed_opcode);
        snprintf(instr_buf, sizeof(instr_buf), "ED %s", ed_instr);
        break;
    }
    case 0xEE:
        snprintf(instr_buf, sizeof(instr_buf), "XOR %02X", operand);
        break;
    case 0xEF:
        snprintf(instr_buf, sizeof(instr_buf), "RST 28");
        break;
    case 0xF0:
        snprintf(instr_buf, sizeof(instr_buf), "RET P");
        break;
    case 0xF1:
        snprintf(instr_buf, sizeof(instr_buf), "POP AF");
        break;
    case 0xF2:
        snprintf(instr_buf, sizeof(instr_buf), "JP P, %04X", addr);
        break;
    case 0xF3:
        snprintf(instr_buf, sizeof(instr_buf), "DI");
        break;
    case 0xF4:
        snprintf(instr_buf, sizeof(instr_buf), "CALL P, %04X", addr);
        break;
    case 0xF5:
        snprintf(instr_buf, sizeof(instr_buf), "PUSH AF");
        break;
    case 0xF6:
        snprintf(instr_buf, sizeof(instr_buf), "OR %02X", operand);
        break;
    case 0xF7:
        snprintf(instr_buf, sizeof(instr_buf), "RST 30");
        break;
    case 0xF8:
        snprintf(instr_buf, sizeof(instr_buf), "RET M");
        break;
    case 0xF9:
        snprintf(instr_buf, sizeof(instr_buf), "LD SP, HL");
        break;
    case 0xFA:
        snprintf(instr_buf, sizeof(instr_buf), "JP M, %04X", addr);
        break;
    case 0xFB:
        snprintf(instr_buf, sizeof(instr_buf), "EI");
        break;
    case 0xFC:
        snprintf(instr_buf, sizeof(instr_buf), "CALL M, %04X", addr);
        break;
    case 0xFD:
        snprintf(instr_buf, sizeof(instr_buf), "FD prefix (IY)");
        break;
    case 0xFE:
        snprintf(instr_buf, sizeof(instr_buf), "CP %02X", operand);
        break;
    case 0xFF:
        snprintf(instr_buf, sizeof(instr_buf), "RST 38");
        break;

        // All cases handled - should never reach default
    }

    // Format: PC: opcode instruction ; registers
    fprintf(emulator->disasm_file, "%04X: %02X %-28s ; A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X\n",
            pc, opcode, instr_buf,
            regs.a, regs.f,
            (regs.b << 8) | regs.c,
            (regs.d << 8) | regs.e,
            (regs.h << 8) | regs.l,
            regs.sp);
    fflush(emulator->disasm_file);
}

/**
 * Signal handler for graceful shutdown
 */
static void dump_memory_handler(int sig)
{
    (void)sig; // Unused
    if (g_emulator)
        g_emulator->dump_memory = 1;
}

/**
 * Dump memory to file
 */
static void dump_memory_to_file(spettrum_emulator_t *emulator)
{
    if (!emulator)
        return;

    // Generate filename with counter
    char filename[256];
    snprintf(filename, sizeof(filename), "memory_dump_%03d.bin", emulator->dump_count++);

    FILE *file = fopen(filename, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: Cannot open dump file '%s'\n", filename);
        return;
    }

    // Write entire 64KB memory
    size_t written = fwrite(emulator->memory, 1, SPETTRUM_TOTAL_MEMORY, file);
    fclose(file);

    fprintf(stderr, "Memory dumped to '%s' (%zu bytes)\n", filename, written);
}

/**
 * Print version information
 */
static void print_version(void)
{
    printf("Spettrum %d.%d.%d\n", SPETTRUM_VERSION_MAJOR, SPETTRUM_VERSION_MINOR, SPETTRUM_VERSION_PATCH);
    printf("Z80 Emulator for Sinclair Spectrum\n");
}

/**
 * Print help/usage information
 */
static void print_help(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -h, --help              Show this help message\n");
    printf("  -v, --version           Show version information\n");
    printf("  -r, --rom FILE          Load ROM from file\n");
    printf("  -d, --disk FILE         Load disk image from file\n");
    printf("  -i, --instructions NUM  Number of instructions to execute (0=unlimited, default=0)\n");
    printf("  -D, --disassemble FILE  Write disassembly to FILE\n");
    printf("\n");
}

/**
 * Memory read callback for Z80 CPU
 */
static uint8_t emulator_read_memory(void *user_data, uint16_t addr)
{
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)user_data;
    return emulator->memory[addr];
}

/**
 * Memory write callback for Z80 CPU
 */
static void emulator_write_memory(void *user_data, uint16_t addr, uint8_t value)
{
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)user_data;
    emulator->memory[addr] = value;
}

/**
 * I/O read callback for Z80 CPU
 */
static uint8_t emulator_read_io(void *user_data, uint8_t port)
{
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)user_data;

    // Port 0xFE is the ULA port (lower 3 bits for border color, bit 3 for mic/speaker)
    if (port == 0xFE)
    {
        // Return ULA state (border color in bits 0-2)
        return ula_get_border_color(emulator->display);
    }

    // Other ports return 0xFF (typical for unused ports)
    return 0xFF;
}

/**
 * I/O write callback for Z80 CPU
 */
static void emulator_write_io(void *user_data, uint8_t port, uint8_t value)
{
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)user_data;

    // Port 0xFE is the ULA port (lower 3 bits for border color, bit 3 for mic/speaker)
    if (port == 0xFE)
    {
        // Update ULA border color from bits 0-2
        uint8_t border_color = value & 0x07;
        ula_set_border_color(emulator->display, border_color);
    }
}

/**
 * Initialize emulator components
 */
static spettrum_emulator_t *emulator_init(void)
{
    spettrum_emulator_t *emulator = malloc(sizeof(spettrum_emulator_t));
    if (!emulator)
    {
        fprintf(stderr, "Error: Failed to allocate emulator memory\n");
        return NULL;
    }

    // Initialize memory
    memset(emulator->memory, 0, SPETTRUM_TOTAL_MEMORY);

    // Initialize Z80 CPU
    emulator->cpu = z80_init();
    if (!emulator->cpu)
    {
        fprintf(stderr, "Error: Failed to initialize Z80 CPU\n");
        free(emulator);
        return NULL;
    }

    // Set memory callbacks
    z80_set_memory_callbacks(emulator->cpu, emulator_read_memory, emulator_write_memory, emulator);

    // Set I/O callbacks
    z80_set_io_callbacks(emulator->cpu, emulator_read_io, emulator_write_io, emulator);

    // Initialize ULA display
    emulator->display = ula_init(SPECTRUM_WIDTH, SPECTRUM_HEIGHT);
    if (!emulator->display)
    {
        fprintf(stderr, "Error: Failed to initialize ULA display\n");
        z80_cleanup(emulator->cpu);
        free(emulator);
        return NULL;
    }

    emulator->running = 1;

    return emulator;
}

/**
 * Load ROM data from file
 */
static int emulator_load_rom(spettrum_emulator_t *emulator, const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        fprintf(stderr, "Error: Cannot open ROM file '%s'\n", filename);
        return -1;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Check if ROM fits in allocated ROM space
    if (file_size > SPETTRUM_ROM_SIZE)
    {
        fprintf(stderr, "Error: ROM file too large (%ld bytes, max %d bytes)\n", file_size, SPETTRUM_ROM_SIZE);
        fclose(file);
        return -1;
    }

    // Read ROM into memory at address 0
    size_t bytes_read = fread(emulator->memory, 1, file_size, file);
    if (bytes_read != (size_t)file_size)
    {
        fprintf(stderr, "Error: Failed to read ROM file\n");
        fclose(file);
        return -1;
    }

    fclose(file);
    printf("Loaded ROM: %ld bytes\n", file_size);
    return 0;
}

/**
 * Load disk image (placeholder)
 */
static int emulator_load_disk(spettrum_emulator_t *emulator __attribute__((unused)), const char *filename)
{
    fprintf(stderr, "Info: Disk loading not yet implemented (%s)\n", filename);
    // TODO: Implement disk image loading
    //  1. Open file
    //  2. Parse disk format
    //  3. Load programs/data into RAM
    return 0;
}

/**
 * Cleanup emulator components
 */
static void emulator_cleanup(spettrum_emulator_t *emulator)
{
    if (!emulator)
        return;

    if (emulator->cpu)
        z80_cleanup(emulator->cpu);

    if (emulator->display)
        ula_cleanup(emulator->display);

    free(emulator);
}

/**
 * Main emulation loop - runs Z80 CPU in main thread while ULA renders in parallel
 */
static int emulator_run(spettrum_emulator_t *emulator, uint64_t instructions_to_run)
{
    printf("Starting emulation...\n");
    printf("Display: %dx%d\n", emulator->display->width, emulator->display->height);
    printf("Memory: %d bytes\n", SPETTRUM_TOTAL_MEMORY);
    printf("CPU: PC=0x%04X, SP=0x%04X\n", emulator->cpu->regs.pc, emulator->cpu->regs.sp);
    printf("\nExecuting Z80 instructions...");
    if (instructions_to_run > 0)
        printf(" (limit: %llu instructions)", instructions_to_run);
    else
        printf(" (unlimited)");
    printf("\nPress Ctrl+C to stop.\n\n");
    fflush(stdout);

    // Initialize terminal for rendering
    ula_term_init();

    // Start ULA render thread
    pthread_t render_thread;
    if (pthread_create(&render_thread, NULL, ula_render_thread, emulator) != 0)
    {
        fprintf(stderr, "Error: Failed to create ULA render thread\n");
        return -1;
    }

    uint64_t instructions_executed = 0;

    // Run Z80 CPU in main thread
    while (emulator->running && (instructions_to_run == 0 || instructions_executed < instructions_to_run))
    {
        // Check if memory dump was requested
        if (emulator->dump_memory)
        {
            emulator->dump_memory = 0;
            dump_memory_to_file(emulator);
        }

        // Get current PC and opcode for disassembly
        uint16_t pc = emulator->cpu->regs.pc;
        uint8_t opcode = emulator->memory[pc];

        // Execute a single instruction and accumulate cycles
        int instruction_cycles = z80_execute_instruction(emulator->cpu);
        emulator->cpu->total_cycles += instruction_cycles;

        // Log disassembly if enabled
        if (emulator->disasm_file)
        {
            log_instruction_disassembly(emulator, pc, opcode);
        }

        instructions_executed++;
    }

    // Signal render thread to stop
    emulator->running = 0;

    // Wait for render thread to finish
    pthread_join(render_thread, NULL);

    // Cleanup terminal rendering
    ula_term_cleanup();

    // Close disassembly file if open
    if (emulator->disasm_file)
    {
        fclose(emulator->disasm_file);
        emulator->disasm_file = NULL;
    }

    printf("\nEmulation completed.\n");
    printf("Total instructions executed: %llu\n", instructions_executed);
    printf("Total cycles: %llu\n", emulator->cpu->total_cycles);
    printf("Final PC: 0x%04X\n", emulator->cpu->regs.pc);
    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char *argv[])
{
    const char *rom_file = NULL;
    const char *disk_file = NULL;
    const char *disasm_file = NULL;
    uint64_t instructions_to_run = 0; // Default: unlimited

    // Command-line options
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"rom", required_argument, 0, 'r'},
        {"disk", required_argument, 0, 'd'},
        {"instructions", required_argument, 0, 'i'},
        {"disassemble", required_argument, 0, 'D'},
        {0, 0, 0, 0}};

    // Parse command-line arguments
    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "hvr:d:i:D:", long_options, &option_index)) != -1)
    {
        switch (c)
        {
        case 'h':
            print_help(argv[0]);
            return EXIT_SUCCESS;
        case 'v':
            print_version();
            return EXIT_SUCCESS;
        case 'r':
            rom_file = optarg;
            break;
        case 'd':
            disk_file = optarg;
            break;
        case 'i':
            instructions_to_run = strtoull(optarg, NULL, 10);
            break;
        case 'D':
            disasm_file = optarg;
            break;
        case '?':
            // getopt_long already printed error message
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return EXIT_FAILURE;
        default:
            fprintf(stderr, "Unknown option.\n");
            return EXIT_FAILURE;
        }
    }

    // Check for unexpected positional arguments
    if (optind < argc)
    {
        fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[optind]);
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Initialize emulator
    spettrum_emulator_t *emulator = emulator_init();
    if (!emulator)
    {
        fprintf(stderr, "Error: Failed to initialize emulator\n");
        return EXIT_FAILURE;
    }

    // Initialize dump counter
    emulator->dump_count = 0;

    // Open disassembly file if specified
    if (disasm_file)
    {
        emulator->disasm_file = fopen(disasm_file, "w");
        if (!emulator->disasm_file)
        {
            fprintf(stderr, "Error: Cannot open disassembly file '%s'\n", disasm_file);
            emulator_cleanup(emulator);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "Disassembly will be written to '%s'\n", disasm_file);
    }

    // Set up signal handlers for graceful shutdown
    g_emulator = emulator;
    signal(SIGINT, signal_handler);       // Ctrl+C
    signal(SIGQUIT, signal_handler);      // Ctrl+D (SIGQUIT)
    signal(SIGUSR1, dump_memory_handler); // Memory dump (kill -USR1 <pid>)

    // Load ROM if specified
    if (rom_file)
    {
        if (emulator_load_rom(emulator, rom_file) != 0)
        {
            fprintf(stderr, "Error: Failed to load ROM from '%s'\n", rom_file);
            emulator_cleanup(emulator);
            return EXIT_FAILURE;
        }
    }

    // Load disk if specified
    if (disk_file)
    {
        if (emulator_load_disk(emulator, disk_file) != 0)
        {
            fprintf(stderr, "Error: Failed to load disk from '%s'\n", disk_file);
            emulator_cleanup(emulator);
            return EXIT_FAILURE;
        }
    }

    // Run emulation
    int result = emulator_run(emulator, instructions_to_run);

    // Cleanup
    emulator_cleanup(emulator);

    return result;
}

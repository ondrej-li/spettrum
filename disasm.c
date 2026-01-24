#include "disasm.h"
#include "z80.h"
#include "main.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

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
static const char *decode_dd_instruction(uint8_t opcode, uint8_t *memory, uint16_t pc)
{
    static char buf[64];

    switch (opcode)
    {
    case 0x09:
        return "ADD IX, BC";
    case 0x19:
        return "ADD IX, DE";
    case 0x21:
        return "LD IX, nn";
    case 0x22:
        return "LD (nn), IX";
    case 0x23:
        return "INC IX";
    case 0x24:
        return "INC IXH";
    case 0x25:
        return "DEC IXH";
    case 0x26:
        return "LD IXH, n";
    case 0x29:
        return "ADD IX, IX";
    case 0x2A:
        return "LD IX, (nn)";
    case 0x2B:
        return "DEC IX";
    case 0x2C:
        return "INC IXL";
    case 0x2D:
        return "DEC IXL";
    case 0x2E:
        return "LD IXL, n";
    case 0x34:
        return "INC (IX+d)";
    case 0x35:
        return "DEC (IX+d)";
    case 0x36:
        return "LD (IX+d), n";
    case 0x39:
        return "ADD IX, SP";
    case 0x44:
        return "LD B, IXH";
    case 0x45:
        return "LD B, IXL";
    case 0x46:
        return "LD B, (IX+d)";
    case 0x4C:
        return "LD C, IXH";
    case 0x4D:
        return "LD C, IXL";
    case 0x4E:
        return "LD C, (IX+d)";
    case 0x54:
        return "LD D, IXH";
    case 0x55:
        return "LD D, IXL";
    case 0x56:
        return "LD D, (IX+d)";
    case 0x5C:
        return "LD E, IXH";
    case 0x5D:
        return "LD E, IXL";
    case 0x5E:
        return "LD E, (IX+d)";
    case 0x60:
        return "LD IXH, B";
    case 0x61:
        return "LD IXH, C";
    case 0x62:
        return "LD IXH, D";
    case 0x63:
        return "LD IXH, E";
    case 0x64:
        return "LD IXH, IXH";
    case 0x65:
        return "LD IXH, IXL";
    case 0x66:
        return "LD H, (IX+d)";
    case 0x67:
        return "LD IXH, A";
    case 0x68:
        return "LD IXL, B";
    case 0x69:
        return "LD IXL, C";
    case 0x6A:
        return "LD IXL, D";
    case 0x6B:
        return "LD IXL, E";
    case 0x6C:
        return "LD IXL, IXH";
    case 0x6D:
        return "LD IXL, IXL";
    case 0x6E:
        return "LD L, (IX+d)";
    case 0x6F:
        return "LD IXL, A";
    case 0x70:
        return "LD (IX+d), B";
    case 0x71:
        return "LD (IX+d), C";
    case 0x72:
        return "LD (IX+d), D";
    case 0x73:
        return "LD (IX+d), E";
    case 0x74:
        return "LD (IX+d), H";
    case 0x75:
        return "LD (IX+d), L";
    case 0x77:
        return "LD (IX+d), A";
    case 0x7C:
        return "LD A, IXH";
    case 0x7D:
        return "LD A, IXL";
    case 0x7E:
        return "LD A, (IX+d)";
    case 0x84:
        return "ADD A, IXH";
    case 0x85:
        return "ADD A, IXL";
    case 0x86:
        return "ADD A, (IX+d)";
    case 0x8C:
        return "ADC A, IXH";
    case 0x8D:
        return "ADC A, IXL";
    case 0x8E:
        return "ADC A, (IX+d)";
    case 0x94:
        return "SUB IXH";
    case 0x95:
        return "SUB IXL";
    case 0x96:
        return "SUB (IX+d)";
    case 0x9C:
        return "SBC A, IXH";
    case 0x9D:
        return "SBC A, IXL";
    case 0x9E:
        return "SBC A, (IX+d)";
    case 0xA4:
        return "AND IXH";
    case 0xA5:
        return "AND IXL";
    case 0xA6:
        return "AND (IX+d)";
    case 0xAC:
        return "XOR IXH";
    case 0xAD:
        return "XOR IXL";
    case 0xAE:
        return "XOR (IX+d)";
    case 0xB4:
        return "OR IXH";
    case 0xB5:
        return "OR IXL";
    case 0xB6:
        return "OR (IX+d)";
    case 0xBC:
        return "CP IXH";
    case 0xBD:
        return "CP IXL";
    case 0xBE:
        return "CP (IX+d)";
    case 0xCB:
        if (pc + 3 < SPETTRUM_TOTAL_MEMORY)
        {
            uint8_t bit_op = memory[pc + 3];
            snprintf(buf, sizeof(buf), "DD CB (IX+d) %02X", bit_op);
            return buf;
        }
        return "DD CB (IX bit ops)";
    case 0xE1:
        return "POP IX";
    case 0xE3:
        return "EX (SP), IX";
    case 0xE5:
        return "PUSH IX";
    case 0xE9:
        return "JP (IX)";
    case 0xF9:
        return "LD SP, IX";
    default:
        snprintf(buf, sizeof(buf), "DD %02X (unknown)", opcode);
        return buf;
    }
}

static const char *decode_fd_instruction(uint8_t opcode, uint8_t *memory, uint16_t pc)
{
    static char buf[64];

    switch (opcode)
    {
    case 0x09:
        return "ADD IY, BC";
    case 0x19:
        return "ADD IY, DE";
    case 0x21:
        return "LD IY, nn";
    case 0x22:
        return "LD (nn), IY";
    case 0x23:
        return "INC IY";
    case 0x24:
        return "INC IYH";
    case 0x25:
        return "DEC IYH";
    case 0x26:
        return "LD IYH, n";
    case 0x29:
        return "ADD IY, IY";
    case 0x2A:
        return "LD IY, (nn)";
    case 0x2B:
        return "DEC IY";
    case 0x2C:
        return "INC IYL";
    case 0x2D:
        return "DEC IYL";
    case 0x2E:
        return "LD IYL, n";
    case 0x34:
        return "INC (IY+d)";
    case 0x35:
        return "DEC (IY+d)";
    case 0x36:
        return "LD (IY+d), n";
    case 0x39:
        return "ADD IY, SP";
    case 0x44:
        return "LD B, IYH";
    case 0x45:
        return "LD B, IYL";
    case 0x46:
        return "LD B, (IY+d)";
    case 0x4C:
        return "LD C, IYH";
    case 0x4D:
        return "LD C, IYL";
    case 0x4E:
        return "LD C, (IY+d)";
    case 0x54:
        return "LD D, IYH";
    case 0x55:
        return "LD D, IYL";
    case 0x56:
        return "LD D, (IY+d)";
    case 0x5C:
        return "LD E, IYH";
    case 0x5D:
        return "LD E, IYL";
    case 0x5E:
        return "LD E, (IY+d)";
    case 0x60:
        return "LD IYH, B";
    case 0x61:
        return "LD IYH, C";
    case 0x62:
        return "LD IYH, D";
    case 0x63:
        return "LD IYH, E";
    case 0x64:
        return "LD IYH, IYH";
    case 0x65:
        return "LD IYH, IYL";
    case 0x66:
        return "LD H, (IY+d)";
    case 0x67:
        return "LD IYH, A";
    case 0x68:
        return "LD IYL, B";
    case 0x69:
        return "LD IYL, C";
    case 0x6A:
        return "LD IYL, D";
    case 0x6B:
        return "LD IYL, E";
    case 0x6C:
        return "LD IYL, IYH";
    case 0x6D:
        return "LD IYL, IYL";
    case 0x6E:
        return "LD L, (IY+d)";
    case 0x6F:
        return "LD IYL, A";
    case 0x70:
        return "LD (IY+d), B";
    case 0x71:
        return "LD (IY+d), C";
    case 0x72:
        return "LD (IY+d), D";
    case 0x73:
        return "LD (IY+d), E";
    case 0x74:
        return "LD (IY+d), H";
    case 0x75:
        return "LD (IY+d), L";
    case 0x77:
        return "LD (IY+d), A";
    case 0x7C:
        return "LD A, IYH";
    case 0x7D:
        return "LD A, IYL";
    case 0x7E:
        return "LD A, (IY+d)";
    case 0x84:
        return "ADD A, IYH";
    case 0x85:
        return "ADD A, IYL";
    case 0x86:
        return "ADD A, (IY+d)";
    case 0x8C:
        return "ADC A, IYH";
    case 0x8D:
        return "ADC A, IYL";
    case 0x8E:
        return "ADC A, (IY+d)";
    case 0x94:
        return "SUB IYH";
    case 0x95:
        return "SUB IYL";
    case 0x96:
        return "SUB (IY+d)";
    case 0x9C:
        return "SBC A, IYH";
    case 0x9D:
        return "SBC A, IYL";
    case 0x9E:
        return "SBC A, (IY+d)";
    case 0xA4:
        return "AND IYH";
    case 0xA5:
        return "AND IYL";
    case 0xA6:
        return "AND (IY+d)";
    case 0xAC:
        return "XOR IYH";
    case 0xAD:
        return "XOR IYL";
    case 0xAE:
        return "XOR (IY+d)";
    case 0xB4:
        return "OR IYH";
    case 0xB5:
        return "OR IYL";
    case 0xB6:
        return "OR (IY+d)";
    case 0xBC:
        return "CP IYH";
    case 0xBD:
        return "CP IYL";
    case 0xBE:
        return "CP (IY+d)";
    case 0xCB:
        if (pc + 3 < SPETTRUM_TOTAL_MEMORY)
        {
            uint8_t bit_op = memory[pc + 3];
            snprintf(buf, sizeof(buf), "FD CB (IY+d) %02X", bit_op);
            return buf;
        }
        return "FD CB (IY bit ops)";
    case 0xE1:
        return "POP IY";
    case 0xE3:
        return "EX (SP), IY";
    case 0xE5:
        return "PUSH IY";
    case 0xE9:
        return "JP (IY)";
    case 0xF9:
        return "LD SP, IY";
    default:
        snprintf(buf, sizeof(buf), "FD %02X (unknown)", opcode);
        return buf;
    }
}

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
void log_instruction_disassembly(spettrum_emulator_t *emulator, uint16_t pc, uint8_t opcode)
{
    if (!emulator || !emulator->disasm_file)
        return;

    z80_registers_t *regs = &emulator->cpu->regs;
    z80_emulator_t *z80 = emulator->cpu;
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
    {
        uint8_t dd_opcode = (pc + 1 < SPETTRUM_TOTAL_MEMORY) ? emulator->memory[pc + 1] : 0;
        const char *dd_instr = decode_dd_instruction(dd_opcode, emulator->memory, pc);
        snprintf(instr_buf, sizeof(instr_buf), "DD %02X %s", dd_opcode, dd_instr);
        break;
    }
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
    {
        uint8_t fd_opcode = (pc + 1 < SPETTRUM_TOTAL_MEMORY) ? emulator->memory[pc + 1] : 0;
        const char *fd_instr = decode_fd_instruction(fd_opcode, emulator->memory, pc);
        snprintf(instr_buf, sizeof(instr_buf), "FD %02X %s", fd_opcode, fd_instr);
        break;
    }
    case 0xFE:
        snprintf(instr_buf, sizeof(instr_buf), "CP %02X", operand);
        break;
    case 0xFF:
        snprintf(instr_buf, sizeof(instr_buf), "RST 38");
        break;

        // All cases handled - should never reach default
    }

    // Decode flags: S Z H P/V N C (uppercase = 1, lowercase = 0)
    char flags[16];
    snprintf(flags, sizeof(flags), "%c%c%c%c%c%c",
             (regs->sf) ? 'S' : 's',
             (regs->zf) ? 'Z' : 'z',
             (regs->hf) ? 'H' : 'h',
             (regs->pf) ? 'P' : 'p',
             (regs->nf) ? 'N' : 'n',
             (regs->cf) ? 'C' : 'c');

    // Add memory access info for certain instructions
    char mem_info[64] = "";
    uint16_t bc = (regs->b << 8) | regs->c;
    uint16_t de = (regs->d << 8) | regs->e;
    uint16_t hl = (regs->h << 8) | regs->l;

    switch (opcode)
    {
    // POP instructions - show what was popped
    case 0xC1: // POP BC
    case 0xD1: // POP DE
    case 0xE1: // POP HL
    case 0xF1: // POP AF
    {
        uint16_t val = emulator->memory[regs->sp] | (emulator->memory[regs->sp + 1] << 8);
        snprintf(mem_info, sizeof(mem_info), " [SP]=%04X", val);
        break;
    }
    // PUSH instructions - show what will be pushed
    case 0xC5: // PUSH BC
        snprintf(mem_info, sizeof(mem_info), " [SP-2]=%04X", bc);
        break;
    case 0xD5: // PUSH DE
        snprintf(mem_info, sizeof(mem_info), " [SP-2]=%04X", de);
        break;
    case 0xE5: // PUSH HL
        snprintf(mem_info, sizeof(mem_info), " [SP-2]=%04X", hl);
        break;
    case 0xF5: // PUSH AF
        snprintf(mem_info, sizeof(mem_info), " [SP-2]=%04X", (regs->a << 8) | get_f(z80));
        break;
    // LD (addr), reg - show what's being written
    case 0x02: // LD (BC), A
        snprintf(mem_info, sizeof(mem_info), " [BC]=%02X", regs->a);
        break;
    case 0x12: // LD (DE), A
        snprintf(mem_info, sizeof(mem_info), " [DE]=%02X", regs->a);
        break;
    case 0x32: // LD (nn), A
        snprintf(mem_info, sizeof(mem_info), " [%04X]=%02X", addr, regs->a);
        break;
    case 0x36: // LD (HL), n
        snprintf(mem_info, sizeof(mem_info), " [HL]=%02X", operand);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73: // LD (HL), r
    case 0x74:
    case 0x75:
    case 0x77:
    {
        uint8_t val = 0;
        switch (opcode & 0x07)
        {
        case 0:
            val = regs->b;
            break;
        case 1:
            val = regs->c;
            break;
        case 2:
            val = regs->d;
            break;
        case 3:
            val = regs->e;
            break;
        case 4:
            val = regs->h;
            break;
        case 5:
            val = regs->l;
            break;
        case 7:
            val = regs->a;
            break;
        }
        snprintf(mem_info, sizeof(mem_info), " [HL]=%02X", val);
        break;
    }
    // LD reg, (addr) - show what's being read
    case 0x0A: // LD A, (BC)
        snprintf(mem_info, sizeof(mem_info), " [BC]=%02X", emulator->memory[bc]);
        break;
    case 0x1A: // LD A, (DE)
        snprintf(mem_info, sizeof(mem_info), " [DE]=%02X", emulator->memory[de]);
        break;
    case 0x3A: // LD A, (nn)
        snprintf(mem_info, sizeof(mem_info), " [%04X]=%02X", addr, emulator->memory[addr]);
        break;
    case 0x46:
    case 0x4E:
    case 0x56:
    case 0x5E: // LD r, (HL)
    case 0x66:
    case 0x6E:
    case 0x7E:
        snprintf(mem_info, sizeof(mem_info), " [HL]=%02X", emulator->memory[hl]);
        break;
    }

    // Format: PC: opcode instruction ; registers with decoded flags
    fprintf(emulator->disasm_file, "%04X: %02X %-28s ; A=%02X F=%s BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X%s\n",
            pc, opcode, instr_buf,
            regs->a, flags,
            bc, de, hl,
            regs->ix,
            regs->iy,
            regs->sp,
            mem_info);
    fflush(emulator->disasm_file);
}

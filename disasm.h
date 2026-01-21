#ifndef DISASM_H
#define DISASM_H

#include <stdint.h>
#include "main.h"

void log_instruction_disassembly(spettrum_emulator_t *emulator, uint16_t pc, uint8_t opcode);

#endif
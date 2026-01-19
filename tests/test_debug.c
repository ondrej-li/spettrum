#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../z80.c"

uint8_t mock_read_memory(void *user_data, uint16_t addr)
{
    uint8_t *mem = (uint8_t *)user_data;
    return mem[addr];
}

void mock_write_memory(void *user_data, uint16_t addr, uint8_t value)
{
    uint8_t *mem = (uint8_t *)user_data;
    mem[addr] = value;
}

int main()
{
    z80_emulator_t *z80 = z80_init();
    uint8_t memory[Z80_MAX_MEMORY] = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, memory);

    z80_set_register(z80, "D", 0x08);
    memory[0x0000] = 0xCB;
    memory[0x0001] = 0x5C;

    printf("Before: D=0x%02X, flags=0x%02X\n", z80_get_register(z80, "D"), z80_get_register(z80, "F"));

    z80_execute_instruction(z80);

    printf("After: D=0x%02X, flags=0x%02X\n", z80_get_register(z80, "D"), z80_get_register(z80, "F"));
    printf("Z flag set? %d\n", z80_get_register(z80, "F") & 0x40);

    z80_cleanup(z80);
    return 0;
}

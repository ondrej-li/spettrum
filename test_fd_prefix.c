#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Minimal memory for testing
#define TEST_MEMORY_SIZE 1024
static uint8_t test_memory[TEST_MEMORY_SIZE];

// Forward declarations from z80.c
typedef struct z80_emulator z80_emulator_t;
extern z80_emulator_t *z80_init(void);
extern void z80_set_memory_callbacks(z80_emulator_t *z80, uint8_t (*read)(void *, uint16_t), void (*write)(void *, uint16_t, uint8_t), void *user_data);
extern void z80_set_io_callbacks(z80_emulator_t *z80, uint8_t (*read)(void *, uint8_t), void (*write)(void *, uint8_t, uint8_t), void *user_data);
extern uint16_t z80_get_register(z80_emulator_t *z80, const char *reg_name);
extern int z80_execute_instruction(z80_emulator_t *z80);
extern void z80_cleanup(z80_emulator_t *z80);

// Test memory callbacks
static uint8_t test_read_memory(void *user_data, uint16_t addr)
{
    if (addr < TEST_MEMORY_SIZE)
        return test_memory[addr];
    return 0xFF;
}

static void test_write_memory(void *user_data, uint16_t addr, uint8_t value)
{
    if (addr < TEST_MEMORY_SIZE)
        test_memory[addr] = value;
}

static uint8_t test_read_io(void *user_data, uint8_t port)
{
    return 0xFF; // No devices
}

static void test_write_io(void *user_data, uint8_t port, uint8_t value)
{
    // No devices
}

int main(void)
{
    printf("Testing FD prefix instruction support...\n");

    z80_emulator_t *z80 = z80_init();
    if (!z80)
    {
        fprintf(stderr, "Failed to initialize Z80\n");
        return 1;
    }

    // Set up memory and I/O callbacks
    z80_set_memory_callbacks(z80, test_read_memory, test_write_memory, NULL);
    z80_set_io_callbacks(z80, test_read_io, test_write_io, NULL);

    // Test 1: FD 21 nn nn - LD IY, nn
    printf("\nTest 1: FD 0x21 0x34 0x12 (LD IY,0x1234)\n");
    memset(test_memory, 0, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD; // FD prefix
    test_memory[0x0001] = 0x21; // LD IY,nn
    test_memory[0x0002] = 0x34; // low byte
    test_memory[0x0003] = 0x12; // high byte
    z80_execute_instruction(z80);
    uint16_t iy = z80_get_register(z80, "iy");
    printf("   IY = 0x%04X (expected 0x1234)\n", iy);
    if (iy == 0x1234)
        printf("   PASS\n");
    else
        printf("   FAIL\n");

    // Test 2: FD 2A nn nn - LD IY,(nn)
    printf("\nTest 2: FD 0x2A 0x10 0x00 (LD IY,(0x0010))\n");
    memset(test_memory, 0, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD; // FD prefix
    test_memory[0x0001] = 0x2A; // LD IY,(nn)
    test_memory[0x0002] = 0x10; // low byte of address
    test_memory[0x0003] = 0x00; // high byte of address
    test_memory[0x0010] = 0x78; // IY low byte
    test_memory[0x0011] = 0x56; // IY high byte
    z80_execute_instruction(z80);
    iy = z80_get_register(z80, "iy");
    printf("   IY = 0x%04X (expected 0x5678)\n", iy);
    if (iy == 0x5678)
        printf("   PASS\n");
    else
        printf("   FAIL\n");

    // Test 3: FD 23 - INC IY
    printf("\nTest 3: FD 0x23 (INC IY)\n");
    memset(test_memory, 0, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD; // FD prefix
    test_memory[0x0001] = 0x23; // INC IY
    z80_execute_instruction(z80);
    iy = z80_get_register(z80, "iy");
    printf("   IY = 0x%04X (expected 0x0001)\n", iy);
    if (iy == 0x0001)
        printf("   PASS\n");
    else
        printf("   FAIL\n");

    // Test 4: FD 2B - DEC IY
    printf("\nTest 4: FD 0x2B (DEC IY)\n");
    memset(test_memory, 0, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD; // FD prefix
    test_memory[0x0001] = 0x2B; // DEC IY
    z80_execute_instruction(z80);
    iy = z80_get_register(z80, "iy");
    printf("   IY = 0x%04X (expected 0xFFFF)\n", iy);
    if (iy == 0xFFFF)
        printf("   PASS\n");
    else
        printf("   FAIL\n");

    z80_cleanup(z80);
    printf("\nFD prefix instruction testing complete.\n");
    return 0;
}

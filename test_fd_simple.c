#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// Test memory for code
#define TEST_MEMORY_SIZE 4096
static uint8_t test_memory[TEST_MEMORY_SIZE];

// Forward declarations
typedef struct z80_emulator z80_emulator_t;
extern z80_emulator_t *z80_init(void);
extern void z80_cleanup(z80_emulator_t *z80);
extern void z80_set_memory_callbacks(z80_emulator_t *z80,
                                     uint8_t (*read)(void *, uint16_t),
                                     void (*write)(void *, uint16_t, uint8_t),
                                     void *user_data);
extern void z80_set_io_callbacks(z80_emulator_t *z80,
                                 uint8_t (*read)(void *, uint8_t),
                                 void (*write)(void *, uint8_t, uint8_t),
                                 void *user_data);
extern uint16_t z80_get_register(z80_emulator_t *z80, const char *reg_name);
extern void z80_set_pc(z80_emulator_t *z80, uint16_t pc);
extern int z80_start(z80_emulator_t *z80);
extern void z80_stop(z80_emulator_t *z80);

// Memory callbacks
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
    return 0xFF;
}

static void test_write_io(void *user_data, uint8_t port, uint8_t value)
{
    // No-op
}

int main(void)
{
    printf("Testing FD prefix (IY register) instruction support...\n\n");

    z80_emulator_t *z80 = z80_init();
    if (!z80)
    {
        fprintf(stderr, "Failed to initialize Z80\n");
        return 1;
    }

    z80_set_memory_callbacks(z80, test_read_memory, test_write_memory, NULL);
    z80_set_io_callbacks(z80, test_read_io, test_write_io, NULL);

    // Test 1: Load FD 0x21 0x34 0x12 (LD IY,0x1234)
    printf("Test 1: FD 0x21 0x34 0x12 - LD IY,0x1234\n");
    memset(test_memory, 0x00, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD;
    test_memory[0x0001] = 0x21;
    test_memory[0x0002] = 0x34;
    test_memory[0x0003] = 0x12;
    test_memory[0x0004] = 0x00; // NOP to stop

    z80_set_pc(z80, 0x0000);
    z80_start(z80);
    sleep(1);
    z80_stop(z80);

    uint16_t iy = z80_get_register(z80, "iy");
    printf("  Result: IY = 0x%04X (expected 0x1234)\n", iy);
    printf("  Status: %s\n\n", (iy == 0x1234) ? "PASS" : "FAIL");

    // Test 2: LD IY,(nn)
    printf("Test 2: FD 0x2A 0x10 0x00 - LD IY,(0x0010)\n");
    memset(test_memory, 0x00, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD;
    test_memory[0x0001] = 0x2A;
    test_memory[0x0002] = 0x10;
    test_memory[0x0003] = 0x00;
    test_memory[0x0010] = 0x78;
    test_memory[0x0011] = 0x56;
    test_memory[0x0004] = 0x00; // NOP to stop

    z80_set_pc(z80, 0x0000);
    z80_start(z80);
    sleep(1);
    z80_stop(z80);

    iy = z80_get_register(z80, "iy");
    printf("  Result: IY = 0x%04X (expected 0x5678)\n", iy);
    printf("  Status: %s\n\n", (iy == 0x5678) ? "PASS" : "FAIL");

    // Test 3: INC IY
    printf("Test 3: FD 0x23 - INC IY\n");
    memset(test_memory, 0x00, TEST_MEMORY_SIZE);
    test_memory[0x0000] = 0xFD;
    test_memory[0x0001] = 0x23;
    test_memory[0x0002] = 0x00; // NOP to stop

    z80_set_pc(z80, 0x0000);
    z80_start(z80);
    sleep(1);
    z80_stop(z80);

    iy = z80_get_register(z80, "iy");
    printf("  Result: IY = 0x%04X (expected 0x0001)\n", iy);
    printf("  Status: %s\n\n", (iy == 0x0001) ? "PASS" : "FAIL");

    z80_cleanup(z80);
    printf("FD prefix instruction testing complete.\n");
    return 0;
}

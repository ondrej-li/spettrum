#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

// Include the Z80 module for testing
#include "../z80.c"

// Test helper macros
#define TEST_ASSERT(condition, message)                                 \
    do                                                                  \
    {                                                                   \
        if (!(condition))                                               \
        {                                                               \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            test_count_failed++;                                        \
            return 0;                                                   \
        }                                                               \
    } while (0)

#define TEST_ASSERT_EQ(expected, actual, message)                            \
    do                                                                       \
    {                                                                        \
        if ((expected) != (actual))                                          \
        {                                                                    \
            fprintf(stderr, "FAIL: %s - Expected %d but got %d (line %d)\n", \
                    message, (int)(expected), (int)(actual), __LINE__);      \
            test_count_failed++;                                             \
            return 0;                                                        \
        }                                                                    \
    } while (0)

static int test_count_passed = 0;
static int test_count_failed = 0;

// Mock memory structure for testing
typedef struct
{
    uint8_t memory[Z80_MAX_MEMORY];
} mock_memory_t;

// Mock I/O structure for testing
typedef struct
{
    uint8_t io_ports[256];
    int io_read_count;
    int io_write_count;
} mock_io_t;

// Mock memory callbacks
static uint8_t mock_read_memory(void *user_data, uint16_t addr)
{
    mock_memory_t *mem = (mock_memory_t *)user_data;
    return mem->memory[addr];
}

static void mock_write_memory(void *user_data, uint16_t addr, uint8_t value)
{
    mock_memory_t *mem = (mock_memory_t *)user_data;
    mem->memory[addr] = value;
}

// Mock I/O callbacks
static uint8_t mock_read_io(void *user_data, uint8_t port)
{
    mock_io_t *io = (mock_io_t *)user_data;
    io->io_read_count++;
    return io->io_ports[port];
}

static void mock_write_io(void *user_data, uint8_t port, uint8_t value)
{
    mock_io_t *io = (mock_io_t *)user_data;
    io->io_write_count++;
    io->io_ports[port] = value;
}

/**
 * Test 1: NOP instruction (0x00)
 */
static int test_nop(void)
{
    printf("Test 1: NOP (0x00)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Load NOP instruction
    memory.memory[0x0000] = 0x00;

    uint16_t initial_pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0000, initial_pc, "Initial PC should be 0");

    // Execute instruction manually
    z80_execute_instruction(z80);

    uint16_t new_pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0001, new_pc, "PC should advance by 1 after NOP");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 2: LD B,n (0x06) - Load immediate into B
 */
static int test_ld_b_n(void)
{
    printf("Test 2: LD B,n (0x06)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Load LD B,0x42
    memory.memory[0x0000] = 0x06;
    memory.memory[0x0001] = 0x42;

    z80_execute_instruction(z80);

    uint16_t b_reg = z80_get_register(z80, "B");
    TEST_ASSERT_EQ(0x42, b_reg, "B register should contain 0x42");

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0002, pc, "PC should advance by 2");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 3: LD BC,nn (0x01) - Load immediate 16-bit into BC
 */
static int test_ld_bc_nn(void)
{
    printf("Test 3: LD BC,nn (0x01)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Load LD BC,0x1234 (little-endian: C first, then B)
    memory.memory[0x0000] = 0x01;
    memory.memory[0x0001] = 0x34; // C
    memory.memory[0x0002] = 0x12; // B

    z80_execute_instruction(z80);

    uint16_t b = z80_get_register(z80, "B");
    uint16_t c = z80_get_register(z80, "C");
    TEST_ASSERT_EQ(0x12, b, "B should be 0x12");
    TEST_ASSERT_EQ(0x34, c, "C should be 0x34");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 4: INC B (0x04) - Increment B register
 */
static int test_inc_b(void)
{
    printf("Test 4: INC B (0x04)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x42);
    memory.memory[0x0000] = 0x04;

    z80_execute_instruction(z80);

    uint16_t b = z80_get_register(z80, "B");
    TEST_ASSERT_EQ(0x43, b, "B should increment to 0x43");

    // Test zero flag when result is 0
    z80_set_register(z80, "B", 0xFF);
    memory.memory[0x0001] = 0x04;
    z80_set_pc(z80, 0x0001);
    z80_execute_instruction(z80);

    b = z80_get_register(z80, "B");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x00, b, "B should wrap to 0x00");
    TEST_ASSERT((f & Z80_FLAG_Z), "Zero flag should be set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 5: DEC B (0x05) - Decrement B register
 */
static int test_dec_b(void)
{
    printf("Test 5: DEC B (0x05)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x42);
    memory.memory[0x0000] = 0x05;

    z80_execute_instruction(z80);

    uint16_t b = z80_get_register(z80, "B");
    TEST_ASSERT_EQ(0x41, b, "B should decrement to 0x41");

    // Test zero flag
    z80_set_register(z80, "B", 0x01);
    memory.memory[0x0001] = 0x05;
    z80_set_pc(z80, 0x0001);
    z80_execute_instruction(z80);

    b = z80_get_register(z80, "B");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x00, b, "B should decrement to 0x00");
    TEST_ASSERT((f & Z80_FLAG_Z), "Zero flag should be set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 6: LD A,n (0x3E) - Load immediate into A
 */
static int test_ld_a_n(void)
{
    printf("Test 6: LD A,n (0x3E)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x3E;
    memory.memory[0x0001] = 0x55;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    TEST_ASSERT_EQ(0x55, a, "A should contain 0x55");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 7: ADD A,B (0x80) - Add B to A
 */
static int test_add_a_b(void)
{
    printf("Test 7: ADD A,B (0x80)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "A", 0x10);
    z80_set_register(z80, "B", 0x20);
    memory.memory[0x0000] = 0x80;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    TEST_ASSERT_EQ(0x30, a, "A should be 0x30 (0x10 + 0x20)");

    // Test carry flag
    z80_set_register(z80, "A", 0xFF);
    z80_set_register(z80, "B", 0x02);
    memory.memory[0x0001] = 0x80;
    z80_set_pc(z80, 0x0001);
    z80_execute_instruction(z80);

    a = z80_get_register(z80, "A");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x01, a, "A should wrap to 0x01");
    TEST_ASSERT((f & Z80_FLAG_C), "Carry flag should be set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 8: SUB A,B (0x90) - Subtract B from A
 */
static int test_sub_a_b(void)
{
    printf("Test 8: SUB A,B (0x90)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "A", 0x50);
    z80_set_register(z80, "B", 0x30);
    memory.memory[0x0000] = 0x90;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x20, a, "A should be 0x20 (0x50 - 0x30)");
    TEST_ASSERT((f & Z80_FLAG_N), "Subtract flag should be set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 9: CP A,B (0xB8) - Compare A with B
 */
static int test_cp_a_b(void)
{
    printf("Test 9: CP A,B (0xB8)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "A", 0x42);
    z80_set_register(z80, "B", 0x42);
    memory.memory[0x0000] = 0xB8;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x42, a, "A should not be modified by CP");
    TEST_ASSERT((f & Z80_FLAG_Z), "Zero flag should be set (values equal)");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 10: LD (HL),A (0x77) - Load A into memory at HL
 */
static int test_ld_hl_a(void)
{
    printf("Test 10: LD (HL),A (0x77)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "H", 0x10);
    z80_set_register(z80, "L", 0x00);
    z80_set_register(z80, "A", 0x42);
    memory.memory[0x0000] = 0x77;

    z80_execute_instruction(z80);

    uint8_t mem_value = memory.memory[0x1000];
    TEST_ASSERT_EQ(0x42, mem_value, "Memory at 0x1000 should contain 0x42");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 11: LD A,(HL) (0x7E) - Load from memory at HL into A
 */
static int test_ld_a_hl(void)
{
    printf("Test 11: LD A,(HL) (0x7E)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "H", 0x10);
    z80_set_register(z80, "L", 0x00);
    memory.memory[0x1000] = 0x99;
    memory.memory[0x0000] = 0x7E;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    TEST_ASSERT_EQ(0x99, a, "A should contain 0x99 from memory");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 12: LD (BC),A (0x02) - Load A into memory at BC
 */
static int test_ld_bc_a(void)
{
    printf("Test 12: LD (BC),A (0x02)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x20);
    z80_set_register(z80, "C", 0x30);
    z80_set_register(z80, "A", 0x77);
    memory.memory[0x0000] = 0x02;

    z80_execute_instruction(z80);

    uint8_t mem_value = memory.memory[0x2030];
    TEST_ASSERT_EQ(0x77, mem_value, "Memory at 0x2030 should contain 0x77");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 13: INC BC (0x03) - Increment BC register pair
 */
static int test_inc_bc(void)
{
    printf("Test 13: INC BC (0x03)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x12);
    z80_set_register(z80, "C", 0x34);
    memory.memory[0x0000] = 0x03;

    z80_execute_instruction(z80);

    uint16_t b = z80_get_register(z80, "B");
    uint16_t c = z80_get_register(z80, "C");
    TEST_ASSERT_EQ(0x12, b, "B should remain 0x12");
    TEST_ASSERT_EQ(0x35, c, "C should increment to 0x35");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 14: RLCA (0x07) - Rotate A left circular
 */
static int test_rlca(void)
{
    printf("Test 14: RLCA (0x07)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "A", 0x80); // 10000000
    memory.memory[0x0000] = 0x07;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    uint16_t f = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x01, a, "A should be 0x01 after rotate");
    TEST_ASSERT((f & Z80_FLAG_C), "Carry flag should be set (MSB was 1)");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 15: JP nn (0xC3) - Jump to address
 */
static int test_jp_nn(void)
{
    printf("Test 15: JP nn (0xC3)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0xC3;
    memory.memory[0x0001] = 0x34; // Low byte
    memory.memory[0x0002] = 0x12; // High byte

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x1234, pc, "PC should jump to 0x1234");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 16: IN A,(n) (0xDB) - Read from I/O port
 */
static int test_in_a_n(void)
{
    printf("Test 16: IN A,(n) (0xDB)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    mock_io_t io = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);
    z80_set_io_callbacks(z80, mock_read_io, mock_write_io, &io);

    io.io_ports[0x50] = 0xAA;
    memory.memory[0x0000] = 0xDB;
    memory.memory[0x0001] = 0x50;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    TEST_ASSERT_EQ(0xAA, a, "A should contain value from I/O port 0x50");
    TEST_ASSERT_EQ(1, io.io_read_count, "I/O read should be called once");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 17: OUT (n),A (0xD3) - Write to I/O port
 */
static int test_out_n_a(void)
{
    printf("Test 17: OUT (n),A (0xD3)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    mock_io_t io = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);
    z80_set_io_callbacks(z80, mock_read_io, mock_write_io, &io);

    z80_set_register(z80, "A", 0xBB);
    memory.memory[0x0000] = 0xD3;
    memory.memory[0x0001] = 0x60;

    z80_execute_instruction(z80);

    TEST_ASSERT_EQ(0xBB, io.io_ports[0x60], "I/O port 0x60 should contain 0xBB");
    TEST_ASSERT_EQ(1, io.io_write_count, "I/O write should be called once");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 18: State save and load
 */
static int test_state_save_load(void)
{
    printf("Test 18: State save and load...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Set some register values
    z80_set_register(z80, "A", 0x42);
    z80_set_register(z80, "B", 0x11);
    z80_set_register(z80, "C", 0x22);
    z80_set_register(z80, "PC", 0x0100);

    // Save state
    uint8_t state_buffer[Z80_STATE_SIZE];
    size_t saved = z80_save_state(z80, state_buffer, Z80_STATE_SIZE);
    TEST_ASSERT_EQ(Z80_STATE_SIZE, saved, "State save should return correct size");

    // Modify registers
    z80_set_register(z80, "A", 0x99);
    z80_set_register(z80, "B", 0xAA);
    z80_set_register(z80, "PC", 0x0200);

    // Load state
    int result = z80_load_state(z80, state_buffer, Z80_STATE_SIZE);
    TEST_ASSERT_EQ(0, result, "State load should succeed");

    // Verify registers restored
    uint16_t a = z80_get_register(z80, "A");
    uint16_t b = z80_get_register(z80, "B");
    uint16_t c = z80_get_register(z80, "C");
    uint16_t pc = z80_get_pc(z80);

    TEST_ASSERT_EQ(0x42, a, "A should be restored to 0x42");
    TEST_ASSERT_EQ(0x11, b, "B should be restored to 0x11");
    TEST_ASSERT_EQ(0x22, c, "C should be restored to 0x22");
    TEST_ASSERT_EQ(0x0100, pc, "PC should be restored to 0x0100");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 19: JR n (0x18) - Relative jump
 */
static int test_jr_n(void)
{
    printf("Test 19: JR n (0x18)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x18;
    memory.memory[0x0001] = 0x10; // Offset +16

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0012, pc, "PC should be 0x0012 (0x0002 + 0x10)");

    // Test negative offset
    z80_set_pc(z80, 0x0100);
    memory.memory[0x0100] = 0x18;
    memory.memory[0x0101] = 0xFE; // Offset -2 (as signed byte)

    z80_execute_instruction(z80);

    pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0100, pc, "PC should be 0x0100 (0x0102 + 0xFE = 0x0102 - 2)");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 20: JR NZ,n (0x20) - Jump if not zero
 */
static int test_jr_nz(void)
{
    printf("Test 20: JR NZ,n (0x20)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when Z flag is not set (should jump)
    z80_set_register(z80, "F", 0x00); // No flags set
    memory.memory[0x0000] = 0x20;
    memory.memory[0x0001] = 0x20; // Offset +32

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0022, pc, "PC should jump to 0x0022 when Z flag not set");

    // Test when Z flag IS set (should NOT jump)
    z80_set_pc(z80, 0x0100);
    z80_set_register(z80, "F", Z80_FLAG_Z); // Z flag set
    memory.memory[0x0100] = 0x20;
    memory.memory[0x0101] = 0x20;

    z80_execute_instruction(z80);

    pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0102, pc, "PC should not jump when Z flag is set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 21: JR Z,n (0x28) - Jump if zero
 */
static int test_jr_z(void)
{
    printf("Test 21: JR Z,n (0x28)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when Z flag IS set (should jump)
    z80_set_register(z80, "F", Z80_FLAG_Z);
    memory.memory[0x0000] = 0x28;
    memory.memory[0x0001] = 0x15; // Offset +21

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0017, pc, "PC should jump to 0x0017 when Z flag is set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 22: JR NC,n (0x30) - Jump if no carry
 */
static int test_jr_nc(void)
{
    printf("Test 22: JR NC,n (0x30)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when C flag is NOT set (should jump)
    z80_set_register(z80, "F", 0x00);
    memory.memory[0x0000] = 0x30;
    memory.memory[0x0001] = 0x08;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x000A, pc, "PC should jump when C flag not set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 23: JR C,n (0x38) - Jump if carry
 */
static int test_jr_c(void)
{
    printf("Test 23: JR C,n (0x38)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when C flag IS set (should jump)
    z80_set_register(z80, "F", Z80_FLAG_C);
    memory.memory[0x0000] = 0x38;
    memory.memory[0x0001] = 0x10;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x0012, pc, "PC should jump when C flag is set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 24: LD C,n (0x0E) - Load immediate into C
 */
static int test_ld_c_n(void)
{
    printf("Test 24: LD C,n (0x0E)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x0E;
    memory.memory[0x0001] = 0xCC;

    z80_execute_instruction(z80);

    uint16_t c = z80_get_register(z80, "C");
    TEST_ASSERT_EQ(0xCC, c, "C should contain 0xCC");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 25: LD D,n (0x16) - Load immediate into D
 */
static int test_ld_d_n(void)
{
    printf("Test 25: LD D,n (0x16)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x16;
    memory.memory[0x0001] = 0xDD;

    z80_execute_instruction(z80);

    uint16_t d = z80_get_register(z80, "D");
    TEST_ASSERT_EQ(0xDD, d, "D should contain 0xDD");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 26: LD E,n (0x1E) - Load immediate into E
 */
static int test_ld_e_n(void)
{
    printf("Test 26: LD E,n (0x1E)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x1E;
    memory.memory[0x0001] = 0xEE;

    z80_execute_instruction(z80);

    uint16_t e = z80_get_register(z80, "E");
    TEST_ASSERT_EQ(0xEE, e, "E should contain 0xEE");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 27: LD H,n (0x26) - Load immediate into H
 */
static int test_ld_h_n(void)
{
    printf("Test 27: LD H,n (0x26)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x26;
    memory.memory[0x0001] = 0x44;

    z80_execute_instruction(z80);

    uint16_t h = z80_get_register(z80, "H");
    TEST_ASSERT_EQ(0x44, h, "H should contain 0x44");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 28: LD L,n (0x2E) - Load immediate into L
 */
static int test_ld_l_n(void)
{
    printf("Test 28: LD L,n (0x2E)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x2E;
    memory.memory[0x0001] = 0x88;

    z80_execute_instruction(z80);

    uint16_t l = z80_get_register(z80, "L");
    TEST_ASSERT_EQ(0x88, l, "L should contain 0x88");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 29: JP NZ,nn (0xC2) - Jump if not zero
 */
static int test_jp_nz_nn(void)
{
    printf("Test 29: JP NZ,nn (0xC2)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when Z flag is not set (should jump)
    z80_set_register(z80, "F", 0x00);
    memory.memory[0x0000] = 0xC2;
    memory.memory[0x0001] = 0x00;
    memory.memory[0x0002] = 0x30;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x3000, pc, "PC should jump to 0x3000 when Z flag not set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 30: JP Z,nn (0xCA) - Jump if zero
 */
static int test_jp_z_nn(void)
{
    printf("Test 30: JP Z,nn (0xCA)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when Z flag IS set (should jump)
    z80_set_register(z80, "F", Z80_FLAG_Z);
    memory.memory[0x0000] = 0xCA;
    memory.memory[0x0001] = 0x50;
    memory.memory[0x0002] = 0x40;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x4050, pc, "PC should jump to 0x4050 when Z flag is set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 31: JP NC,nn (0xD2) - Jump if no carry
 */
static int test_jp_nc_nn(void)
{
    printf("Test 31: JP NC,nn (0xD2)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when C flag is not set (should jump)
    z80_set_register(z80, "F", 0x00);
    memory.memory[0x0000] = 0xD2;
    memory.memory[0x0001] = 0x22;
    memory.memory[0x0002] = 0x11;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x1122, pc, "PC should jump when C flag not set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 32: JP C,nn (0xDA) - Jump if carry
 */
static int test_jp_c_nn(void)
{
    printf("Test 32: JP C,nn (0xDA)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // Test when C flag IS set (should jump)
    z80_set_register(z80, "F", Z80_FLAG_C);
    memory.memory[0x0000] = 0xDA;
    memory.memory[0x0001] = 0x77;
    memory.memory[0x0002] = 0x88;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    TEST_ASSERT_EQ(0x8877, pc, "PC should jump when C flag is set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 33: CALL nn (0xCD) - Call subroutine
 */
static int test_call_nn(void)
{
    printf("Test 33: CALL nn (0xCD)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "SP", 0x8000);
    memory.memory[0x0000] = 0xCD;
    memory.memory[0x0001] = 0x34;
    memory.memory[0x0002] = 0x12;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    uint16_t sp = z80_get_register(z80, "SP");

    TEST_ASSERT_EQ(0x1234, pc, "PC should jump to 0x1234");
    TEST_ASSERT_EQ(0x7FFE, sp, "SP should be decremented by 2");

    // Check return address on stack
    uint8_t ret_low = memory.memory[0x7FFE];
    uint8_t ret_high = memory.memory[0x7FFF];
    TEST_ASSERT_EQ(0x03, ret_low, "Return address low byte should be 0x03");
    TEST_ASSERT_EQ(0x00, ret_high, "Return address high byte should be 0x00");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 34: RET (0xC9) - Return from subroutine
 */
static int test_ret(void)
{
    printf("Test 34: RET (0xC9)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "SP", 0x7FFE);
    memory.memory[0x7FFE] = 0x00; // Return address low byte
    memory.memory[0x7FFF] = 0x20; // Return address high byte
    memory.memory[0x0000] = 0xC9;

    z80_execute_instruction(z80);

    uint16_t pc = z80_get_pc(z80);
    uint16_t sp = z80_get_register(z80, "SP");

    TEST_ASSERT_EQ(0x2000, pc, "PC should be restored to 0x2000");
    TEST_ASSERT_EQ(0x8000, sp, "SP should be incremented by 2");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 35: DI (0xF3) - Disable interrupts
 */
static int test_di(void)
{
    printf("Test 35: DI (0xF3)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80->regs.iff1 = 1;
    z80->regs.iff2 = 1;
    memory.memory[0x0000] = 0xF3;

    z80_execute_instruction(z80);

    TEST_ASSERT_EQ(0, z80->regs.iff1, "IFF1 should be disabled");
    TEST_ASSERT_EQ(0, z80->regs.iff2, "IFF2 should be disabled");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 36: EI (0xFB) - Enable interrupts
 */
static int test_ei(void)
{
    printf("Test 36: EI (0xFB)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80->regs.iff1 = 0;
    z80->regs.iff2 = 0;
    memory.memory[0x0000] = 0xFB;

    z80_execute_instruction(z80);

    TEST_ASSERT_EQ(1, z80->regs.iff1, "IFF1 should be enabled");
    TEST_ASSERT_EQ(1, z80->regs.iff2, "IFF2 should be enabled");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 37: LD A,B (0x78) - Load B into A
 */
static int test_ld_a_b(void)
{
    printf("Test 37: LD A,B (0x78)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x55);
    memory.memory[0x0000] = 0x78;

    z80_execute_instruction(z80);

    uint16_t a = z80_get_register(z80, "A");
    TEST_ASSERT_EQ(0x55, a, "A should contain 0x55 from B");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 38: LD B,A (0x47) - Load A into B
 */
static int test_ld_b_a(void)
{
    printf("Test 38: LD B,A (0x47)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "A", 0x66);
    memory.memory[0x0000] = 0x47;

    z80_execute_instruction(z80);

    uint16_t b = z80_get_register(z80, "B");
    TEST_ASSERT_EQ(0x66, b, "B should contain 0x66 from A");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 39: LD DE,nn (0x11) - Load immediate into DE
 */
static int test_ld_de_nn(void)
{
    printf("Test 39: LD DE,nn (0x11)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x11;
    memory.memory[0x0001] = 0x56; // E
    memory.memory[0x0002] = 0x34; // D

    z80_execute_instruction(z80);

    uint16_t d = z80_get_register(z80, "D");
    uint16_t e = z80_get_register(z80, "E");
    TEST_ASSERT_EQ(0x34, d, "D should be 0x34");
    TEST_ASSERT_EQ(0x56, e, "E should be 0x56");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 40: LD HL,nn (0x21) - Load immediate into HL
 */
static int test_ld_hl_nn(void)
{
    printf("Test 40: LD HL,nn (0x21)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    memory.memory[0x0000] = 0x21;
    memory.memory[0x0001] = 0x78; // L
    memory.memory[0x0002] = 0x56; // H

    z80_execute_instruction(z80);

    uint16_t h = z80_get_register(z80, "H");
    uint16_t l = z80_get_register(z80, "L");
    TEST_ASSERT_EQ(0x56, h, "H should be 0x56");
    TEST_ASSERT_EQ(0x78, l, "L should be 0x78");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 41: CB prefix - RLC B (Rotate Left Circular B)
 */
static int test_cb_rlc_b(void)
{
    printf("Test 41: CB RLC B (0xCB 0x00)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "B", 0x81); // 10000001 -> should rotate to 00000011, carry=1

    memory.memory[0x0000] = 0xCB;
    memory.memory[0x0001] = 0x00; // RLC B

    z80_execute_instruction(z80);

    uint8_t b = z80_get_register(z80, "B");
    uint8_t flags = z80_get_register(z80, "F");
    TEST_ASSERT_EQ(0x03, b, "B should be 0x03 after RLC");
    TEST_ASSERT(flags & Z80_FLAG_C, "Carry flag should be set");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 42: CB prefix - BIT test (Test bit 3 in D)
 */
static int test_cb_bit_d(void)
{
    printf("Test 42: CB BIT 3,D (0xCB 0x5C)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "D", 0x08); // 00001000 (bit 3 is set)

    memory.memory[0x0000] = 0xCB;
    memory.memory[0x0001] = 0x5A; // BIT 3,D (01011010)

    z80_execute_instruction(z80);

    uint8_t flags = z80_get_register(z80, "F");
    TEST_ASSERT(!(flags & Z80_FLAG_Z), "Zero flag should NOT be set (bit 3 is set)");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 43: CB prefix - RES (Reset Bit 2 in E)
 */
static int test_cb_res_e(void)
{
    printf("Test 43: CB RES 2,E (0xCB 0xA3)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "E", 0x04); // 00000100 (bit 2 is set)

    memory.memory[0x0000] = 0xCB;
    memory.memory[0x0001] = 0x93; // RES 2,E (10010011)

    z80_execute_instruction(z80);

    uint8_t e = z80_get_register(z80, "E");
    TEST_ASSERT_EQ(0x00, e, "E should be 0x00 after RES 2,E");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 44: CB prefix - SET (Set Bit 5 in L)
 */
static int test_cb_set_l(void)
{
    printf("Test 44: CB SET 5,L (0xCB 0xED)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    z80_set_register(z80, "L", 0x00);

    memory.memory[0x0000] = 0xCB;
    memory.memory[0x0001] = 0xED; // SET 5,L

    z80_execute_instruction(z80);

    uint8_t l = z80_get_register(z80, "L");
    TEST_ASSERT_EQ(0x20, l, "L should be 0x20 after SET 5,L");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 45: ED prefix - IN B,(C) - Input from port C into B
 */
static int test_ed_in_b_c(void)
{
    printf("Test 45: ED IN B,(C) (0xED 0x40)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    mock_io_t io = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);
    z80_set_io_callbacks(z80, mock_read_io, mock_write_io, &io);

    z80_set_register(z80, "C", 0x50); // Port address
    io.io_ports[0x50] = 0xAB;         // Data on port

    memory.memory[0x0000] = 0xED;
    memory.memory[0x0001] = 0x40; // IN B,(C)

    z80_execute_instruction(z80);

    uint8_t b = z80_get_register(z80, "B");
    TEST_ASSERT_EQ(0xAB, b, "B should be 0xAB after IN B,(C)");
    TEST_ASSERT_EQ(1, io.io_read_count, "IO read should have occurred");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 46: ED prefix - OUT (C),A - Output from A to port C
 */
static int test_ed_out_c_a(void)
{
    printf("Test 46: ED OUT (C),A (0xED 0x4F)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    mock_io_t io = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);
    z80_set_io_callbacks(z80, mock_read_io, mock_write_io, &io);

    z80_set_register(z80, "A", 0x42);
    z80_set_register(z80, "C", 0x60); // Port address

    memory.memory[0x0000] = 0xED;
    memory.memory[0x0001] = 0x4F; // OUT (C),A

    z80_execute_instruction(z80);

    TEST_ASSERT_EQ(0x42, io.io_ports[0x60], "Port 0x60 should contain 0x42");
    TEST_ASSERT_EQ(1, io.io_write_count, "IO write should have occurred");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 47: ED prefix - RRD (Rotate Right Decimal)
 */
static int test_ed_rrd(void)
{
    printf("Test 47: ED RRD (0xED 0x67)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // A = 0xAB, (HL) = 0xCD
    // After RRD: A = 0xAD, (HL) = 0xCB
    z80_set_register(z80, "A", 0xAB);
    z80_set_register(z80, "H", 0x00);
    z80_set_register(z80, "L", 0x20);
    memory.memory[0x0020] = 0xCD;

    memory.memory[0x0000] = 0xED;
    memory.memory[0x0001] = 0x67; // RRD

    z80_execute_instruction(z80);

    uint8_t a = z80_get_register(z80, "A");
    uint8_t mem_val = memory.memory[0x0020];
    TEST_ASSERT_EQ(0xAD, a, "A should be 0xAD after RRD");
    TEST_ASSERT_EQ(0xCB, mem_val, "(HL) should be 0xCB after RRD");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Test 48: ED prefix - RLD (Rotate Left Decimal)
 */
static int test_ed_rld(void)
{
    printf("Test 48: ED RLD (0xED 0x6F)...\n");

    z80_emulator_t *z80 = z80_init();
    TEST_ASSERT(z80 != NULL, "Z80 initialization failed");

    mock_memory_t memory = {0};
    z80_set_memory_callbacks(z80, mock_read_memory, mock_write_memory, &memory);

    // A = 0xAB, (HL) = 0xCD
    // After RLD: A = 0xAC, (HL) = 0xDB
    z80_set_register(z80, "A", 0xAB);
    z80_set_register(z80, "H", 0x00);
    z80_set_register(z80, "L", 0x20);
    memory.memory[0x0020] = 0xCD;

    memory.memory[0x0000] = 0xED;
    memory.memory[0x0001] = 0x6F; // RLD

    z80_execute_instruction(z80);

    uint8_t a = z80_get_register(z80, "A");
    uint8_t mem_val = memory.memory[0x0020];
    TEST_ASSERT_EQ(0xAC, a, "A should be 0xAC after RLD");
    TEST_ASSERT_EQ(0xDB, mem_val, "(HL) should be 0xDB after RLD");

    z80_cleanup(z80);
    test_count_passed++;
    printf("  PASS\n");
    return 1;
}

/**
 * Main test runner
 */
int main(void)
{
    printf("=== Z80 Emulator Test Suite ===\n\n");

    test_nop();
    test_ld_b_n();
    test_ld_bc_nn();
    test_inc_b();
    test_dec_b();
    test_ld_a_n();
    test_add_a_b();
    test_sub_a_b();
    test_cp_a_b();
    test_ld_hl_a();
    test_ld_a_hl();
    test_ld_bc_a();
    test_inc_bc();
    test_rlca();
    test_jp_nn();
    test_in_a_n();
    test_out_n_a();
    test_state_save_load();
    test_jr_n();
    test_jr_nz();
    test_jr_z();
    test_jr_nc();
    test_jr_c();
    test_ld_c_n();
    test_ld_d_n();
    test_ld_e_n();
    test_ld_h_n();
    test_ld_l_n();
    test_jp_nz_nn();
    test_jp_z_nn();
    test_jp_nc_nn();
    test_jp_c_nn();
    test_call_nn();
    test_ret();
    test_di();
    test_ei();
    test_ld_a_b();
    test_ld_b_a();
    test_ld_de_nn();
    test_ld_hl_nn();
    test_cb_rlc_b();
    test_cb_bit_d();
    test_cb_res_e();
    test_cb_set_l();
    test_ed_in_b_c();
    test_ed_out_c_a();
    test_ed_rrd();
    test_ed_rld();

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_count_passed);
    printf("Failed: %d\n", test_count_failed);

    if (test_count_failed == 0)
    {
        printf("\nAll tests passed!\n");
        return 0;
    }
    else
    {
        printf("\nSome tests failed!\n");
        return 1;
    }
}

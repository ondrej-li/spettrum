#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

// Z80 Configuration
#define Z80_CLOCK_FREQ 3500000 // 3.5 MHz
#define Z80_MAX_MEMORY 65536   // 64KB address space

// Z80 Register file
typedef struct
{
    uint16_t pc; // Program counter
    uint16_t sp; // Stack pointer
    uint16_t ix; // Index register X
    uint16_t iy; // Index register Y

    // Main registers (AF, BC, DE, HL)
    uint8_t a, f; // Accumulator and flags
    uint8_t b, c; // BC register pair
    uint8_t d, e; // DE register pair
    uint8_t h, l; // HL register pair

    // Alternate registers (AF', BC', DE', HL')
    uint8_t a_alt, f_alt;
    uint8_t b_alt, c_alt;
    uint8_t d_alt, e_alt;
    uint8_t h_alt, l_alt;

    // Special registers
    uint8_t i; // Interrupt vector
    uint8_t r; // Memory refresh

    // Interrupt/control
    uint8_t im;         // Interrupt mode (0, 1, or 2)
    uint8_t iff1, iff2; // Interrupt flip-flops
} z80_registers_t;

// Z80 Flags (F register bits)
#define Z80_FLAG_C 0x01  // Carry
#define Z80_FLAG_N 0x02  // Subtract
#define Z80_FLAG_PV 0x04 // Parity/Overflow
#define Z80_FLAG_H 0x10  // Half-carry
#define Z80_FLAG_Z 0x40  // Zero
#define Z80_FLAG_S 0x80  // Sign

// Context holder for both memory and I/O callbacks
typedef struct
{
    void *memory_data;
    void *io_data;
} z80_callback_context_t;

// Z80 Emulator state
typedef struct
{
    z80_registers_t regs;

    // Thread state
    pthread_t thread;
    volatile int running;
    volatile int paused;
    volatile int halted;
    pthread_mutex_t state_lock;
    pthread_cond_t state_cond;

    // Timing
    struct timespec last_cycle_time;
    uint64_t total_cycles;

    // I/O and memory callbacks (pluggable)
    uint8_t (*read_io)(void *user_data, uint8_t port);
    void (*write_io)(void *user_data, uint8_t port, uint8_t value);
    uint8_t (*read_memory)(void *user_data, uint16_t addr);
    void (*write_memory)(void *user_data, uint16_t addr, uint8_t value);
    void *user_data;
} z80_emulator_t;

// Global emulator instance
static z80_emulator_t *g_z80 = NULL;

/**
 * Initialize Z80 emulator
 * Returns pointer to emulator context
 */
z80_emulator_t *z80_init(void)
{
    z80_emulator_t *z80 = malloc(sizeof(z80_emulator_t));
    if (!z80)
        return NULL;

    // Initialize registers
    memset(&z80->regs, 0, sizeof(z80_registers_t));
    z80->regs.pc = 0x0000;
    z80->regs.sp = 0xFFFF;
    z80->regs.f = 0x00;
    z80->regs.im = 0;

    // Initialize state
    z80->running = 0;
    z80->paused = 0;
    z80->halted = 0;
    z80->total_cycles = 0;

    // Initialize synchronization primitives
    pthread_mutex_init(&z80->state_lock, NULL);
    pthread_cond_init(&z80->state_cond, NULL);

    // Initialize callbacks to NULL
    z80->read_io = NULL;
    z80->write_io = NULL;
    z80->read_memory = NULL;
    z80->write_memory = NULL;
    z80->user_data = NULL;

    return z80;
}

/**
 * Clean up Z80 emulator
 * Note: Memory management is external; this only cleans up the emulator structure
 */
void z80_cleanup(z80_emulator_t *z80)
{
    if (!z80)
        return;

    z80->running = 0;

    if (z80->thread)
    {
        pthread_join(z80->thread, NULL);
    }

    pthread_mutex_destroy(&z80->state_lock);
    pthread_cond_destroy(&z80->state_cond);

    // Free context if it exists
    if (z80->user_data)
    {
        free(z80->user_data);
    }

    free(z80);
}

/**
 * Set I/O callbacks for pluggable I/O
 */
void z80_set_io_callbacks(z80_emulator_t *z80,
                          uint8_t (*read_io)(void *user_data, uint8_t port),
                          void (*write_io)(void *user_data, uint8_t port, uint8_t value),
                          void *user_data)
{
    if (!z80)
        return;

    // If no context exists yet, create one
    if (!z80->user_data)
    {
        z80_callback_context_t *ctx = malloc(sizeof(z80_callback_context_t));
        if (!ctx)
            return;
        ctx->memory_data = NULL;
        ctx->io_data = user_data;
        z80->user_data = ctx;
    }
    else
    {
        // Update existing context
        z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
        ctx->io_data = user_data;
    }

    z80->read_io = read_io;
    z80->write_io = write_io;
}

/**
 * Set memory callbacks for pluggable RAM/ROM
 */
void z80_set_memory_callbacks(z80_emulator_t *z80,
                              uint8_t (*read_memory)(void *user_data, uint16_t addr),
                              void (*write_memory)(void *user_data, uint16_t addr, uint8_t value),
                              void *user_data)
{
    if (!z80)
        return;

    // If no context exists yet, create one
    if (!z80->user_data)
    {
        z80_callback_context_t *ctx = malloc(sizeof(z80_callback_context_t));
        if (!ctx)
            return;
        ctx->memory_data = user_data;
        ctx->io_data = NULL;
        z80->user_data = ctx;
    }
    else
    {
        // Update existing context
        z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
        ctx->memory_data = user_data;
    }

    z80->read_memory = read_memory;
    z80->write_memory = write_memory;
}

/**
 * Internal memory read with callback support
 */
static uint8_t z80_read_memory_internal(z80_emulator_t *z80, uint16_t addr)
{
    if (z80->read_memory)
    {
        void *mem_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            mem_data = ctx->memory_data;
        }
        return z80->read_memory(mem_data, addr);
    }
    return 0xFF; // No default memory; must use callbacks
}

/**
 * Internal memory write with callback support
 */
static void z80_write_memory_internal(z80_emulator_t *z80, uint16_t addr, uint8_t value)
{
    if (z80->write_memory)
    {
        void *mem_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            mem_data = ctx->memory_data;
        }
        z80->write_memory(mem_data, addr, value);
    }
}

/**
 * Internal I/O read with callback support
 */
static uint8_t z80_read_io_internal(z80_emulator_t *z80, uint8_t port)
{
    if (z80->read_io)
    {
        void *io_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            io_data = ctx->io_data;
        }
        return z80->read_io(io_data, port);
    }
    return 0xFF; // Default: no device
}

/**
 * Internal I/O write with callback support
 */
static void z80_write_io_internal(z80_emulator_t *z80, uint8_t port, uint8_t value)
{
    if (z80->write_io)
    {
        void *io_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            io_data = ctx->io_data;
        }
        z80->write_io(io_data, port, value);
    }
}

/**
 * Load ROM/code into memory at specified address
 * Uses write_memory callbacks; caller must implement this
 */
void z80_load_memory(z80_emulator_t *z80, uint16_t addr, const uint8_t *data, size_t size)
{
    if (!z80 || !data || !z80->write_memory)
        return;

    pthread_mutex_lock(&z80->state_lock);
    for (size_t i = 0; i < size; i++)
    {
        z80_write_memory_internal(z80, addr + i, data[i]);
    }
    pthread_mutex_unlock(&z80->state_lock);
}

/**
 * Get program counter
 */
uint16_t z80_get_pc(z80_emulator_t *z80)
{
    if (!z80)
        return 0;
    return z80->regs.pc;
}

/**
 * Set program counter
 */
void z80_set_pc(z80_emulator_t *z80, uint16_t pc)
{
    if (!z80)
        return;
    pthread_mutex_lock(&z80->state_lock);
    z80->regs.pc = pc;
    pthread_mutex_unlock(&z80->state_lock);
}

/**
 * Get total cycles executed
 */
uint64_t z80_get_cycles(z80_emulator_t *z80)
{
    if (!z80)
        return 0;
    return z80->total_cycles;
}

/**
 * Calculate half-carry flag for 8-bit addition
 */
static int calculate_half_carry_add(uint8_t a, uint8_t b)
{
    return ((a & 0x0F) + (b & 0x0F)) & 0x10;
}

/**
 * Calculate half-carry flag for 8-bit subtraction
 */
static int calculate_half_carry_sub(uint8_t a, uint8_t b)
{
    return ((a & 0x0F) - (b & 0x0F)) & 0x10;
}

/**
 * Calculate parity flag (1 if even number of set bits)
 */
static int calculate_parity(uint8_t val)
{
    int count = 0;
    for (int i = 0; i < 8; i++)
    {
        count += (val >> i) & 1;
    }
    return (count & 1) == 0; // 1 if even (parity flag set)
}

/**
 * Calculate overflow flag for 8-bit addition
 * Overflow occurs when adding two numbers with same sign produces opposite sign
 */
static int calculate_overflow_add(uint8_t a, uint8_t b, uint8_t result)
{
    // Overflow if: sign(a) == sign(b) AND sign(result) != sign(a)
    return ((a ^ result) & ~(a ^ b) & 0x80) ? 1 : 0;
}

/**
 * Calculate overflow flag for 8-bit subtraction
 * Overflow occurs when subtracting numbers with different signs produces wrong sign
 */
static int calculate_overflow_sub(uint8_t a, uint8_t b, uint8_t result)
{
    // Overflow if: sign(a) != sign(b) AND sign(result) != sign(a)
    return ((a ^ b) & (a ^ result) & 0x80) ? 1 : 0;
}

/**
 * Execute a single Z80 instruction
 * Returns number of clock cycles for this instruction
 */
static int z80_execute_instruction(z80_emulator_t *z80)
{
    uint8_t opcode = z80_read_memory_internal(z80, z80->regs.pc);
    z80->regs.pc++;
    z80->regs.r++; // Increment refresh register

    int cycles = 4; // Default cycle count
    uint8_t operand;
    uint16_t addr;
    int result;

    // CB prefix variables
    uint8_t cb_opcode;
    uint8_t reg_idx;
    uint8_t operation;
    uint8_t *reg_ptr;
    uint16_t hl_addr;
    uint8_t val;
    int bit_pos;
    int carry;
    int new_carry;
    int msb;
    int bit_set;

    switch (opcode)
    {
    // NOP
    case 0x00:
        cycles = 4;
        break;

    // LD BC,nn
    case 0x01:
        z80->regs.c = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.b = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 10;
        break;

    // LD (BC),A
    case 0x02:
        addr = (z80->regs.b << 8) | z80->regs.c;
        z80_write_memory_internal(z80, addr, z80->regs.a);
        cycles = 7;
        break;

    // INC BC
    case 0x03:
        if (++z80->regs.c == 0)
            z80->regs.b++;
        cycles = 6;
        break;

    // INC B
    case 0x04:
        result = z80->regs.b + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C); // Preserve carry
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.b, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.b == 0x7F) // Overflow from 0x7F to 0x80
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.b = (uint8_t)result;
        cycles = 4;
        break;

    // DEC B
    case 0x05:
        result = z80->regs.b - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N; // Preserve carry, set subtract flag
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.b, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.b == 0x80) // Overflow from 0x80 to 0x7F
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.b = (uint8_t)result;
        cycles = 4;
        break;

    // LD B,n
    case 0x06:
        z80->regs.b = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // RLCA
    case 0x07:
        z80->regs.f &= ~(Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_H);
        if (z80->regs.a & 0x80)
            z80->regs.f |= Z80_FLAG_C;
        z80->regs.a = (z80->regs.a << 1) | ((z80->regs.a >> 7) & 1);
        cycles = 4;
        break;

    // LD A,n
    case 0x3E:
        z80->regs.a = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // LD (HL),A
    case 0x77:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.a);
        cycles = 7;
        break;

    // ADD A,B
    case 0x80:
        result = z80->regs.a + z80->regs.b;
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, z80->regs.b))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_add(z80->regs.a, z80->regs.b, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // SUB A,B
    case 0x90:
        result = z80->regs.a - z80->regs.b;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, z80->regs.b))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, z80->regs.b, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // CP A,B
    case 0xB8:
        result = z80->regs.a - z80->regs.b;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, z80->regs.b))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, z80->regs.b, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 4;
        break;

    // JP nn
    case 0xC3:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.pc = (high << 8) | low;
        cycles = 10;
        break;
    }

    // IN A,(n)
    case 0xDB:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.a = z80_read_io_internal(z80, operand);
        cycles = 11;
        break;

    // OUT (n),A
    case 0xD3:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80_write_io_internal(z80, operand, z80->regs.a);
        cycles = 11;
        break;

    // LD C,n
    case 0x0E:
        z80->regs.c = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // LD D,n
    case 0x16:
        z80->regs.d = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // LD E,n
    case 0x1E:
        z80->regs.e = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // JR n (Relative jump)
    case 0x18:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.pc += offset;
        cycles = 12;
        break;
    }

    // JR NZ,n (Jump if not zero)
    case 0x20:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_Z) == 0)
        {
            z80->regs.pc += offset;
            cycles = 12;
        }
        else
        {
            cycles = 7;
        }
        break;
    }

    // JR Z,n (Jump if zero)
    case 0x28:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_Z)
        {
            z80->regs.pc += offset;
            cycles = 12;
        }
        else
        {
            cycles = 7;
        }
        break;
    }

    // JR NC,n (Jump if no carry)
    case 0x30:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_C) == 0)
        {
            z80->regs.pc += offset;
            cycles = 12;
        }
        else
        {
            cycles = 7;
        }
        break;
    }

    // JR C,n (Jump if carry)
    case 0x38:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_C)
        {
            z80->regs.pc += offset;
            cycles = 12;
        }
        else
        {
            cycles = 7;
        }
        break;
    }

    // LD H,n
    case 0x26:
        z80->regs.h = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // LD L,n
    case 0x2E:
        z80->regs.l = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 7;
        break;

    // DI (Disable Interrupts)
    case 0xF3:
        z80->regs.iff1 = 0;
        z80->regs.iff2 = 0;
        cycles = 4;
        break;

    // EI (Enable Interrupts)
    case 0xFB:
        z80->regs.iff1 = 1;
        z80->regs.iff2 = 1;
        cycles = 4;
        break;

    // JP NZ,nn
    case 0xC2:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_Z) == 0)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // JP Z,nn
    case 0xCA:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_Z)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // CALL nn (Call subroutine)
    case 0xCD:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        // Push return address onto stack
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = target;
        cycles = 17;
        break;
    }

    // JP NC,nn
    case 0xD2:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_C) == 0)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // JP C,nn
    case 0xDA:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_C)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // RET (Return from subroutine)
    case 0xC9:
    {
        uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80->regs.pc = (high << 8) | low;
        z80->regs.sp += 2;
        cycles = 10;
        break;
    }

    // LD DE,nn
    case 0x11:
        z80->regs.e = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.d = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 10;
        break;

    // LD HL,nn
    case 0x21:
        z80->regs.l = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.h = z80_read_memory_internal(z80, z80->regs.pc++);
        cycles = 10;
        break;

    // LD SP,nn
    case 0x31:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.sp = (high << 8) | low;
        cycles = 10;
        break;
    }

    // INC A
    case 0x3C:
        result = z80->regs.a + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.a == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // DEC A
    case 0x3D:
        result = z80->regs.a - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.a == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

        // RET NZ
    case 0xC0:
        if ((z80->regs.f & Z80_FLAG_Z) == 0)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // POP BC
    case 0xC1:
    {
        uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80->regs.c = low;
        z80->regs.b = high;
        z80->regs.sp += 2;
        cycles = 10;
        break;
    }

    // CALL NZ,nn
    case 0xC4:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if ((z80->regs.f & Z80_FLAG_Z) == 0)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // PUSH BC
    case 0xC5:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.c);
        z80_write_memory_internal(z80, z80->regs.sp + 1, z80->regs.b);
        cycles = 11;
        break;

    // RST 0H
    case 0xC7:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0000;
        cycles = 11;
        break;

    // RET Z
    case 0xC8:
        if (z80->regs.f & Z80_FLAG_Z)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // CALL Z,nn
    case 0xCC:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if (z80->regs.f & Z80_FLAG_Z)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // RST 08H
    case 0xCF:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0008;
        cycles = 11;
        break;

    // RET NC
    case 0xD0:
        if ((z80->regs.f & Z80_FLAG_C) == 0)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // POP DE
    case 0xD1:
    {
        uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80->regs.e = low;
        z80->regs.d = high;
        z80->regs.sp += 2;
        cycles = 10;
        break;
    }

    // CALL NC,nn
    case 0xD4:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if ((z80->regs.f & Z80_FLAG_C) == 0)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // PUSH DE
    case 0xD5:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.e);
        z80_write_memory_internal(z80, z80->regs.sp + 1, z80->regs.d);
        cycles = 11;
        break;

    // RST 10H
    case 0xD7:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0010;
        cycles = 11;
        break;

    // RST 18H
    case 0xDF:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0018;
        cycles = 11;
        break;

    // RET C
    case 0xD8:
        if (z80->regs.f & Z80_FLAG_C)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // EXX
    case 0xD9:
    {
        uint8_t tmp_b = z80->regs.b;
        uint8_t tmp_c = z80->regs.c;
        uint8_t tmp_d = z80->regs.d;
        uint8_t tmp_e = z80->regs.e;
        uint8_t tmp_h = z80->regs.h;
        uint8_t tmp_l = z80->regs.l;
        z80->regs.b = z80->regs.b_alt;
        z80->regs.c = z80->regs.c_alt;
        z80->regs.d = z80->regs.d_alt;
        z80->regs.e = z80->regs.e_alt;
        z80->regs.h = z80->regs.h_alt;
        z80->regs.l = z80->regs.l_alt;
        z80->regs.b_alt = tmp_b;
        z80->regs.c_alt = tmp_c;
        z80->regs.d_alt = tmp_d;
        z80->regs.e_alt = tmp_e;
        z80->regs.h_alt = tmp_h;
        z80->regs.l_alt = tmp_l;
        cycles = 4;
        break;
    }

    // CALL C,nn
    case 0xDC:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if (z80->regs.f & Z80_FLAG_C)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // RET PO
    case 0xE0:
        if ((z80->regs.f & Z80_FLAG_PV) == 0)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // JP PO,nn
    case 0xE2:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_PV) == 0)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // CALL PO,nn
    case 0xE4:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if ((z80->regs.f & Z80_FLAG_PV) == 0)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // AND n
    case 0xE6:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.a &= operand;
        z80->regs.f = Z80_FLAG_H;
        if (z80->regs.a == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if (z80->regs.a & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity(z80->regs.a))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 7;
        break;

    // RST 20H
    case 0xE7:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0020;
        cycles = 11;
        break;

    // RET PE
    case 0xE8:
        if (z80->regs.f & Z80_FLAG_PV)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;

    // JP PE,nn
    case 0xEA:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_PV)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // CALL PE,nn
    case 0xEC:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target = (high << 8) | low;
        if (z80->regs.f & Z80_FLAG_PV)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = target;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // POP HL
    case 0xE1:
    {
        uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80->regs.l = low;
        z80->regs.h = high;
        z80->regs.sp += 2;
        cycles = 10;
        break;
    }

    // EX (SP),HL
    case 0xE3:
    {
        uint8_t sp_low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t sp_high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.l);
        z80_write_memory_internal(z80, z80->regs.sp + 1, z80->regs.h);
        z80->regs.l = sp_low;
        z80->regs.h = sp_high;
        cycles = 19;
        break;
    }

    // JP (HL)
    case 0xE9:
        z80->regs.pc = (z80->regs.h << 8) | z80->regs.l;
        cycles = 4;
        break;

    // EX DE,HL
    case 0xEB:
    {
        uint8_t tmp_d = z80->regs.d;
        uint8_t tmp_e = z80->regs.e;
        z80->regs.d = z80->regs.h;
        z80->regs.e = z80->regs.l;
        z80->regs.h = tmp_d;
        z80->regs.l = tmp_e;
        cycles = 4;
        break;
    }

    // XOR n
    case 0xEE:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.a ^= operand;
        z80->regs.f = 0;
        if (z80->regs.a == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if (z80->regs.a & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity(z80->regs.a))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 7;
        break;

    // RST 28H
    case 0xEF:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0028;
        cycles = 11;
        break;

    // RET P (Return if Positive - Sign flag clear)
    case 0xF0:
    {
        if ((z80->regs.f & Z80_FLAG_S) == 0)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;
    }

    // POP AF
    case 0xF1:
    {
        uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
        uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
        z80->regs.f = low;
        z80->regs.a = high;
        z80->regs.sp += 2;
        cycles = 10;
        break;
    }

    // JP P,nn (Jump if Positive/Plus - Sign flag clear)
    case 0xF2:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if ((z80->regs.f & Z80_FLAG_S) == 0)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // CALL P,nn (Call if Positive - Sign flag clear)
    case 0xF4:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t addr = (high << 8) | low;
        if ((z80->regs.f & Z80_FLAG_S) == 0)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = addr;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // PUSH AF
    case 0xF5:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.f);
        z80_write_memory_internal(z80, z80->regs.sp + 1, z80->regs.a);
        cycles = 11;
        break;

    // OR n
    case 0xF6:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80->regs.a |= operand;
        z80->regs.f = 0;
        if (z80->regs.a == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if (z80->regs.a & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity(z80->regs.a))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 7;
        break;

    // RST 30H
    case 0xF7:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0030;
        cycles = 11;
        break;

    // RET M (Return if Minus/Negative - Sign flag set)
    case 0xF8:
    {
        if (z80->regs.f & Z80_FLAG_S)
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.pc = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 11;
        }
        else
        {
            cycles = 5;
        }
        break;
    }

    // LD SP,HL
    case 0xF9:
        z80->regs.sp = (z80->regs.h << 8) | z80->regs.l;
        cycles = 6;
        break;

    // JP M,nn (Jump if Minus/Negative - Sign flag set)
    case 0xFA:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        if (z80->regs.f & Z80_FLAG_S)
        {
            z80->regs.pc = (high << 8) | low;
        }
        cycles = 10;
        break;
    }

    // CALL M,nn (Call if Minus/Negative - Sign flag set)
    case 0xFC:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t addr = (high << 8) | low;
        if (z80->regs.f & Z80_FLAG_S)
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
            z80->regs.pc = addr;
            cycles = 17;
        }
        else
        {
            cycles = 10;
        }
        break;
    }

    // CP n
    case 0xFE:
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        result = z80->regs.a - operand;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 7;
        break;

    // RST 38H
    case 0xFF:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.pc & 0xFF);
        z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.pc >> 8) & 0xFF);
        z80->regs.pc = 0x0038;
        cycles = 11;
        break;

    // PUSH HL
    case 0xE5:
        z80->regs.sp -= 2;
        z80_write_memory_internal(z80, z80->regs.sp, z80->regs.l);
        z80_write_memory_internal(z80, z80->regs.sp + 1, z80->regs.h);
        cycles = 11;
        break;

        // PUSH AF
        // In a real Z80, this would halt the CPU until interrupt
        // For now, just skip it
        cycles = 4;
        break;

    // EX AF,AF'
    case 0x08:
    {
        uint8_t tmp_a = z80->regs.a;
        uint8_t tmp_f = z80->regs.f;
        z80->regs.a = z80->regs.a_alt;
        z80->regs.f = z80->regs.f_alt;
        z80->regs.a_alt = tmp_a;
        z80->regs.f_alt = tmp_f;
        cycles = 4;
        break;
    }

    // ADD HL,BC
    case 0x09:
    {
        uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
        uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
        uint16_t result_16 = hl + bc;
        z80->regs.h = (result_16 >> 8) & 0xFF;
        z80->regs.l = result_16 & 0xFF;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H | Z80_FLAG_C);
        if (result_16 & 0x10000)
            z80->regs.f |= Z80_FLAG_C;
        if (((hl & 0x0FFF) + (bc & 0x0FFF)) & 0x1000)
            z80->regs.f |= Z80_FLAG_H;
        cycles = 11;
        break;
    }

    // LD A,(BC)
    case 0x0A:
        addr = (z80->regs.b << 8) | z80->regs.c;
        z80->regs.a = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break;

    // DEC BC
    case 0x0B:
        if (z80->regs.c == 0)
            z80->regs.b--;
        z80->regs.c--;
        cycles = 6;
        break;

    // INC C
    case 0x0C:
        result = z80->regs.c + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.c, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.c == 0x7F) // Overflow from 0x7F to 0x80
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.c = (uint8_t)result;
        cycles = 4;
        break;

    // DEC C
    case 0x0D:
        result = z80->regs.c - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.c, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.c == 0x80) // Overflow from 0x80 to 0x7F
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.c = (uint8_t)result;
        cycles = 4;
        break;

    // RRCA
    case 0x0F:
        z80->regs.f &= ~(Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_H);
        if (z80->regs.a & 0x01)
            z80->regs.f |= Z80_FLAG_C;
        z80->regs.a = (z80->regs.a >> 1) | ((z80->regs.a & 1) << 7);
        cycles = 4;
        break;

    // LD (DE),A
    case 0x12:
        addr = (z80->regs.d << 8) | z80->regs.e;
        z80_write_memory_internal(z80, addr, z80->regs.a);
        cycles = 7;
        break;

    // INC DE
    case 0x13:
        if (++z80->regs.e == 0)
            z80->regs.d++;
        cycles = 6;
        break;

    // INC D
    case 0x14:
        result = z80->regs.d + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.d, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.d == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.d = (uint8_t)result;
        cycles = 4;
        break;

    // DEC D
    case 0x15:
        result = z80->regs.d - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.d, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.d == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.d = (uint8_t)result;
        cycles = 4;
        break;

    // RLA
    case 0x17:
    {
        int old_carry = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        int new_carry = (z80->regs.a >> 7) & 1;
        z80->regs.f &= ~(Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_H);
        z80->regs.a = (z80->regs.a << 1) | old_carry;
        if (new_carry)
            z80->regs.f |= Z80_FLAG_C;
        cycles = 4;
        break;
    }

    // ADD HL,DE
    case 0x19:
    {
        uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
        uint16_t de = (z80->regs.d << 8) | z80->regs.e;
        uint16_t result_16 = hl + de;
        z80->regs.h = (result_16 >> 8) & 0xFF;
        z80->regs.l = result_16 & 0xFF;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H | Z80_FLAG_C);
        if (result_16 & 0x10000)
            z80->regs.f |= Z80_FLAG_C;
        if (((hl & 0x0FFF) + (de & 0x0FFF)) & 0x1000)
            z80->regs.f |= Z80_FLAG_H;
        cycles = 11;
        break;
    }

    // LD A,(DE)
    case 0x1A:
        addr = (z80->regs.d << 8) | z80->regs.e;
        z80->regs.a = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break;

    // DEC DE
    case 0x1B:
        if (z80->regs.e == 0)
            z80->regs.d--;
        z80->regs.e--;
        cycles = 6;
        break;

    // INC E
    case 0x1C:
        result = z80->regs.e + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.e, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.e == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.e = (uint8_t)result;
        cycles = 4;
        break;

    // DEC E
    case 0x1D:
        result = z80->regs.e - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.e, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.e == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.e = (uint8_t)result;
        cycles = 4;
        break;

    // RRA
    case 0x1F:
    {
        int old_carry = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        int new_carry = z80->regs.a & 1;
        z80->regs.f &= ~(Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_H);
        z80->regs.a = (z80->regs.a >> 1) | (old_carry << 7);
        if (new_carry)
            z80->regs.f |= Z80_FLAG_C;
        cycles = 4;
        break;
    }

    // DJNZ (offset)
    case 0x10:
    {
        int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
        if (--z80->regs.b != 0)
        {
            z80->regs.pc += offset;
            cycles = 13;
        }
        else
        {
            cycles = 8;
        }
        break;
    }

    // INC H
    case 0x24:
        result = z80->regs.h + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.h, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.h == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.h = (uint8_t)result;
        cycles = 4;
        break;

    // DEC H
    case 0x25:
        result = z80->regs.h - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.h, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.h == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.h = (uint8_t)result;
        cycles = 4;
        break;

    // DAA
    case 0x27:
    {
        // Decimal Adjust Accumulator - proper implementation
        uint8_t correction = 0;
        int carry = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;

        if (z80->regs.f & Z80_FLAG_N)
        {
            // After subtraction
            if (z80->regs.f & Z80_FLAG_H)
                correction |= 0x06;
            if (carry)
                correction |= 0x60;
            z80->regs.a -= correction;
        }
        else
        {
            // After addition
            if ((z80->regs.a & 0x0F) > 0x09 || (z80->regs.f & Z80_FLAG_H))
                correction |= 0x06;
            if (z80->regs.a > 0x99 || carry)
            {
                correction |= 0x60;
                carry = 1;
            }
            z80->regs.a += correction;
        }

        z80->regs.f &= Z80_FLAG_N | Z80_FLAG_C;
        if (carry)
            z80->regs.f |= Z80_FLAG_C;
        if (z80->regs.a == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if (z80->regs.a & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if ((correction & 0x06) != 0)
            z80->regs.f |= Z80_FLAG_H;
        if (calculate_parity(z80->regs.a))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 4;
        break;
    }

    // CPL
    case 0x2F:
        z80->regs.a ^= 0xFF;
        z80->regs.f |= Z80_FLAG_N | Z80_FLAG_H;
        cycles = 4;
        break;

    // SCF
    case 0x37:
        z80->regs.f |= Z80_FLAG_C;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H);
        cycles = 4;
        break;

    // CCF
    case 0x3F:
        z80->regs.f ^= Z80_FLAG_C;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H);
        cycles = 4;
        break;

    // INC HL
    case 0x23:
        if (++z80->regs.l == 0)
            z80->regs.h++;
        cycles = 6;
        break;

    // LD (NN),HL
    case 0x22:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target_addr = (high << 8) | low;
        z80_write_memory_internal(z80, target_addr, z80->regs.l);
        z80_write_memory_internal(z80, target_addr + 1, z80->regs.h);
        cycles = 16;
        break;
    }

    // ADD HL,HL
    case 0x29:
    {
        uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
        uint16_t result_16 = hl + hl;
        z80->regs.h = (result_16 >> 8) & 0xFF;
        z80->regs.l = result_16 & 0xFF;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H | Z80_FLAG_C);
        if (result_16 & 0x10000)
            z80->regs.f |= Z80_FLAG_C;
        if (((hl & 0x0FFF) + (hl & 0x0FFF)) & 0x1000)
            z80->regs.f |= Z80_FLAG_H;
        cycles = 11;
        break;
    }

    // LD HL,(NN)
    case 0x2A:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t source_addr = (high << 8) | low;
        z80->regs.l = z80_read_memory_internal(z80, source_addr);
        z80->regs.h = z80_read_memory_internal(z80, source_addr + 1);
        cycles = 16;
        break;
    }

    // DEC HL
    case 0x2B:
        if (z80->regs.l == 0)
            z80->regs.h--;
        z80->regs.l--;
        cycles = 6;
        break;

    // INC L
    case 0x2C:
        result = z80->regs.l + 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.l, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.l == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.l = (uint8_t)result;
        cycles = 4;
        break;

    // DEC L
    case 0x2D:
        result = z80->regs.l - 1;
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.l, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (z80->regs.l == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.l = (uint8_t)result;
        cycles = 4;
        break;

    // LD (NN),A
    case 0x32:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t target_addr = (high << 8) | low;
        z80_write_memory_internal(z80, target_addr, z80->regs.a);
        cycles = 13;
        break;
    }

    // INC SP
    case 0x33:
        z80->regs.sp++;
        cycles = 6;
        break;

    // INC (HL)
    case 0x34:
        addr = (z80->regs.h << 8) | z80->regs.l;
        operand = z80_read_memory_internal(z80, addr);
        result = operand + 1;
        z80_write_memory_internal(z80, addr, (uint8_t)result);
        z80->regs.f = (z80->regs.f & Z80_FLAG_C);
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(operand, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (operand == 0x7F)
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 11;
        break;

    // DEC (HL)
    case 0x35:
        addr = (z80->regs.h << 8) | z80->regs.l;
        operand = z80_read_memory_internal(z80, addr);
        result = operand - 1;
        z80_write_memory_internal(z80, addr, (uint8_t)result);
        z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(operand, 1))
            z80->regs.f |= Z80_FLAG_H;
        if (operand == 0x80)
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 11;
        break;

    // LD (HL),N
    case 0x36:
        addr = (z80->regs.h << 8) | z80->regs.l;
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        z80_write_memory_internal(z80, addr, operand);
        cycles = 10;
        break;

    // DEC SP
    case 0x3B:
        z80->regs.sp--;
        cycles = 6;
        break;

    // ADD HL,SP
    case 0x39:
    {
        uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
        uint16_t sp = z80->regs.sp;
        uint16_t result_16 = hl + sp;
        z80->regs.h = (result_16 >> 8) & 0xFF;
        z80->regs.l = result_16 & 0xFF;
        z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_H | Z80_FLAG_C);
        if (result_16 & 0x10000)
            z80->regs.f |= Z80_FLAG_C;
        if (((hl & 0x0FFF) + (sp & 0x0FFF)) & 0x1000)
            z80->regs.f |= Z80_FLAG_H;
        cycles = 11;
        break;
    }

    // LD A,(NN)
    case 0x3A:
    {
        uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
        uint16_t source_addr = (high << 8) | low;
        z80->regs.a = z80_read_memory_internal(z80, source_addr);
        cycles = 13;
        break;
    }

    // ======== CB PREFIX: SHIFT, ROTATE, BIT OPERATIONS ========
    case 0xCB:
        cb_opcode = z80_read_memory_internal(z80, z80->regs.pc++);
        reg_idx = cb_opcode & 0x07;
        operation = (cb_opcode >> 3) & 0x1F;
        reg_ptr = NULL;

        // Determine target register
        switch (reg_idx)
        {
        case 0:
            reg_ptr = &z80->regs.b;
            break;
        case 1:
            reg_ptr = &z80->regs.c;
            break;
        case 2:
            reg_ptr = &z80->regs.d;
            break;
        case 3:
            reg_ptr = &z80->regs.e;
            break;
        case 4:
            reg_ptr = &z80->regs.h;
            break;
        case 5:
            reg_ptr = &z80->regs.l;
            break;
        case 6:
            reg_ptr = NULL;
            break; // (HL)
        case 7:
            reg_ptr = &z80->regs.a;
            break;
        }

        // Get value to operate on
        if (reg_ptr != NULL)
        {
            val = *reg_ptr;
            hl_addr = 0;
        }
        else
        {
            hl_addr = (z80->regs.h << 8) | z80->regs.l;
            val = z80_read_memory_internal(z80, hl_addr);
        }

        // BIT, RES, SET operations - handle separately
        if ((cb_opcode & 0xC0) == 0x40)
        {
            // 0x40-0x7F: BIT b,r (Test Bit)
            bit_pos = (cb_opcode >> 3) & 0x07;
            bit_set = (val >> bit_pos) & 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_H;
            if (!bit_set)
                z80->regs.f |= Z80_FLAG_Z | Z80_FLAG_PV; // PV=Z for BIT
            if (val & 0x80)
                z80->regs.f |= Z80_FLAG_S; // S is copy of bit 7 of tested value
            cycles = (reg_ptr != NULL) ? 8 : 12;
        }
        else if ((cb_opcode & 0xC0) == 0x80)
        {
            // 0x80-0xBF: RES b,r (Reset Bit)
            bit_pos = (cb_opcode >> 3) & 0x07;
            val &= ~(1 << bit_pos);
            cycles = (reg_ptr != NULL) ? 8 : 15;
            // Store result
            if (reg_ptr != NULL)
            {
                *reg_ptr = val;
            }
            else
            {
                z80_write_memory_internal(z80, hl_addr, val);
            }
        }
        else if ((cb_opcode & 0xC0) == 0xC0)
        {
            // 0xC0-0xFF: SET b,r (Set Bit)
            bit_pos = (cb_opcode >> 3) & 0x07;
            val |= (1 << bit_pos);
            cycles = (reg_ptr != NULL) ? 8 : 15;
            // Store result
            if (reg_ptr != NULL)
            {
                *reg_ptr = val;
            }
            else
            {
                z80_write_memory_internal(z80, hl_addr, val);
            }
        }
        else
        {
            // Shift/rotate operations
            // Execute CB operation
            if (operation < 8)
            {
                // 0x00-0x07: RLC (Rotate Left Circular)
                carry = (val >> 7) & 1;
                val = (val << 1) | carry;
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 16)
            {
                // 0x08-0x0F: RRC (Rotate Right Circular)
                carry = val & 1;
                val = (val >> 1) | (carry << 7);
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 24)
            {
                // 0x10-0x17: RL (Rotate Left through Carry)
                new_carry = (val >> 7) & 1;
                val = ((val << 1) | ((z80->regs.f & Z80_FLAG_C) ? 1 : 0)) & 0xFF;
                z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 32)
            {
                // 0x18-0x1F: RR (Rotate Right through Carry)
                new_carry = val & 1;
                val = (val >> 1) | (((z80->regs.f & Z80_FLAG_C) ? 1 : 0) << 7);
                z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 40)
            {
                // 0x20-0x27: SLA (Shift Left Arithmetic)
                carry = (val >> 7) & 1;
                val = (val << 1) & 0xFF;
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 48)
            {
                // 0x28-0x2F: SRA (Shift Right Arithmetic)
                carry = val & 1;
                msb = val & 0x80;
                val = (val >> 1) | msb;
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else if (operation < 56)
            {
                // 0x30-0x37: SLL (Shift Left Logical) / SLS
                carry = (val >> 7) & 1;
                val = ((val << 1) | 1) & 0xFF;
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }
            else
            {
                // 0x38-0x3F: SRL (Shift Right Logical)
                carry = val & 1;
                val = val >> 1;
                z80->regs.f = carry ? Z80_FLAG_C : 0;
                cycles = (reg_ptr != NULL) ? 8 : 15;
            }

            // Set flags for shift/rotate operations
            if (val == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (val & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(val))
                z80->regs.f |= Z80_FLAG_PV;

            // Store result
            if (reg_ptr != NULL)
            {
                *reg_ptr = val;
            }
            else
            {
                z80_write_memory_internal(z80, hl_addr, val);
            }
        }
        break;

    // ======== ED PREFIX: I/O AND SPECIAL OPERATIONS ========
    case 0xED:
    {
        uint8_t ed_opcode = z80_read_memory_internal(z80, z80->regs.pc++);

        switch (ed_opcode)
        {
        // 0xED 0x40-0x47: IN r,(C) - Input from port C
        case 0x40:
            z80->regs.b = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C; // Preserve C, clear others
            if (z80->regs.b == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.b & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.b))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x41:
            z80->regs.c = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.c == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.c & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.c))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x42:
            z80->regs.d = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.d == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.d & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.d))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x43:
            z80->regs.e = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.e == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.e & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.e))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x44:
            z80->regs.h = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.h == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.h & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.h))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x45:
            z80->regs.l = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.l == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.l & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.l))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;
        case 0x46:
        {
            uint8_t temp = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (temp == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (temp & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(temp))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break; // IN (C) - discard result but set flags
        }
        case 0x47:
            z80->regs.a = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.a))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 12;
            break;

        // 0xED 0x48-0x4F: OUT (C),r - Output to port C
        case 0x48:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.b);
            cycles = 12;
            break;
        case 0x49:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.c);
            cycles = 12;
            break;
        case 0x4A:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.d);
            cycles = 12;
            break;
        case 0x4B:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.e);
            cycles = 12;
            break;
        case 0x4C:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.h);
            cycles = 12;
            break;
        case 0x4D:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.l);
            cycles = 12;
            break;
        case 0x4E:
            z80_write_io_internal(z80, z80->regs.c, 0);
            cycles = 12;
            break; // OUT (C),0
        case 0x4F:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.a);
            cycles = 12;
            break;

        // 0xED 0x50: LD (C), B (undocumented - write B to port C)
        case 0x50:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.b);
            cycles = 12;
            break;

        // 0xED 0x51: LD (C), C (undocumented - write C to port C)
        case 0x51:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.c);
            cycles = 12;
            break;

        // 0xED 0x52: SBC HL,DE - Subtract DE from HL with carry
        case 0x52:
        {
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            int hl_val = (z80->regs.h << 8) | z80->regs.l;
            int de_val = (z80->regs.d << 8) | z80->regs.e;
            int result16 = hl_val - de_val - carry_in;
            z80->regs.f = Z80_FLAG_N;
            if (result16 < 0)
                z80->regs.f |= Z80_FLAG_C;
            if ((result16 & 0xFFFF) == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (((result16 >> 15) & 1))
                z80->regs.f |= Z80_FLAG_S;
            if (((hl_val & 0xFFF) - (de_val & 0xFFF) - carry_in) < 0)
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.h = ((result16 >> 8) & 0xFF);
            z80->regs.l = (result16 & 0xFF);
            cycles = 15;
            break;
        }

        // 0xED 0x53: LD (nn),DE
        case 0x53:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t dest_addr = (high << 8) | low;
            z80_write_memory_internal(z80, dest_addr, z80->regs.e);
            z80_write_memory_internal(z80, dest_addr + 1, z80->regs.d);
            cycles = 20;
            break;
        }

        // 0xED 0x56: IM 1 - Set interrupt mode 1
        case 0x56:
            z80->regs.im = 1;
            cycles = 8;
            break;

        // 0xED 0x57: LD A,I - Load interrupt vector to A
        case 0x57:
            z80->regs.a = z80->regs.i;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (z80->regs.iff2)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 9;
            break;

        // 0xED 0x58: LD (C), E (undocumented - write E to port C)
        case 0x58:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.e);
            cycles = 12;
            break;

        // 0xED 0x59: LD (C), L (undocumented - write L to port C)
        case 0x59:
            z80_write_io_internal(z80, z80->regs.c, z80->regs.l);
            cycles = 12;
            break;

        // 0xED 0x5F: LD A,R (Load refresh register to A)
        case 0x5F:
            z80->regs.a = z80->regs.r;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (z80->regs.iff2)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 9;
            break;

        // 0xED 0x5A: ADC HL,DE
        case 0x5A:
        {
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            int hl_val = (z80->regs.h << 8) | z80->regs.l;
            int de_val = (z80->regs.d << 8) | z80->regs.e;
            int result16 = hl_val + de_val + carry_in;
            z80->regs.f = (result16 > 0xFFFF) ? Z80_FLAG_C : 0;
            if ((result16 & 0xFFFF) == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (((result16 >> 15) & 1))
                z80->regs.f |= Z80_FLAG_S;
            if (((hl_val & 0xFFF) + (de_val & 0xFFF) + carry_in) > 0xFFF)
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.h = ((result16 >> 8) & 0xFF);
            z80->regs.l = (result16 & 0xFF);
            cycles = 15;
            break;
        }

        // 0xED 0x5B: LD DE,(nn)
        case 0x5B:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t source_addr = (high << 8) | low;
            z80->regs.e = z80_read_memory_internal(z80, source_addr);
            z80->regs.d = z80_read_memory_internal(z80, source_addr + 1);
            cycles = 20;
            break;
        }

        // 0xED 0x5C: LD HL,(nn)
        case 0x5C:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t source_addr = (high << 8) | low;
            z80->regs.l = z80_read_memory_internal(z80, source_addr);
            z80->regs.h = z80_read_memory_internal(z80, source_addr + 1);
            cycles = 20;
            break;
        }

        // 0xED 0x5E: IM 2 - Set interrupt mode 2
        case 0x5E:
            z80->regs.im = 2;
            cycles = 8;
            break;

        // 0xED 0x6A: ADC HL,HL (Add HL to HL with carry)
        case 0x6A:
        {
            uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            int result16 = hl + hl + carry_in;
            z80->regs.f = (result16 > 0xFFFF) ? Z80_FLAG_C : 0;
            if ((result16 & 0xFFFF) == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (((result16 >> 15) & 1))
                z80->regs.f |= Z80_FLAG_S;
            if (((hl & 0xFFF) + (hl & 0xFFF) + carry_in) > 0xFFF)
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.h = ((result16 >> 8) & 0xFF);
            z80->regs.l = (result16 & 0xFF);
            cycles = 15;
            break;
        }

        // 0xED 0x67: RRD - Rotate right decimal
        case 0x67:
        {
            uint16_t hl_addr_val = (z80->regs.h << 8) | z80->regs.l;
            uint8_t mem_val = z80_read_memory_internal(z80, hl_addr_val);
            uint8_t a_low = z80->regs.a & 0x0F;
            uint8_t mem_low = mem_val & 0x0F;
            uint8_t mem_high = (mem_val >> 4) & 0x0F;

            // Rotate: A_low -> new mem_high, mem_low -> A_low, mem_high -> mem_low
            z80->regs.a = (z80->regs.a & 0xF0) | mem_low;
            mem_val = (mem_high << 4) | a_low;
            z80_write_memory_internal(z80, hl_addr_val, mem_val);

            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.a))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 18;
            break;
        }

        // 0xED 0x6F: RLD - Rotate left decimal
        case 0x6F:
        {
            uint16_t hl_addr_val = (z80->regs.h << 8) | z80->regs.l;
            uint8_t mem_val = z80_read_memory_internal(z80, hl_addr_val);
            uint8_t temp = (mem_val >> 4) & 0x0F;
            mem_val = ((mem_val << 4) & 0xF0) | (z80->regs.a & 0x0F);
            z80->regs.a = (z80->regs.a & 0xF0) | temp;
            z80_write_memory_internal(z80, hl_addr_val, mem_val);
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.a))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 18;
            break;
        }

        // 0xED 0x70: IN (C) - Input from port C, discard
        case 0x70:
        {
            z80_read_io_internal(z80, z80->regs.c);
            cycles = 12;
            break;
        }

        // 0xED 0x71: OUT (C),0 - Output 0 to port C
        case 0x71:
        {
            z80_write_io_internal(z80, z80->regs.c, 0);
            cycles = 12;
            break;
        }

        // 0xED 0x72: SBC HL,HL
        case 0x72:
        {
            uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            result = hl - hl - carry_in;
            z80->regs.f = Z80_FLAG_N;
            if ((uint16_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint16_t)result & 0x8000)
                z80->regs.f |= Z80_FLAG_S;
            if (result & 0x10000)
                z80->regs.f |= Z80_FLAG_C;
            z80->regs.h = ((uint16_t)result >> 8) & 0xFF;
            z80->regs.l = (uint16_t)result & 0xFF;
            cycles = 15;
            break;
        }

        // 0xED 0x73: LD (nn),HL
        case 0x73:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t dest_addr = (high << 8) | low;
            z80_write_memory_internal(z80, dest_addr, z80->regs.l);
            z80_write_memory_internal(z80, dest_addr + 1, z80->regs.h);
            cycles = 20;
            break;
        }

        // 0xED 0x74: NEG - Negate A (two's complement)
        case 0x74:
        {
            result = 0 - z80->regs.a;
            z80->regs.f = Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(0, z80->regs.a))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 8;
            break;
        }

        // 0xED 0x76: HLT - Halt (undocumented, acts as NOP)
        case 0x76:
        {
            cycles = 4;
            break;
        }

        // 0xED 0x7B: LD SP,(nn)
        case 0x7B:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t source_addr = (high << 8) | low;
            uint8_t sp_low = z80_read_memory_internal(z80, source_addr);
            uint8_t sp_high = z80_read_memory_internal(z80, source_addr + 1);
            z80->regs.sp = (sp_high << 8) | sp_low;
            cycles = 20;
            break;
        }

        // 0xED 0x78: IN A,(C) - Input from port C into A
        case 0x78:
        {
            result = z80_read_io_internal(z80, z80->regs.c);
            z80->regs.f &= Z80_FLAG_C;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 12;
            break;
        }

        // 0xED 0x79: OUT (C),A - Output A to port C
        case 0x79:
        {
            z80_write_io_internal(z80, z80->regs.c, z80->regs.a);
            cycles = 12;
            break;
        }

        // 0xED 0x7A: ADC HL,SP - Add SP to HL with carry
        case 0x7A:
        {
            uint16_t hl = (z80->regs.h << 8) | z80->regs.l;
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            uint32_t result_32 = (uint32_t)hl + (uint32_t)z80->regs.sp + carry_in;
            z80->regs.f &= ~(Z80_FLAG_N | Z80_FLAG_Z | Z80_FLAG_S | Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_PV);
            if ((uint16_t)result_32 == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint16_t)result_32 & 0x8000)
                z80->regs.f |= Z80_FLAG_S;
            if (result_32 & 0x10000)
                z80->regs.f |= Z80_FLAG_C;
            if ((hl & 0x0FFF) + (z80->regs.sp & 0x0FFF) + carry_in & 0x1000)
                z80->regs.f |= Z80_FLAG_H;
            if (((hl & 0x7FFF) + (z80->regs.sp & 0x7FFF) + carry_in) & 0x8000)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.h = ((uint16_t)result_32 >> 8) & 0xFF;
            z80->regs.l = (uint16_t)result_32 & 0xFF;
            cycles = 15;
            break;
        }

        // 0xED 0x7C: NEG - Negate A (same as 0x74)
        case 0x7C:
        {
            result = 0 - z80->regs.a;
            z80->regs.f = Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(0, z80->regs.a))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 8;
            break;
        }

        // ======== LDI/LDD/LDIR/LDDR FAMILY (0xAx, 0xBx) ========
        // 0xED 0xA0: LDI - Load (HL) to (DE), increment HL and DE, decrement BC
        case 0xA0:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            z80_write_memory_internal(z80, (z80->regs.d << 8) | z80->regs.e, val);
            z80->regs.l++;
            if (z80->regs.l == 0)
                z80->regs.h++;
            z80->regs.e++;
            if (z80->regs.e == 0)
                z80->regs.d++;
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            bc--;
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f &= ~(Z80_FLAG_PV | Z80_FLAG_N | Z80_FLAG_H);
            if (bc != 0)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 16;
            break;
        }

        // 0xED 0xA1: CPI - Compare (HL) with A, increment HL, decrement BC
        case 0xA1:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            result = z80->regs.a - val;
            z80->regs.l++;
            if (z80->regs.l == 0)
                z80->regs.h++;
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            bc--;
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(z80->regs.a, val))
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.f |= Z80_FLAG_N;
            if (bc != 0)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 16;
            break;
        }

        // 0xED 0xA2: INI - Input from port C to (HL), increment HL, decrement B
        case 0xA2:
        {
            uint8_t val = z80_read_io_internal(z80, z80->regs.c);
            z80_write_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l, val);
            z80->regs.l++;
            if (z80->regs.l == 0)
                z80->regs.h++;
            result = z80->regs.b - 1;
            z80->regs.b = (uint8_t)result;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            z80->regs.f |= Z80_FLAG_N;
            cycles = 16;
            break;
        }

        // 0xED 0xA3: OUTI - Output (HL) to port C, increment HL, decrement B
        case 0xA3:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            z80_write_io_internal(z80, z80->regs.c, val);
            z80->regs.l++;
            if (z80->regs.l == 0)
                z80->regs.h++;
            result = z80->regs.b - 1;
            z80->regs.b = (uint8_t)result;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            z80->regs.f |= Z80_FLAG_N;
            cycles = 16;
            break;
        }

        // 0xED 0xA8: LDD - Load (HL) to (DE), decrement HL and DE, decrement BC
        case 0xA8:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            z80_write_memory_internal(z80, (z80->regs.d << 8) | z80->regs.e, val);
            if (z80->regs.l == 0)
                z80->regs.h--;
            z80->regs.l--;
            if (z80->regs.e == 0)
                z80->regs.d--;
            z80->regs.e--;
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            bc--;
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f &= ~(Z80_FLAG_PV | Z80_FLAG_N | Z80_FLAG_H);
            if (bc != 0)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 16;
            break;
        }

        // 0xED 0xA9: CPD - Compare (HL) with A, decrement HL, decrement BC
        case 0xA9:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            result = z80->regs.a - val;
            if (z80->regs.l == 0)
                z80->regs.h--;
            z80->regs.l--;
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            bc--;
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(z80->regs.a, val))
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.f |= Z80_FLAG_N;
            if (bc != 0)
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 16;
            break;
        }

        // 0xED 0xAA: IND - Input from port C to (HL), decrement HL, decrement B
        case 0xAA:
        {
            uint8_t val = z80_read_io_internal(z80, z80->regs.c);
            z80_write_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l, val);
            if (z80->regs.l == 0)
                z80->regs.h--;
            z80->regs.l--;
            result = z80->regs.b - 1;
            z80->regs.b = (uint8_t)result;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            z80->regs.f |= Z80_FLAG_N;
            cycles = 16;
            break;
        }

        // 0xED 0xAB: OUTD - Output (HL) to port C, decrement HL, decrement B
        case 0xAB:
        {
            uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
            z80_write_io_internal(z80, z80->regs.c, val);
            if (z80->regs.l == 0)
                z80->regs.h--;
            z80->regs.l--;
            result = z80->regs.b - 1;
            z80->regs.b = (uint8_t)result;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            z80->regs.f |= Z80_FLAG_N;
            cycles = 16;
            break;
        }

        // 0xED 0xB0: LDIR - Load (HL) to (DE), increment HL and DE, decrement BC, repeat while BC != 0
        case 0xB0:
        {
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            cycles = 16; // Base cycle count for final iteration
            while (bc > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                z80_write_memory_internal(z80, (z80->regs.d << 8) | z80->regs.e, val);
                z80->regs.l++;
                if (z80->regs.l == 0)
                    z80->regs.h++;
                z80->regs.e++;
                if (z80->regs.e == 0)
                    z80->regs.d++;
                bc--;
                if (bc > 0)
                    cycles += 21; // 21 cycles per additional iteration
            }
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f &= ~(Z80_FLAG_PV | Z80_FLAG_N | Z80_FLAG_H);
            break;
        }

        // 0xED 0xB1: CPIR - Compare (HL) with A, increment HL, decrement BC, repeat while BC != 0 and Z = 0
        case 0xB1:
        {
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            cycles = 16; // Base cycle count
            while (bc > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                result = z80->regs.a - val;
                z80->regs.l++;
                if (z80->regs.l == 0)
                    z80->regs.h++;
                bc--;
                z80->regs.f = (z80->regs.f & Z80_FLAG_C);
                if ((uint8_t)result == 0)
                    z80->regs.f |= Z80_FLAG_Z;
                if ((uint8_t)result & 0x80)
                    z80->regs.f |= Z80_FLAG_S;
                if (calculate_half_carry_sub(z80->regs.a, val))
                    z80->regs.f |= Z80_FLAG_H;
                z80->regs.f |= Z80_FLAG_N;
                if (bc > 0)
                    z80->regs.f |= Z80_FLAG_PV;
                if ((uint8_t)result == 0 || bc == 0)
                    break; // Stop if found or end of block
                if (bc > 0)
                    cycles += 21;
            }
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            break;
        }

        // 0xED 0xB2: INIR - Input from port C to (HL), increment HL, decrement B, repeat while B != 0
        case 0xB2:
        {
            cycles = 16;
            while (z80->regs.b > 0)
            {
                uint8_t val = z80_read_io_internal(z80, z80->regs.c);
                z80_write_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l, val);
                z80->regs.l++;
                if (z80->regs.l == 0)
                    z80->regs.h++;
                z80->regs.b--;
                if (z80->regs.b > 0)
                    cycles += 21;
            }
            result = 0;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            z80->regs.f |= Z80_FLAG_Z | Z80_FLAG_N;
            break;
        }

        // 0xED 0xB3: OTIR - Output (HL) to port C, increment HL, decrement B, repeat while B != 0
        case 0xB3:
        {
            cycles = 16;
            while (z80->regs.b > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                z80_write_io_internal(z80, z80->regs.c, val);
                z80->regs.l++;
                if (z80->regs.l == 0)
                    z80->regs.h++;
                z80->regs.b--;
                if (z80->regs.b > 0)
                    cycles += 21;
            }
            result = 0;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            z80->regs.f |= Z80_FLAG_Z | Z80_FLAG_N;
            break;
        }

        // 0xED 0xB8: LDDR - Load (HL) to (DE), decrement HL and DE, decrement BC, repeat while BC != 0
        case 0xB8:
        {
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            cycles = 16;
            while (bc > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                z80_write_memory_internal(z80, (z80->regs.d << 8) | z80->regs.e, val);
                if (z80->regs.l == 0)
                    z80->regs.h--;
                z80->regs.l--;
                if (z80->regs.e == 0)
                    z80->regs.d--;
                z80->regs.e--;
                bc--;
                if (bc > 0)
                    cycles += 21;
            }
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            z80->regs.f &= ~(Z80_FLAG_PV | Z80_FLAG_N | Z80_FLAG_H);
            break;
        }

        // 0xED 0xB9: CPDR - Compare (HL) with A, decrement HL, decrement BC, repeat while BC != 0 and Z = 0
        case 0xB9:
        {
            uint16_t bc = (z80->regs.b << 8) | z80->regs.c;
            cycles = 16;
            while (bc > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                result = z80->regs.a - val;
                if (z80->regs.l == 0)
                    z80->regs.h--;
                z80->regs.l--;
                bc--;
                z80->regs.f = (z80->regs.f & Z80_FLAG_C);
                if ((uint8_t)result == 0)
                    z80->regs.f |= Z80_FLAG_Z;
                if ((uint8_t)result & 0x80)
                    z80->regs.f |= Z80_FLAG_S;
                if (calculate_half_carry_sub(z80->regs.a, val))
                    z80->regs.f |= Z80_FLAG_H;
                z80->regs.f |= Z80_FLAG_N;
                if (bc > 0)
                    z80->regs.f |= Z80_FLAG_PV;
                if ((uint8_t)result == 0 || bc == 0)
                    break;
                if (bc > 0)
                    cycles += 21;
            }
            z80->regs.b = (bc >> 8) & 0xFF;
            z80->regs.c = bc & 0xFF;
            break;
        }

        // 0xED 0xBA: INDR - Input from port C to (HL), decrement HL, decrement B, repeat while B != 0
        case 0xBA:
        {
            cycles = 16;
            while (z80->regs.b > 0)
            {
                uint8_t val = z80_read_io_internal(z80, z80->regs.c);
                z80_write_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l, val);
                if (z80->regs.l == 0)
                    z80->regs.h--;
                z80->regs.l--;
                z80->regs.b--;
                if (z80->regs.b > 0)
                    cycles += 21;
            }
            result = 0;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            z80->regs.f |= Z80_FLAG_Z | Z80_FLAG_N;
            break;
        }

        // 0xED 0xBB: OTDR - Output (HL) to port C, decrement HL, decrement B, repeat while B != 0
        case 0xBB:
        {
            cycles = 16;
            while (z80->regs.b > 0)
            {
                uint8_t val = z80_read_memory_internal(z80, (z80->regs.h << 8) | z80->regs.l);
                z80_write_io_internal(z80, z80->regs.c, val);
                if (z80->regs.l == 0)
                    z80->regs.h--;
                z80->regs.l--;
                z80->regs.b--;
                if (z80->regs.b > 0)
                    cycles += 21;
            }
            result = 0;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            z80->regs.f |= Z80_FLAG_Z | Z80_FLAG_N;
            break;
        }

        // Default ED instruction - unknown
        default:
        {
            fprintf(stderr, "FATAL: Unimplemented Z80 instruction ED 0x%02X at PC 0x%04X\n", ed_opcode, z80->regs.pc - 2);
            fprintf(stderr, "Instruction decode failed. Terminating emulation.\n");
            exit(EXIT_FAILURE);
        }
        }
        break;
    }

    // ======== FD PREFIX: IY INDEX REGISTER OPERATIONS ========
    case 0xFD:
    {
        uint8_t fd_opcode = z80_read_memory_internal(z80, z80->regs.pc++);

        switch (fd_opcode)
        {
        // FD 0x21: LD IY,nn
        case 0x21:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.iy = (high << 8) | low;
            cycles = 14;
            break;
        }

        // FD 0x22: LD (nn),IY
        case 0x22:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t dest_addr = (high << 8) | low;
            z80_write_memory_internal(z80, dest_addr, z80->regs.iy & 0xFF);
            z80_write_memory_internal(z80, dest_addr + 1, (z80->regs.iy >> 8) & 0xFF);
            cycles = 20;
            break;
        }

        // FD 0x23: INC IY
        case 0x23:
            z80->regs.iy++;
            cycles = 10;
            break;

        // FD 0x24: INC IYH
        case 0x24:
        {
            uint8_t iyh = (z80->regs.iy >> 8) & 0xFF;
            result = iyh + 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(iyh, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (iyh == 0x7F)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.iy = (z80->regs.iy & 0x00FF) | ((uint8_t)result << 8);
            cycles = 8;
            break;
        }

        // FD 0x25: DEC IYH
        case 0x25:
        {
            uint8_t iyh = (z80->regs.iy >> 8) & 0xFF;
            result = iyh - 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(iyh, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (iyh == 0x80)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.iy = (z80->regs.iy & 0x00FF) | ((uint8_t)result << 8);
            cycles = 8;
            break;
        }

        // FD 0x26: LD IYH,n
        case 0x26:
        {
            uint8_t n = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.iy = (z80->regs.iy & 0x00FF) | (n << 8);
            cycles = 11;
            break;
        }

        // FD 0x2C: INC IYL
        case 0x2C:
        {
            uint8_t iyl = z80->regs.iy & 0xFF;
            result = iyl + 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(iyl, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (iyl == 0x7F)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.iy = (z80->regs.iy & 0xFF00) | (uint8_t)result;
            cycles = 8;
            break;
        }

        // FD 0x2D: DEC IYL
        case 0x2D:
        {
            uint8_t iyl = z80->regs.iy & 0xFF;
            result = iyl - 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(iyl, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (iyl == 0x80)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.iy = (z80->regs.iy & 0xFF00) | (uint8_t)result;
            cycles = 8;
            break;
        }

        // FD 0x2E: LD IYL,n
        case 0x2E:
        {
            uint8_t n = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.iy = (z80->regs.iy & 0xFF00) | n;
            cycles = 11;
            break;
        }

        // FD 0x2A: LD IY,(nn)
        case 0x2A:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t source_addr = (high << 8) | low;
            uint8_t iy_low = z80_read_memory_internal(z80, source_addr);
            uint8_t iy_high = z80_read_memory_internal(z80, source_addr + 1);
            z80->regs.iy = (iy_high << 8) | iy_low;
            cycles = 20;
            break;
        }

        // FD 0x29: ADD IY,IY
        case 0x29:
        {
            uint32_t result_32 = (uint32_t)z80->regs.iy + (uint32_t)z80->regs.iy;
            z80->regs.f = (z80->regs.f & ~(Z80_FLAG_N | Z80_FLAG_C | Z80_FLAG_H));
            if (result_32 & 0x10000)
                z80->regs.f |= Z80_FLAG_C;
            if ((z80->regs.iy & 0x0FFF) + (z80->regs.iy & 0x0FFF) & 0x1000)
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.iy = (uint16_t)result_32;
            cycles = 15;
            break;
        }

        // FD 0x2B: DEC IY
        case 0x2B:
            z80->regs.iy--;
            cycles = 10;
            break;

        // FD 0x34: INC (IY+d)
        case 0x34:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = val + 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(val, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80_write_memory_internal(z80, addr, (uint8_t)result);
            cycles = 23;
            break;
        }

        // FD 0x35: DEC (IY+d)
        case 0x35:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = val - 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(val, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80_write_memory_internal(z80, addr, (uint8_t)result);
            cycles = 23;
            break;
        }

        // FD 0x36: LD (IY+d),n
        case 0x36:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint8_t n = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, n);
            cycles = 19;
            break;
        }

        // FD 0xCB: Bit operations on (IY+d)
        case 0xCB:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint8_t bit_opcode = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);

            // BIT, RES, SET operations - handle separately
            if ((bit_opcode & 0xC0) == 0x40)
            {
                // 0x40-0x7F: BIT b,(IY+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                bit_set = (val >> bit_pos) & 1;
                z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_H;
                if (!bit_set)
                    z80->regs.f |= Z80_FLAG_Z;
                cycles = 20;
            }
            else if ((bit_opcode & 0xC0) == 0x80)
            {
                // 0x80-0xBF: RES b,(IY+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                val &= ~(1 << bit_pos);
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            else if ((bit_opcode & 0xC0) == 0xC0)
            {
                // 0xC0-0xFF: SET b,(IY+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                val |= (1 << bit_pos);
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            else
            {
                // Shift/rotate operations on (IY+d)
                uint8_t operation = (bit_opcode >> 3) & 0x1F;
                if (operation < 8)
                {
                    // 0x00-0x07: RLC (Rotate Left Circular)
                    carry = (val >> 7) & 1;
                    val = (val << 1) | carry;
                    z80->regs.f = carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 16)
                {
                    // 0x08-0x0F: RRC (Rotate Right Circular)
                    carry = val & 1;
                    val = (val >> 1) | (carry << 7);
                    z80->regs.f = carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 24)
                {
                    // 0x10-0x17: RL (Rotate Left through Carry)
                    new_carry = (val >> 7) & 1;
                    val = ((val << 1) | ((z80->regs.f & Z80_FLAG_C) ? 1 : 0)) & 0xFF;
                    z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 32)
                {
                    // 0x18-0x1F: RR (Rotate Right through Carry)
                    new_carry = val & 1;
                    val = (val >> 1) | (((z80->regs.f & Z80_FLAG_C) ? 1 : 0) << 7);
                    z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                }
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            break;
        }

        // FD 0x46: LD B,(IY+d)
        case 0x46:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.b = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x4E: LD C,(IY+d)
        case 0x4E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.c = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x56: LD D,(IY+d)
        case 0x56:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.d = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x5E: LD E,(IY+d)
        case 0x5E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.e = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x66: LD H,(IY+d)
        case 0x66:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.h = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x6E: LD L,(IY+d)
        case 0x6E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.l = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x70: LD (IY+d),B
        case 0x70:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.b);
            cycles = 19;
            break;
        }

        // FD 0x71: LD (IY+d),C
        case 0x71:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.c);
            cycles = 19;
            break;
        }

        // FD 0x72: LD (IY+d),D
        case 0x72:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.d);
            cycles = 19;
            break;
        }

        // FD 0x73: LD (IY+d),E
        case 0x73:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.e);
            cycles = 19;
            break;
        }

        // FD 0x74: LD (IY+d),H
        case 0x74:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.h);
            cycles = 19;
            break;
        }

        // FD 0x75: LD (IY+d),L
        case 0x75:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.l);
            cycles = 19;
            break;
        }

        // FD 0x77: LD (IY+d),A
        case 0x77:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80_write_memory_internal(z80, addr, z80->regs.a);
            cycles = 19;
            break;
        }

        // FD 0x7E: LD A,(IY+d)
        case 0x7E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            z80->regs.a = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // FD 0x86: ADD A,(IY+d)
        case 0x86:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a + val;
            z80->regs.f = 0;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(z80->regs.a, val))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0x8E: ADC A,(IY+d)
        case 0x8E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            result = z80->regs.a + val + carry_in;
            z80->regs.f = 0;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(z80->regs.a, val + carry_in))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0x96: SUB A,(IY+d)
        case 0x96:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a - val;
            z80->regs.f = Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(z80->regs.a, val))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0x9E: SBC A,(IY+d)
        case 0x9E:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            int carry_in = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
            result = z80->regs.a - val - carry_in;
            z80->regs.f = Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(z80->regs.a, val + carry_in))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0xA6: AND A,(IY+d)
        case 0xA6:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a & val;
            z80->regs.f = Z80_FLAG_H;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0xAE: XOR A,(IY+d)
        case 0xAE:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a ^ val;
            z80->regs.f = 0;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0xB6: OR A,(IY+d)
        case 0xB6:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a | val;
            z80->regs.f = 0;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.a = (uint8_t)result;
            cycles = 19;
            break;
        }

        // FD 0xBE: CP A,(IY+d)
        case 0xBE:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.iy + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);
            result = z80->regs.a - val;
            z80->regs.f = Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(z80->regs.a, val))
                z80->regs.f |= Z80_FLAG_H;
            if (result & 0x100)
                z80->regs.f |= Z80_FLAG_C;
            if (calculate_parity((uint8_t)result))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 19;
            break;
        }

        // FD 0xE1: POP IY
        case 0xE1:
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.iy = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 14;
            break;
        }

        // FD 0xE3: EX (SP),IY
        case 0xE3:
        {
            uint8_t sp_low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t sp_high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.iy & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.iy >> 8) & 0xFF);
            z80->regs.iy = (sp_high << 8) | sp_low;
            cycles = 23;
            break;
        }

        // FD 0xE5: PUSH IY
        case 0xE5:
        {
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.iy & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.iy >> 8) & 0xFF);
            cycles = 15;
            break;
        }

        // FD 0xE9: JP (IY)
        case 0xE9:
        {
            z80->regs.pc = z80->regs.iy;
            cycles = 8;
            break;
        }

        // FD 0xF9: LD SP,IY
        case 0xF9:
        {
            z80->regs.sp = z80->regs.iy;
            cycles = 10;
            break;
        }

        // FD 0x54: LD D,IYH - Load D from high byte of IY
        case 0x54:
            z80->regs.d = (z80->regs.iy >> 8) & 0xFF;
            cycles = 8;
            break;

        // FD 0x5D: LD E,IYL - Load E from low byte of IY
        case 0x5D:
            z80->regs.e = z80->regs.iy & 0xFF;
            cycles = 8;
            break;

        // FD 0x7C: LD A,IYH - Load A from high byte of IY
        case 0x7C:
            z80->regs.a = (z80->regs.iy >> 8) & 0xFF;
            cycles = 8;
            break;

        // FD 0x7D: LD A,IYL - Load A from low byte of IY
        case 0x7D:
            z80->regs.a = z80->regs.iy & 0xFF;
            cycles = 8;
            break;

        // Default FD instruction - not yet implemented
        default:
        {
            fprintf(stderr, "FATAL: Unimplemented Z80 instruction FD 0x%02X at PC 0x%04X\n", fd_opcode, z80->regs.pc - 2);
            fprintf(stderr, "Instruction decode failed. Terminating emulation.\n");
            exit(EXIT_FAILURE);
        }
        }
        break;
    }

    // ======== DD PREFIX: IX INDEX REGISTER OPERATIONS ========
    case 0xDD:
    {
        uint8_t dd_opcode = z80_read_memory_internal(z80, z80->regs.pc++);

        switch (dd_opcode)
        {
        // DD 0x21: LD IX,nn
        case 0x21:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.ix = (high << 8) | low;
            cycles = 14;
            break;
        }

        // DD 0x22: LD (nn),IX
        case 0x22:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t dest_addr = (high << 8) | low;
            z80_write_memory_internal(z80, dest_addr, z80->regs.ix & 0xFF);
            z80_write_memory_internal(z80, dest_addr + 1, (z80->regs.ix >> 8) & 0xFF);
            cycles = 20;
            break;
        }

        // DD 0x23: INC IX
        case 0x23:
            z80->regs.ix++;
            cycles = 10;
            break;

        // DD 0x24: INC IXH
        case 0x24:
        {
            uint8_t ixh = (z80->regs.ix >> 8) & 0xFF;
            result = ixh + 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(ixh, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (ixh == 0x7F)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.ix = (z80->regs.ix & 0x00FF) | ((uint8_t)result << 8);
            cycles = 8;
            break;
        }

        // DD 0x25: DEC IXH
        case 0x25:
        {
            uint8_t ixh = (z80->regs.ix >> 8) & 0xFF;
            result = ixh - 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(ixh, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (ixh == 0x80)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.ix = (z80->regs.ix & 0x00FF) | ((uint8_t)result << 8);
            cycles = 8;
            break;
        }

        // DD 0x26: LD IXH,n
        case 0x26:
        {
            uint8_t n = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.ix = (z80->regs.ix & 0x00FF) | (n << 8);
            cycles = 11;
            break;
        }

        // DD 0x2C: INC IXL
        case 0x2C:
        {
            uint8_t ixl = z80->regs.ix & 0xFF;
            result = ixl + 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C);
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_add(ixl, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (ixl == 0x7F)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.ix = (z80->regs.ix & 0xFF00) | (uint8_t)result;
            cycles = 8;
            break;
        }

        // DD 0x2D: DEC IXL
        case 0x2D:
        {
            uint8_t ixl = z80->regs.ix & 0xFF;
            result = ixl - 1;
            z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_N;
            if ((uint8_t)result == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if ((uint8_t)result & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_half_carry_sub(ixl, 1))
                z80->regs.f |= Z80_FLAG_H;
            if (ixl == 0x80)
                z80->regs.f |= Z80_FLAG_PV;
            z80->regs.ix = (z80->regs.ix & 0xFF00) | (uint8_t)result;
            cycles = 8;
            break;
        }

        // DD 0x2E: LD IXL,n
        case 0x2E:
        {
            uint8_t n = z80_read_memory_internal(z80, z80->regs.pc++);
            z80->regs.ix = (z80->regs.ix & 0xFF00) | n;
            cycles = 11;
            break;
        }

        // DD 0x7C: LD A,IXH - Load A from high byte of IX
        case 0x7C:
            z80->regs.a = (z80->regs.ix >> 8) & 0xFF;
            cycles = 8;
            break;

        // DD 0x5C: LD E,IXH - Load E from high byte of IX
        case 0x5C:
            z80->regs.e = (z80->regs.ix >> 8) & 0xFF;
            cycles = 8;
            break;

        // DD 0x67: LD IXH,A - Load high byte of IX from A
        case 0x67:
            z80->regs.ix = (z80->regs.ix & 0x00FF) | (z80->regs.a << 8);
            cycles = 8;
            break;

        // DD 0x6F: LD IXL,A - Load low byte of IX from A
        case 0x6F:
            z80->regs.ix = (z80->regs.ix & 0xFF00) | z80->regs.a;
            cycles = 8;
            break;

        // DD 0x7D: LD A,IXL - Load A from low byte of IX
        case 0x7D:
            z80->regs.a = z80->regs.ix & 0xFF;
            cycles = 8;
            break;

        // DD 0x7E: LD A,(IX+d) - Load A from memory at IX+displacement
        case 0x7E:
        {
            int8_t displacement = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.ix + displacement;
            z80->regs.a = z80_read_memory_internal(z80, addr);
            cycles = 19;
            break;
        }

        // DD 0x2A: LD IX,(nn)
        case 0x2A:
        {
            uint16_t low = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t high = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t source_addr = (high << 8) | low;
            uint8_t ix_low = z80_read_memory_internal(z80, source_addr);
            uint8_t ix_high = z80_read_memory_internal(z80, source_addr + 1);
            z80->regs.ix = (ix_high << 8) | ix_low;
            cycles = 20;
            break;
        }

        // DD 0x29: ADD IX,IX
        case 0x29:
        {
            uint32_t result_32 = (uint32_t)z80->regs.ix + (uint32_t)z80->regs.ix;
            z80->regs.f = (z80->regs.f & ~(Z80_FLAG_N | Z80_FLAG_C | Z80_FLAG_H));
            if (result_32 & 0x10000)
                z80->regs.f |= Z80_FLAG_C;
            if ((z80->regs.ix & 0x0FFF) + (z80->regs.ix & 0x0FFF) & 0x1000)
                z80->regs.f |= Z80_FLAG_H;
            z80->regs.ix = (uint16_t)result_32;
            cycles = 15;
            break;
        }

        // DD 0x2B: DEC IX
        case 0x2B:
            z80->regs.ix--;
            cycles = 10;
            break;

        // DD 0x36: LD (IX+d),n - Load immediate value into memory at IX+displacement
        case 0x36:
        {
            int8_t displacement = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint8_t value = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t target_addr = z80->regs.ix + displacement;
            z80_write_memory_internal(z80, target_addr, value);
            cycles = 19;
            break;
        }

        // DD 0x77: LD (IX+d),A - Load memory at IX+displacement with A
        case 0x77:
        {
            int8_t displacement = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t target_addr = z80->regs.ix + displacement;
            z80_write_memory_internal(z80, target_addr, z80->regs.a);
            cycles = 19;
            break;
        }

        // DD 0xE1: POP IX - Pop IX from stack
        case 0xE1:
        {
            uint8_t low = z80_read_memory_internal(z80, z80->regs.sp);
            uint8_t high = z80_read_memory_internal(z80, z80->regs.sp + 1);
            z80->regs.ix = (high << 8) | low;
            z80->regs.sp += 2;
            cycles = 14;
            break;
        }

        // DD 0xE5: PUSH IX - Push IX onto stack
        case 0xE5:
            z80->regs.sp -= 2;
            z80_write_memory_internal(z80, z80->regs.sp, z80->regs.ix & 0xFF);
            z80_write_memory_internal(z80, z80->regs.sp + 1, (z80->regs.ix >> 8) & 0xFF);
            cycles = 15;
            break;

        // DD 0xE9: JP (IX) - Jump to address in IX register
        case 0xE9:
            z80->regs.pc = z80->regs.ix;
            cycles = 8;
            break;

        // DD 0xCB: Bit operations on (IX+d)
        case 0xCB:
        {
            int8_t offset = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint8_t bit_opcode = z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.ix + offset;
            uint8_t val = z80_read_memory_internal(z80, addr);

            // BIT, RES, SET operations - handle separately
            if ((bit_opcode & 0xC0) == 0x40)
            {
                // 0x40-0x7F: BIT b,(IX+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                bit_set = (val >> bit_pos) & 1;
                z80->regs.f = (z80->regs.f & Z80_FLAG_C) | Z80_FLAG_H;
                if (!bit_set)
                    z80->regs.f |= Z80_FLAG_Z;
                cycles = 20;
            }
            else if ((bit_opcode & 0xC0) == 0x80)
            {
                // 0x80-0xBF: RES b,(IX+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                val &= ~(1 << bit_pos);
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            else if ((bit_opcode & 0xC0) == 0xC0)
            {
                // 0xC0-0xFF: SET b,(IX+d)
                bit_pos = (bit_opcode >> 3) & 0x07;
                val |= (1 << bit_pos);
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            else
            {
                // Shift/rotate operations on (IX+d)
                uint8_t operation = (bit_opcode >> 3) & 0x1F;
                if (operation < 8)
                {
                    // 0x00-0x07: RLC (Rotate Left Circular)
                    carry = (val >> 7) & 1;
                    val = (val << 1) | carry;
                    z80->regs.f = carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 16)
                {
                    // 0x08-0x0F: RRC (Rotate Right Circular)
                    carry = val & 1;
                    val = (val >> 1) | (carry << 7);
                    z80->regs.f = carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 24)
                {
                    // 0x10-0x17: RL (Rotate Left through Carry)
                    new_carry = (val >> 7) & 1;
                    val = ((val << 1) | ((z80->regs.f & Z80_FLAG_C) ? 1 : 0)) & 0xFF;
                    z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                }
                else if (operation < 32)
                {
                    // 0x18-0x1F: RR (Rotate Right through Carry)
                    new_carry = val & 1;
                    val = (val >> 1) | (((z80->regs.f & Z80_FLAG_C) ? 1 : 0) << 7);
                    z80->regs.f = new_carry ? Z80_FLAG_C : 0;
                }
                z80_write_memory_internal(z80, addr, val);
                cycles = 23;
            }
            break;
        }

        // DD 0xB6: OR (IX+d) - Bitwise OR with value at IX+displacement
        case 0xB6:
        {
            int8_t displacement = (int8_t)z80_read_memory_internal(z80, z80->regs.pc++);
            uint16_t addr = z80->regs.ix + displacement;
            uint8_t value = z80_read_memory_internal(z80, addr);
            z80->regs.a |= value;
            z80->regs.f = 0;
            if (z80->regs.a == 0)
                z80->regs.f |= Z80_FLAG_Z;
            if (z80->regs.a & 0x80)
                z80->regs.f |= Z80_FLAG_S;
            if (calculate_parity(z80->regs.a))
                z80->regs.f |= Z80_FLAG_PV;
            cycles = 19;
            break;
        }

        // Default DD instruction - not yet implemented
        default:
        {
            fprintf(stderr, "FATAL: Unimplemented Z80 instruction DD 0x%02X at PC 0x%04X\n", dd_opcode, z80->regs.pc - 2);
            fprintf(stderr, "Instruction decode failed. Terminating emulation.\n");
            exit(EXIT_FAILURE);
        }
        }
        break;
    }

    // Comprehensive LD r, r' (0x40-0x7F) - Load register from register
    case 0x40:
        z80->regs.b = z80->regs.b;
        cycles = 4;
        break; // LD B,B
    case 0x41:
        z80->regs.b = z80->regs.c;
        cycles = 4;
        break; // LD B,C
    case 0x42:
        z80->regs.b = z80->regs.d;
        cycles = 4;
        break; // LD B,D
    case 0x43:
        z80->regs.b = z80->regs.e;
        cycles = 4;
        break; // LD B,E
    case 0x44:
        z80->regs.b = z80->regs.h;
        cycles = 4;
        break; // LD B,H
    case 0x45:
        z80->regs.b = z80->regs.l;
        cycles = 4;
        break; // LD B,L
    case 0x46:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.b = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD B,(HL)
    case 0x47:
        z80->regs.b = z80->regs.a;
        cycles = 4;
        break; // LD B,A

    case 0x48:
        z80->regs.c = z80->regs.b;
        cycles = 4;
        break; // LD C,B
    case 0x49:
        z80->regs.c = z80->regs.c;
        cycles = 4;
        break; // LD C,C
    case 0x4A:
        z80->regs.c = z80->regs.d;
        cycles = 4;
        break; // LD C,D
    case 0x4B:
        z80->regs.c = z80->regs.e;
        cycles = 4;
        break; // LD C,E
    case 0x4C:
        z80->regs.c = z80->regs.h;
        cycles = 4;
        break; // LD C,H
    case 0x4D:
        z80->regs.c = z80->regs.l;
        cycles = 4;
        break; // LD C,L
    case 0x4E:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.c = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD C,(HL)
    case 0x4F:
        z80->regs.c = z80->regs.a;
        cycles = 4;
        break; // LD C,A

    case 0x50:
        z80->regs.d = z80->regs.b;
        cycles = 4;
        break; // LD D,B
    case 0x51:
        z80->regs.d = z80->regs.c;
        cycles = 4;
        break; // LD D,C
    case 0x52:
        z80->regs.d = z80->regs.d;
        cycles = 4;
        break; // LD D,D
    case 0x53:
        z80->regs.d = z80->regs.e;
        cycles = 4;
        break; // LD D,E
    case 0x54:
        z80->regs.d = z80->regs.h;
        cycles = 4;
        break; // LD D,H
    case 0x55:
        z80->regs.d = z80->regs.l;
        cycles = 4;
        break; // LD D,L
    case 0x56:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.d = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD D,(HL)
    case 0x57:
        z80->regs.d = z80->regs.a;
        cycles = 4;
        break; // LD D,A

    case 0x58:
        z80->regs.e = z80->regs.b;
        cycles = 4;
        break; // LD E,B
    case 0x59:
        z80->regs.e = z80->regs.c;
        cycles = 4;
        break; // LD E,C
    case 0x5A:
        z80->regs.e = z80->regs.d;
        cycles = 4;
        break; // LD E,D
    case 0x5B:
        z80->regs.e = z80->regs.e;
        cycles = 4;
        break; // LD E,E
    case 0x5C:
        z80->regs.e = z80->regs.h;
        cycles = 4;
        break; // LD E,H
    case 0x5D:
        z80->regs.e = z80->regs.l;
        cycles = 4;
        break; // LD E,L
    case 0x5E:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.e = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD E,(HL)
    case 0x5F:
        z80->regs.e = z80->regs.a;
        cycles = 4;
        break; // LD E,A

    case 0x60:
        z80->regs.h = z80->regs.b;
        cycles = 4;
        break; // LD H,B
    case 0x61:
        z80->regs.h = z80->regs.c;
        cycles = 4;
        break; // LD H,C
    case 0x62:
        z80->regs.h = z80->regs.d;
        cycles = 4;
        break; // LD H,D
    case 0x63:
        z80->regs.h = z80->regs.e;
        cycles = 4;
        break; // LD H,E
    case 0x64:
        z80->regs.h = z80->regs.h;
        cycles = 4;
        break; // LD H,H
    case 0x65:
        z80->regs.h = z80->regs.l;
        cycles = 4;
        break; // LD H,L
    case 0x66:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.h = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD H,(HL)
    case 0x67:
        z80->regs.h = z80->regs.a;
        cycles = 4;
        break; // LD H,A

    case 0x68:
        z80->regs.l = z80->regs.b;
        cycles = 4;
        break; // LD L,B
    case 0x69:
        z80->regs.l = z80->regs.c;
        cycles = 4;
        break; // LD L,C
    case 0x6A:
        z80->regs.l = z80->regs.d;
        cycles = 4;
        break; // LD L,D
    case 0x6B:
        z80->regs.l = z80->regs.e;
        cycles = 4;
        break; // LD L,E
    case 0x6C:
        z80->regs.l = z80->regs.h;
        cycles = 4;
        break; // LD L,H
    case 0x6D:
        z80->regs.l = z80->regs.l;
        cycles = 4;
        break; // LD L,L
    case 0x6E:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.l = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD L,(HL)
    case 0x6F:
        z80->regs.l = z80->regs.a;
        cycles = 4;
        break; // LD L,A

    case 0x70:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.b);
        cycles = 7;
        break; // LD (HL),B
    case 0x71:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.c);
        cycles = 7;
        break; // LD (HL),C
    case 0x72:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.d);
        cycles = 7;
        break; // LD (HL),D
    case 0x73:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.e);
        cycles = 7;
        break; // LD (HL),E
    case 0x74:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.h);
        cycles = 7;
        break; // LD (HL),H
    case 0x75:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80_write_memory_internal(z80, addr, z80->regs.l);
        cycles = 7;
        break; // LD (HL),L
    case 0x76:
        // HALT - Stop CPU until interrupt
        z80->halted = 1;
        cycles = 4;
        break;
    case 0x78:
        z80->regs.a = z80->regs.b;
        cycles = 4;
        break; // LD A,B
    case 0x79:
        z80->regs.a = z80->regs.c;
        cycles = 4;
        break; // LD A,C
    case 0x7A:
        z80->regs.a = z80->regs.d;
        cycles = 4;
        break; // LD A,D
    case 0x7B:
        z80->regs.a = z80->regs.e;
        cycles = 4;
        break; // LD A,E
    case 0x7C:
        z80->regs.a = z80->regs.h;
        cycles = 4;
        break; // LD A,H
    case 0x7D:
        z80->regs.a = z80->regs.l;
        cycles = 4;
        break; // LD A,L
    case 0x7E:
        addr = (z80->regs.h << 8) | z80->regs.l;
        z80->regs.a = z80_read_memory_internal(z80, addr);
        cycles = 7;
        break; // LD A,(HL)
    case 0x7F:
        z80->regs.a = z80->regs.a;
        cycles = 4;
        break; // LD A,A

    // ADD A,r
    case 0x81:
        result = z80->regs.a + z80->regs.c;
        goto alu_add;
    case 0x82:
        result = z80->regs.a + z80->regs.d;
        goto alu_add;
    case 0x83:
        result = z80->regs.a + z80->regs.e;
        goto alu_add;
    case 0x84:
        result = z80->regs.a + z80->regs.h;
        goto alu_add;
    case 0x85:
        result = z80->regs.a + z80->regs.l;
        goto alu_add;
    case 0x86:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a + z80_read_memory_internal(z80, addr);
        goto alu_add;
    case 0x87:
        result = z80->regs.a + z80->regs.a;
    alu_add:
    {
        uint8_t operand = result - z80->regs.a;
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_add(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;
    }

    // ADC A,r
    case 0x88:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.b + c;
        goto alu_adc;
    }
    case 0x89:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.c + c;
        goto alu_adc;
    }
    case 0x8A:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.d + c;
        goto alu_adc;
    }
    case 0x8B:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.e + c;
        goto alu_adc;
    }
    case 0x8C:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.h + c;
        goto alu_adc;
    }
    case 0x8D:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.l + c;
        goto alu_adc;
    }
    case 0x8E:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a + z80_read_memory_internal(z80, addr) + c;
        goto alu_adc;
    }
    case 0x8F:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a + z80->regs.a + c;
    alu_adc:
    {
        uint16_t temp = result - (z80->regs.f & Z80_FLAG_C ? 1 : 0);
        uint8_t operand = temp - z80->regs.a;
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_add(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;
    }
    }

    // SUB A,r
    case 0x91:
        result = z80->regs.a - z80->regs.c;
        goto alu_sub;
    case 0x92:
        result = z80->regs.a - z80->regs.d;
        goto alu_sub;
    case 0x93:
        result = z80->regs.a - z80->regs.e;
        goto alu_sub;
    case 0x94:
        result = z80->regs.a - z80->regs.h;
        goto alu_sub;
    case 0x95:
        result = z80->regs.a - z80->regs.l;
        goto alu_sub;
    case 0x96:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a - z80_read_memory_internal(z80, addr);
        goto alu_sub;
    case 0x97:
        result = z80->regs.a - z80->regs.a;
    alu_sub:
    {
        uint8_t operand = z80->regs.a - (uint8_t)result;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;
    }

    // SBC A,r
    case 0x98:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.b - c;
        goto alu_sbc;
    }
    case 0x99:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.c - c;
        goto alu_sbc;
    }
    case 0x9A:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.d - c;
        goto alu_sbc;
    }
    case 0x9B:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.e - c;
        goto alu_sbc;
    }
    case 0x9C:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.h - c;
        goto alu_sbc;
    }
    case 0x9D:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.l - c;
        goto alu_sbc;
    }
    case 0x9E:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a - z80_read_memory_internal(z80, addr) - c;
        goto alu_sbc;
    }
    case 0x9F:
    {
        uint8_t c = (z80->regs.f & Z80_FLAG_C) ? 1 : 0;
        result = z80->regs.a - z80->regs.a - c;
    alu_sbc:
    {
        uint16_t temp = result + (z80->regs.f & Z80_FLAG_C ? 1 : 0);
        uint8_t operand = z80->regs.a - (uint8_t)temp;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;
    }
    }

    // AND A,r
    case 0xA0:
        result = z80->regs.a & z80->regs.b;
        goto alu_and;
    case 0xA1:
        result = z80->regs.a & z80->regs.c;
        goto alu_and;
    case 0xA2:
        result = z80->regs.a & z80->regs.d;
        goto alu_and;
    case 0xA3:
        result = z80->regs.a & z80->regs.e;
        goto alu_and;
    case 0xA4:
        result = z80->regs.a & z80->regs.h;
        goto alu_and;
    case 0xA5:
        result = z80->regs.a & z80->regs.l;
        goto alu_and;
    case 0xA6:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a & z80_read_memory_internal(z80, addr);
        goto alu_and;
    case 0xA7:
        result = z80->regs.a & z80->regs.a;
    alu_and:
        z80->regs.f = Z80_FLAG_H;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity((uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // XOR A,r
    case 0xA8:
        result = z80->regs.a ^ z80->regs.b;
        goto alu_xor;
    case 0xA9:
        result = z80->regs.a ^ z80->regs.c;
        goto alu_xor;
    case 0xAA:
        result = z80->regs.a ^ z80->regs.d;
        goto alu_xor;
    case 0xAB:
        result = z80->regs.a ^ z80->regs.e;
        goto alu_xor;
    case 0xAC:
        result = z80->regs.a ^ z80->regs.h;
        goto alu_xor;
    case 0xAD:
        result = z80->regs.a ^ z80->regs.l;
        goto alu_xor;
    case 0xAE:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a ^ z80_read_memory_internal(z80, addr);
        goto alu_xor;
    case 0xAF:
        result = z80->regs.a ^ z80->regs.a;
    alu_xor:
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity((uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // OR A,r
    case 0xB0:
        result = z80->regs.a | z80->regs.b;
        goto alu_or;
    case 0xB1:
        result = z80->regs.a | z80->regs.c;
        goto alu_or;
    case 0xB2:
        result = z80->regs.a | z80->regs.d;
        goto alu_or;
    case 0xB3:
        result = z80->regs.a | z80->regs.e;
        goto alu_or;
    case 0xB4:
        result = z80->regs.a | z80->regs.h;
        goto alu_or;
    case 0xB5:
        result = z80->regs.a | z80->regs.l;
        goto alu_or;
    case 0xB6:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a | z80_read_memory_internal(z80, addr);
        goto alu_or;
    case 0xB7:
        result = z80->regs.a | z80->regs.a;
    alu_or:
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_parity((uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 4;
        break;

    // CP A,r
    case 0xB9:
        result = z80->regs.a - z80->regs.c;
        goto alu_cp;
    case 0xBA:
        result = z80->regs.a - z80->regs.d;
        goto alu_cp;
    case 0xBB:
        result = z80->regs.a - z80->regs.e;
        goto alu_cp;
    case 0xBC:
        result = z80->regs.a - z80->regs.h;
        goto alu_cp;
    case 0xBD:
        result = z80->regs.a - z80->regs.l;
        goto alu_cp;
    case 0xBE:
        addr = (z80->regs.h << 8) | z80->regs.l;
        result = z80->regs.a - z80_read_memory_internal(z80, addr);
        goto alu_cp;
    case 0xBF:
        result = z80->regs.a - z80->regs.a;
    alu_cp:
    {
        uint8_t operand = z80->regs.a - (uint8_t)result;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        cycles = 4;
        break;
    }

    // Additional immediate ALU operations
    case 0xC6: // ADD A,n
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        result = z80->regs.a + operand;
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_add(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 7;
        break;

    case 0xCE: // ADC A,n
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        result = z80->regs.a + operand + ((z80->regs.f & Z80_FLAG_C) ? 1 : 0);
        z80->regs.f = 0;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_add(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_add(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 7;
        break;

    case 0xD6: // SUB A,n
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        result = z80->regs.a - operand;
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 7;
        break;

    case 0xDE: // SBC A,n
        operand = z80_read_memory_internal(z80, z80->regs.pc++);
        result = z80->regs.a - operand - ((z80->regs.f & Z80_FLAG_C) ? 1 : 0);
        z80->regs.f = Z80_FLAG_N;
        if ((uint8_t)result == 0)
            z80->regs.f |= Z80_FLAG_Z;
        if ((uint8_t)result & 0x80)
            z80->regs.f |= Z80_FLAG_S;
        if (calculate_half_carry_sub(z80->regs.a, operand))
            z80->regs.f |= Z80_FLAG_H;
        if (result & 0x100)
            z80->regs.f |= Z80_FLAG_C;
        if (calculate_overflow_sub(z80->regs.a, operand, (uint8_t)result))
            z80->regs.f |= Z80_FLAG_PV;
        z80->regs.a = (uint8_t)result;
        cycles = 7;
        break;

    // Default: unknown instruction - this is a fatal error
    default:
    {
        fprintf(stderr, "FATAL: Unknown Z80 instruction 0x%02X at PC 0x%04X\n", opcode, z80->regs.pc - 1);
        fprintf(stderr, "Instruction decode failed. Terminating emulation.\n");
        exit(EXIT_FAILURE);
    }
    }

    return cycles;
}

/**
 * Z80 CPU thread function
 * Executes instructions with proper cycle timing at 3.5MHz
 */
static void *z80_thread_func(void *arg)
{
    z80_emulator_t *z80 = (z80_emulator_t *)arg;
    struct timespec cycle_start, cycle_end;

    // Nanoseconds per cycle at 3.5MHz: 1 / 3500000 * 1e9
    const long NS_PER_CYCLE = 1000000000 / Z80_CLOCK_FREQ; // ~286 ns per cycle

    while (z80->running)
    {
        pthread_mutex_lock(&z80->state_lock);

        // Handle pause requests
        while (z80->paused && z80->running)
        {
            pthread_cond_wait(&z80->state_cond, &z80->state_lock);
        }

        pthread_mutex_unlock(&z80->state_lock);

        if (!z80->running)
            break;

        // Get cycle start time
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);

        // Execute instruction and get cycle count
        int instruction_cycles = z80_execute_instruction(z80);
        long total_ns = instruction_cycles * NS_PER_CYCLE;

        pthread_mutex_lock(&z80->state_lock);
        z80->total_cycles += instruction_cycles;
        pthread_mutex_unlock(&z80->state_lock);

        // Get cycle end time
        clock_gettime(CLOCK_MONOTONIC, &cycle_end);

        // Calculate elapsed time
        long elapsed_ns = (cycle_end.tv_sec - cycle_start.tv_sec) * 1000000000L +
                          (cycle_end.tv_nsec - cycle_start.tv_nsec);

        // Sleep for remaining instruction time
        if (elapsed_ns < total_ns)
        {
            long sleep_ns = total_ns - elapsed_ns;
            usleep(sleep_ns / 1000); // Convert to microseconds
        }
    }

    return NULL;
}

/**
 * Start Z80 emulation in a thread
 */
int z80_start(z80_emulator_t *z80)
{
    if (!z80 || z80->running)
        return -1;

    pthread_mutex_lock(&z80->state_lock);
    z80->running = 1;
    z80->paused = 0;
    pthread_mutex_unlock(&z80->state_lock);

    return pthread_create(&z80->thread, NULL, z80_thread_func, z80);
}

/**
 * Stop Z80 emulation
 */
void z80_stop(z80_emulator_t *z80)
{
    if (!z80)
        return;

    pthread_mutex_lock(&z80->state_lock);
    z80->running = 0;
    z80->paused = 0;
    pthread_cond_broadcast(&z80->state_cond);
    pthread_mutex_unlock(&z80->state_lock);

    if (z80->thread)
    {
        pthread_join(z80->thread, NULL);
        z80->thread = 0;
    }
}

/**
 * Pause Z80 emulation
 */
void z80_pause(z80_emulator_t *z80)
{
    if (!z80)
        return;

    pthread_mutex_lock(&z80->state_lock);
    z80->paused = 1;
    pthread_mutex_unlock(&z80->state_lock);
}

/**
 * Resume Z80 emulation
 */
void z80_resume(z80_emulator_t *z80)
{
    if (!z80)
        return;

    pthread_mutex_lock(&z80->state_lock);
    z80->paused = 0;
    pthread_cond_broadcast(&z80->state_cond);
    pthread_mutex_unlock(&z80->state_lock);
}

/**
 * Save Z80 state to buffer
 * Saves only register state and cycle counter
 * Memory is managed externally and not saved here
 */
#define Z80_STATE_SIZE (sizeof(z80_registers_t) + sizeof(uint64_t))

size_t z80_save_state(z80_emulator_t *z80, uint8_t *buffer, size_t buffer_size)
{
    if (!z80 || !buffer)
        return 0;
    if (buffer_size < Z80_STATE_SIZE)
        return 0;

    pthread_mutex_lock(&z80->state_lock);

    size_t offset = 0;

    // Save registers
    memcpy(buffer + offset, &z80->regs, sizeof(z80_registers_t));
    offset += sizeof(z80_registers_t);

    // Save total cycles
    memcpy(buffer + offset, &z80->total_cycles, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    pthread_mutex_unlock(&z80->state_lock);

    return offset;
}

/**
 * Load Z80 state from buffer
 * Restores only register state and cycle counter
 * Memory is managed externally and not restored here
 */
int z80_load_state(z80_emulator_t *z80, const uint8_t *buffer, size_t buffer_size)
{
    if (!z80 || !buffer)
        return -1;
    if (buffer_size < Z80_STATE_SIZE)
        return -1;

    // Pause CPU while loading state
    int was_running = z80->running;
    if (was_running)
        z80_pause(z80);
    usleep(10000); // Give thread time to pause

    pthread_mutex_lock(&z80->state_lock);

    size_t offset = 0;

    // Load registers
    memcpy(&z80->regs, buffer + offset, sizeof(z80_registers_t));
    offset += sizeof(z80_registers_t);

    // Load total cycles
    memcpy(&z80->total_cycles, buffer + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    pthread_mutex_unlock(&z80->state_lock);

    // Resume if it was running
    if (was_running)
        z80_resume(z80);

    return 0;
}

/**
 * Get register value (for debugging/inspection)
 */
uint16_t z80_get_register(z80_emulator_t *z80, const char *reg_name)
{
    if (!z80)
        return 0;

    pthread_mutex_lock(&z80->state_lock);

    uint16_t value = 0;
    if (strcmp(reg_name, "PC") == 0)
        value = z80->regs.pc;
    else if (strcmp(reg_name, "SP") == 0)
        value = z80->regs.sp;
    else if (strcmp(reg_name, "IX") == 0)
        value = z80->regs.ix;
    else if (strcmp(reg_name, "IY") == 0)
        value = z80->regs.iy;
    else if (strcmp(reg_name, "A") == 0)
        value = z80->regs.a;
    else if (strcmp(reg_name, "F") == 0)
        value = z80->regs.f;
    else if (strcmp(reg_name, "B") == 0)
        value = z80->regs.b;
    else if (strcmp(reg_name, "C") == 0)
        value = z80->regs.c;
    else if (strcmp(reg_name, "D") == 0)
        value = z80->regs.d;
    else if (strcmp(reg_name, "E") == 0)
        value = z80->regs.e;
    else if (strcmp(reg_name, "H") == 0)
        value = z80->regs.h;
    else if (strcmp(reg_name, "L") == 0)
        value = z80->regs.l;

    pthread_mutex_unlock(&z80->state_lock);
    return value;
}

/**
 * Set register value (for debugging/initialization)
 */
void z80_set_register(z80_emulator_t *z80, const char *reg_name, uint16_t value)
{
    if (!z80)
        return;

    pthread_mutex_lock(&z80->state_lock);

    if (strcmp(reg_name, "PC") == 0)
        z80->regs.pc = value;
    else if (strcmp(reg_name, "SP") == 0)
        z80->regs.sp = value;
    else if (strcmp(reg_name, "IX") == 0)
        z80->regs.ix = value;
    else if (strcmp(reg_name, "IY") == 0)
        z80->regs.iy = value;
    else if (strcmp(reg_name, "A") == 0)
        z80->regs.a = value & 0xFF;
    else if (strcmp(reg_name, "F") == 0)
        z80->regs.f = value & 0xFF;
    else if (strcmp(reg_name, "B") == 0)
        z80->regs.b = value & 0xFF;
    else if (strcmp(reg_name, "C") == 0)
        z80->regs.c = value & 0xFF;
    else if (strcmp(reg_name, "D") == 0)
        z80->regs.d = value & 0xFF;
    else if (strcmp(reg_name, "E") == 0)
        z80->regs.e = value & 0xFF;
    else if (strcmp(reg_name, "H") == 0)
        z80->regs.h = value & 0xFF;
    else if (strcmp(reg_name, "L") == 0)
        z80->regs.l = value & 0xFF;

    pthread_mutex_unlock(&z80->state_lock);
}

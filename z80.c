#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include "z80.h"

// Z80 Configuration
#define Z80_CLOCK_FREQ 3500000 // 3.5 MHz
#define Z80_MAX_MEMORY 65536   // 64KB address space

// Z80 Flags (F register bits)
#define Z80_FLAG_C 0x01  // Carry
#define Z80_FLAG_N 0x02  // Subtract
#define Z80_FLAG_PV 0x04 // Parity/Overflow
#define Z80_FLAG_H 0x10  // Half-carry
#define Z80_FLAG_Z 0x40  // Zero
#define Z80_FLAG_S 0x80  // Sign

#define GET_BIT(n, val) (((val) >> (n)) & 1)

static const uint8_t cyc_00[256] = {4, 10, 7, 6, 4, 4, 7, 4, 4, 11, 7, 6, 4, 4,
                                    7, 4, 8, 10, 7, 6, 4, 4, 7, 4, 12, 11, 7, 6, 4, 4, 7, 4, 7, 10, 16, 6, 4, 4,
                                    7, 4, 7, 11, 16, 6, 4, 4, 7, 4, 7, 10, 13, 6, 11, 11, 10, 4, 7, 11, 13, 6,
                                    4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4,
                                    4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4,
                                    7, 4, 7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7,
                                    4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
                                    4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4,
                                    4, 4, 4, 4, 4, 7, 4, 5, 10, 10, 10, 10, 11, 7, 11, 5, 10, 10, 0, 10, 17, 7,
                                    11, 5, 10, 10, 11, 10, 11, 7, 11, 5, 4, 10, 11, 10, 0, 7, 11, 5, 10, 10, 19,
                                    10, 11, 7, 11, 5, 4, 10, 4, 10, 0, 7, 11, 5, 10, 10, 4, 10, 11, 7, 11, 5, 6,
                                    10, 4, 10, 0, 7, 11};

static const uint8_t cyc_ed[256] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 12,
                                    12, 15, 20, 8, 14, 8, 9, 12, 12, 15, 20, 8, 14, 8, 9, 12, 12, 15, 20, 8, 14,
                                    8, 9, 12, 12, 15, 20, 8, 14, 8, 9, 12, 12, 15, 20, 8, 14, 8, 18, 12, 12, 15,
                                    20, 8, 14, 8, 18, 12, 12, 15, 20, 8, 14, 8, 8, 12, 12, 15, 20, 8, 14, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 16, 8, 8, 8, 8, 16, 16, 16, 16, 8, 8, 8, 8,
                                    16, 16, 16, 16, 8, 8, 8, 8, 16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
                                    8, 8, 8, 8, 8, 8, 8};

static const uint8_t cyc_ddfd[256] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 15, 4, 4, 4, 4,
                                      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 15, 4, 4, 4, 4, 4, 4, 4, 14, 20, 10, 8, 8,
                                      11, 4, 4, 15, 20, 10, 8, 8, 11, 4, 4, 4, 4, 4, 23, 23, 19, 4, 4, 15, 4, 4,
                                      4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4, 4, 8,
                                      8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 8, 8, 8, 8, 8, 8, 19, 8, 8, 8, 8, 8, 8,
                                      8, 19, 8, 19, 19, 19, 19, 19, 19, 4, 19, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4,
                                      4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4,
                                      4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4,
                                      4, 8, 8, 19, 4, 4, 4, 4, 4, 8, 8, 19, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0,
                                      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 14, 4, 23, 4,
                                      15, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 10, 4, 4, 4, 4,
                                      4, 4};

static int exec_opcode(z80_emulator_t *const z, uint8_t opcode);
static int exec_opcode_cb(z80_emulator_t *const z, uint8_t opcode);
static int exec_opcode_dcb(z80_emulator_t *const z, const uint8_t opcode, const uint16_t addr);
static int exec_opcode_ed(z80_emulator_t *const z, uint8_t opcode);
static int exec_opcode_ddfd(z80_emulator_t *const z, uint8_t opcode, uint16_t *const iz);

/**
 * Internal I/O read with callback support
 */
static uint8_t z80_read_io_internal(z80_emulator_t *z80, uint8_t port)
{
    // Check for port-specific callback first
    if (z80->port_callbacks[port].read_fn)
    {
        void *io_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            io_data = ctx->io_data;
        }
        return z80->port_callbacks[port].read_fn(io_data, port);
    }

    // Fall back to generic I/O callback
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
    // Check for port-specific callback first
    if (z80->port_callbacks[port].write_fn)
    {
        void *io_data = z80->user_data;
        if (z80->user_data)
        {
            z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
            io_data = ctx->io_data;
        }
        z80->port_callbacks[port].write_fn(io_data, port, value);
        return;
    }

    // Fall back to generic I/O callback
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

static inline uint8_t rb(z80_emulator_t *const z, uint16_t addr)
{
    return z->read_memory(z->user_data, addr);
}

static inline void wb(z80_emulator_t *const z, uint16_t addr, uint8_t val)
{
    z->write_memory(z->user_data, addr, val);
}

static inline uint16_t rw(z80_emulator_t *const z, uint16_t addr)
{
    return (z->read_memory(z->user_data, addr + 1) << 8) |
           z->read_memory(z->user_data, addr);
}

static inline void ww(z80_emulator_t *const z, uint16_t addr, uint16_t val)
{
    z->write_memory(z->user_data, addr, val & 0xFF);
    z->write_memory(z->user_data, addr + 1, val >> 8);
}

static inline void pushw(z80_emulator_t *const z, uint16_t val)
{
    z->regs.sp -= 2;
    ww(z, z->regs.sp, val);
}

static inline uint16_t popw(z80_emulator_t *const z)
{
    z->regs.sp += 2;
    return rw(z, z->regs.sp - 2);
}

static inline uint8_t nextb(z80_emulator_t *const z)
{
    return rb(z, z->regs.pc++);
}

static inline uint16_t nextw(z80_emulator_t *const z)
{
    z->regs.pc += 2;
    return rw(z, z->regs.pc - 2);
}

static inline uint16_t get_bc(z80_emulator_t *const z)
{
    return (z->regs.b << 8) | z->regs.c;
}

static inline uint16_t get_de(z80_emulator_t *const z)
{
    return (z->regs.d << 8) | z->regs.e;
}

static inline uint16_t get_hl(z80_emulator_t *const z)
{
    return (z->regs.h << 8) | z->regs.l;
}

static inline void set_bc(z80_emulator_t *const z, uint16_t val)
{
    z->regs.b = val >> 8;
    z->regs.c = val & 0xFF;
}

static inline void set_de(z80_emulator_t *const z, uint16_t val)
{
    z->regs.d = val >> 8;
    z->regs.e = val & 0xFF;
}

static inline void set_hl(z80_emulator_t *const z, uint16_t val)
{
    z->regs.h = val >> 8;
    z->regs.l = val & 0xFF;
}

inline uint8_t get_f(z80_emulator_t *const z)
{
    uint8_t val = 0;
    val |= z->regs.cf << 0;
    val |= z->regs.nf << 1;
    val |= z->regs.pf << 2;
    val |= z->regs.xf << 3;
    val |= z->regs.hf << 4;
    val |= z->regs.yf << 5;
    val |= z->regs.zf << 6;
    val |= z->regs.sf << 7;
    return val;
}

inline void set_f(z80_emulator_t *const z, uint8_t val)
{
    z->regs.cf = (val >> 0) & 1;
    z->regs.nf = (val >> 1) & 1;
    z->regs.pf = (val >> 2) & 1;
    z->regs.xf = (val >> 3) & 1;
    z->regs.hf = (val >> 4) & 1;
    z->regs.yf = (val >> 5) & 1;
    z->regs.zf = (val >> 6) & 1;
    z->regs.sf = (val >> 7) & 1;
}

// increments R, keeping the highest byte intact
static inline void inc_r(z80_emulator_t *const z)
{
    z->regs.r = (z->regs.r & 0x80) | ((z->regs.r + 1) & 0x7f);
}

// returns if there was a carry between bit "bit_no" and "bit_no - 1" when
// executing "a + b + cy"
static inline bool carry(int bit_no, uint16_t a, uint16_t b, bool cy)
{
    int32_t result = a + b + cy;
    int32_t carry = result ^ a ^ b;
    return carry & (1 << bit_no);
}

// returns the parity of byte: 0 if number of 1 bits in `val` is odd, else 1
static inline bool parity(uint8_t val)
{
    uint8_t nb_one_bits = 0;
    for (int i = 0; i < 8; i++)
    {
        nb_one_bits += ((val >> i) & 1);
    }

    return (nb_one_bits & 1) == 0;
}

// function to call when an NMI is to be serviced
void z80_gen_nmi(z80_emulator_t *const z)
{
    z->nmi_pending = 1;
}

// MARK: opcodes
// jumps to an address
static inline void jump(z80_emulator_t *const z, uint16_t addr)
{
    z->regs.pc = addr;
    z->regs.mem_ptr = addr;
}

// jumps to next word in memory if condition is true
static inline void cond_jump(z80_emulator_t *const z, bool condition)
{
    const uint16_t addr = nextw(z);
    if (condition)
    {
        jump(z, addr);
    }
    z->regs.mem_ptr = addr;
}

// calls to next word in memory
static inline void call(z80_emulator_t *const z, uint16_t addr)
{
    pushw(z, z->regs.pc);
    z->regs.pc = addr;
    z->regs.mem_ptr = addr;
}

// calls to next word in memory if condition is true
static inline void cond_call(z80_emulator_t *const z, bool condition)
{
    const uint16_t addr = nextw(z);
    if (condition)
    {
        call(z, addr);
        z->cyc += 7;
    }
    z->regs.mem_ptr = addr;
}

// returns from subroutine
static inline void ret(z80_emulator_t *const z)
{
    z->regs.pc = popw(z);
    z->regs.mem_ptr = z->regs.pc;
}

// returns from subroutine if condition is true
static inline void cond_ret(z80_emulator_t *const z, bool condition)
{
    if (condition)
    {
        ret(z);
        z->cyc += 6;
    }
}

static inline void jr(z80_emulator_t *const z, int8_t displacement)
{
    z->regs.pc += displacement;
    z->regs.mem_ptr = z->regs.pc;
}

static inline void cond_jr(z80_emulator_t *const z, bool condition)
{
    const int8_t b = nextb(z);
    if (condition)
    {
        jr(z, b);
        z->cyc += 5;
    }
}

// ADD Byte: adds two bytes together
static inline uint8_t addb(z80_emulator_t *const z, uint8_t a, uint8_t b, bool cy)
{
    const uint8_t result = a + b + cy;
    z->regs.sf = result >> 7;
    z->regs.zf = result == 0;
    z->regs.hf = carry(4, a, b, cy);
    z->regs.pf = carry(7, a, b, cy) != carry(8, a, b, cy);
    z->regs.cf = carry(8, a, b, cy);
    z->regs.nf = 0;
    z->regs.xf = GET_BIT(3, result);
    z->regs.yf = GET_BIT(5, result);
    return result;
}

// SUBstract Byte: substracts two bytes (with optional carry)
static inline uint8_t subb(z80_emulator_t *const z, uint8_t a, uint8_t b, bool cy)
{
    uint8_t val = addb(z, a, ~b, !cy);
    z->regs.cf = !z->regs.cf;
    z->regs.hf = !z->regs.hf;
    z->regs.nf = 1;
    return val;
}

// ADD Word: adds two words together
static inline uint16_t addw(z80_emulator_t *const z, uint16_t a, uint16_t b, bool cy)
{
    uint8_t lsb = addb(z, a, b, cy);
    uint8_t msb = addb(z, a >> 8, b >> 8, z->regs.cf);

    uint16_t result = (msb << 8) | lsb;
    z->regs.zf = result == 0;
    z->regs.mem_ptr = a + 1;
    return result;
}

// SUBstract Word: substracts two words (with optional carry)
static inline uint16_t subw(z80_emulator_t *const z, uint16_t a, uint16_t b, bool cy)
{
    uint8_t lsb = subb(z, a, b, cy);
    uint8_t msb = subb(z, a >> 8, b >> 8, z->regs.cf);

    uint16_t result = (msb << 8) | lsb;
    z->regs.zf = result == 0;
    z->regs.mem_ptr = a + 1;
    return result;
}

// adds a word to HL
static inline void addhl(z80_emulator_t *const z, uint16_t val)
{
    bool sf = z->regs.sf;
    bool zf = z->regs.zf;
    bool pf = z->regs.pf;
    uint16_t result = addw(z, get_hl(z), val, 0);
    set_hl(z, result);
    z->regs.sf = sf;
    z->regs.zf = zf;
    z->regs.pf = pf;
}

// adds a word to IX or IY
static inline void addiz(z80_emulator_t *const z, uint16_t *reg, uint16_t val)
{
    bool sf = z->regs.sf;
    bool zf = z->regs.zf;
    bool pf = z->regs.pf;
    uint16_t result = addw(z, *reg, val, 0);
    *reg = result;
    z->regs.sf = sf;
    z->regs.zf = zf;
    z->regs.pf = pf;
}

// adds a word (+ carry) to HL
static inline void adchl(z80_emulator_t *const z, uint16_t val)
{
    uint16_t result = addw(z, get_hl(z), val, z->regs.cf);
    z->regs.sf = result >> 15;
    z->regs.zf = result == 0;
    set_hl(z, result);
}

// substracts a word (+ carry) to HL
static inline void sbchl(z80_emulator_t *const z, uint16_t val)
{
    const uint16_t result = subw(z, get_hl(z), val, z->regs.cf);
    z->regs.sf = result >> 15;
    z->regs.zf = result == 0;
    set_hl(z, result);
}

// increments a byte value
static inline uint8_t inc(z80_emulator_t *const z, uint8_t a)
{
    bool cf = z->regs.cf;
    uint8_t result = addb(z, a, 1, 0);
    z->regs.cf = cf;
    return result;
}

// decrements a byte value
static inline uint8_t dec(z80_emulator_t *const z, uint8_t a)
{
    bool cf = z->regs.cf;
    uint8_t result = subb(z, a, 1, 0);
    z->regs.cf = cf;
    return result;
}

// MARK: bitwise

// executes a logic "and" between register A and a byte, then stores the
// result in register A
static inline void land(z80_emulator_t *const z, uint8_t val)
{
    const uint8_t result = z->regs.a & val;
    z->regs.sf = result >> 7;
    z->regs.zf = result == 0;
    z->regs.hf = 1;
    z->regs.pf = parity(result);
    z->regs.nf = 0;
    z->regs.cf = 0;
    z->regs.xf = GET_BIT(3, result);
    z->regs.yf = GET_BIT(5, result);
    z->regs.a = result;
}

// executes a logic "xor" between register A and a byte, then stores the
// result in register A
static inline void lxor(z80_emulator_t *const z, const uint8_t val)
{
    const uint8_t result = z->regs.a ^ val;
    z->regs.sf = result >> 7;
    z->regs.zf = result == 0;
    z->regs.hf = 0;
    z->regs.pf = parity(result);
    z->regs.nf = 0;
    z->regs.cf = 0;
    z->regs.xf = GET_BIT(3, result);
    z->regs.yf = GET_BIT(5, result);
    z->regs.a = result;
}

// executes a logic "or" between register A and a byte, then stores the
// result in register A
static inline void lor(z80_emulator_t *const z, const uint8_t val)
{
    const uint8_t result = z->regs.a | val;
    z->regs.sf = result >> 7;
    z->regs.zf = result == 0;
    z->regs.hf = 0;
    z->regs.pf = parity(result);
    z->regs.nf = 0;
    z->regs.cf = 0;
    z->regs.xf = GET_BIT(3, result);
    z->regs.yf = GET_BIT(5, result);
    z->regs.a = result;
}

// compares a value with register A
static inline void cp(z80_emulator_t *const z, const uint8_t val)
{
    subb(z, z->regs.a, val, 0);

    // the only difference between cp and sub is that
    // the xf/yf are taken from the value to be substracted,
    // not the result
    z->regs.yf = GET_BIT(5, val);
    z->regs.xf = GET_BIT(3, val);
}

// 0xCB opcodes
// rotate left with carry
static inline uint8_t cb_rlc(z80_emulator_t *const z, uint8_t val)
{
    const bool old = val >> 7;
    val = (val << 1) | old;
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.pf = parity(val);
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.cf = old;
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// rotate right with carry
static inline uint8_t cb_rrc(z80_emulator_t *const z, uint8_t val)
{
    const bool old = val & 1;
    val = (val >> 1) | (old << 7);
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.cf = old;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// rotate left (simple)
static inline uint8_t cb_rl(z80_emulator_t *const z, uint8_t val)
{
    const bool cf = z->regs.cf;
    z->regs.cf = val >> 7;
    val = (val << 1) | cf;
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// rotate right (simple)
static inline uint8_t cb_rr(z80_emulator_t *const z, uint8_t val)
{
    const bool c = z->regs.cf;
    z->regs.cf = val & 1;
    val = (val >> 1) | (c << 7);
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// shift left preserving sign
static inline uint8_t cb_sla(z80_emulator_t *const z, uint8_t val)
{
    z->regs.cf = val >> 7;
    val <<= 1;
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// SLL (exactly like SLA, but sets the first bit to 1)
static inline uint8_t cb_sll(z80_emulator_t *const z, uint8_t val)
{
    z->regs.cf = val >> 7;
    val <<= 1;
    val |= 1;
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// shift right preserving sign
static inline uint8_t cb_sra(z80_emulator_t *const z, uint8_t val)
{
    z->regs.cf = val & 1;
    val = (val >> 1) | (val & 0x80); // 0b10000000
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// shift register right
static inline uint8_t cb_srl(z80_emulator_t *const z, uint8_t val)
{
    z->regs.cf = val & 1;
    val >>= 1;
    z->regs.sf = val >> 7;
    z->regs.zf = val == 0;
    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = parity(val);
    z->regs.xf = GET_BIT(3, val);
    z->regs.yf = GET_BIT(5, val);
    return val;
}

// tests bit "n" from a byte
static inline uint8_t cb_bit(z80_emulator_t *const z, uint8_t val, uint8_t n)
{
    const uint8_t result = val & (1 << n);
    z->regs.sf = result >> 7;
    z->regs.zf = result == 0;
    z->regs.yf = GET_BIT(5, val);
    z->regs.hf = 1;
    z->regs.xf = GET_BIT(3, val);
    z->regs.pf = z->regs.zf;
    z->regs.nf = 0;
    return result;
}

static inline void ldi(z80_emulator_t *const z)
{
    const uint16_t de = get_de(z);
    const uint16_t hl = get_hl(z);
    const uint8_t val = rb(z, hl);

    wb(z, de, val);

    set_hl(z, get_hl(z) + 1);
    set_de(z, get_de(z) + 1);
    set_bc(z, get_bc(z) - 1);

    // see https://wikiti.brandonw.net/index.php?title=Z80_Instruction_Set
    // for the calculation of xf/yf on LDI
    const uint8_t result = val + z->regs.a;
    z->regs.xf = GET_BIT(3, result);
    z->regs.yf = GET_BIT(1, result);

    z->regs.nf = 0;
    z->regs.hf = 0;
    z->regs.pf = get_bc(z) > 0;
}

static inline void ldd(z80_emulator_t *const z)
{
    ldi(z);
    // same as ldi but HL and DE are decremented instead of incremented
    set_hl(z, get_hl(z) - 2);
    set_de(z, get_de(z) - 2);
}

static inline void cpi(z80_emulator_t *const z)
{
    bool cf = z->regs.cf;
    const uint8_t result = subb(z, z->regs.a, rb(z, get_hl(z)), 0);
    set_hl(z, get_hl(z) + 1);
    set_bc(z, get_bc(z) - 1);
    z->regs.xf = GET_BIT(3, result - z->regs.hf);
    z->regs.yf = GET_BIT(1, result - z->regs.hf);
    z->regs.pf = get_bc(z) != 0;
    z->regs.cf = cf;
    z->regs.mem_ptr += 1;
}

static inline void cpd(z80_emulator_t *const z)
{
    cpi(z);
    // same as cpi but HL is decremented instead of incremented
    set_hl(z, get_hl(z) - 2);
    z->regs.mem_ptr -= 2;
}

static void in_r_c(z80_emulator_t *const z, uint8_t *r)
{
    *r = z80_read_io_internal(z, z->regs.c);
    z->regs.zf = *r == 0;
    z->regs.sf = *r >> 7;
    z->regs.pf = parity(*r);
    z->regs.nf = 0;
    z->regs.hf = 0;
}

static void ini(z80_emulator_t *const z)
{
    uint8_t val = z80_read_io_internal(z, z->regs.c);
    wb(z, get_hl(z), val);
    set_hl(z, get_hl(z) + 1);
    z->regs.b -= 1;
    z->regs.zf = z->regs.b == 0;
    z->regs.nf = 1;
    z->regs.mem_ptr = get_bc(z) + 1;
}

static void ind(z80_emulator_t *const z)
{
    ini(z);
    set_hl(z, get_hl(z) - 2);
    z->regs.mem_ptr = get_bc(z) - 2;
}

static void outi(z80_emulator_t *const z)
{
    z80_write_io_internal(z, z->regs.c, rb(z, get_hl(z)));
    set_hl(z, get_hl(z) + 1);
    z->regs.b -= 1;
    z->regs.zf = z->regs.b == 0;
    z->regs.nf = 1;
    z->regs.mem_ptr = get_bc(z) + 1;
}

static void outd(z80_emulator_t *const z)
{
    outi(z);
    set_hl(z, get_hl(z) - 2);
    z->regs.mem_ptr = get_bc(z) - 2;
}

static void daa(z80_emulator_t *const z)
{
    // "When this instruction is executed, the A register is BCD corrected
    // using the  contents of the flags. The exact process is the following:
    // if the least significant four bits of A contain a non-BCD digit
    // (i. e. it is greater than 9) or the H flag is set, then $06 is
    // added to the register. Then the four most significant bits are
    // checked. If this more significant digit also happens to be greater
    // than 9 or the C flag is set, then $60 is added."
    // > http://z80-heaven.wikidot.com/instructions-set:daa
    uint8_t correction = 0;

    if ((z->regs.a & 0x0F) > 0x09 || z->regs.hf)
    {
        correction += 0x06;
    }

    if (z->regs.a > 0x99 || z->regs.cf)
    {
        correction += 0x60;
        z->regs.cf = 1;
    }

    const bool substraction = z->regs.nf;
    if (substraction)
    {
        z->regs.hf = z->regs.hf && (z->regs.a & 0x0F) < 0x06;
        z->regs.a -= correction;
    }
    else
    {
        z->regs.hf = (z->regs.a & 0x0F) > 0x09;
        z->regs.a += correction;
    }

    z->regs.sf = z->regs.a >> 7;
    z->regs.zf = z->regs.a == 0;
    z->regs.pf = parity(z->regs.a);
    z->regs.xf = GET_BIT(3, z->regs.a);
    z->regs.yf = GET_BIT(5, z->regs.a);
}

static inline uint16_t displace(z80_emulator_t *const z, uint16_t base_addr, int8_t displacement)
{
    const uint16_t addr = base_addr + displacement;
    z->regs.mem_ptr = addr;
    return addr;
}

static inline void process_interrupts(z80_emulator_t *const z)
{
    // "When an EI instruction is executed, any pending interrupt request
    // is not accepted until after the instruction following EI is executed."
    if (z->regs.iff_delay > 0)
    {
        z->regs.iff_delay -= 1;
        if (z->regs.iff_delay == 0)
        {
            z->regs.iff1 = 1;
            z->regs.iff2 = 1;
        }
        return;
    }

    if (z->nmi_pending)
    {
        z->nmi_pending = 0;
        z->halted = 0;
        z->regs.iff1 = 0;
        inc_r(z);

        z->cyc += 11;
        call(z, 0x66);
        return;
    }

    if (z->int_pending && z->regs.iff1)
    {
        z->int_pending = 0;
        z->halted = 0;
        z->regs.iff1 = 0;
        z->regs.iff2 = 0;
        inc_r(z);

        switch (z->regs.im)
        {
        case 0:
            z->cyc += 11;
            exec_opcode(z, z->int_data);
            break;

        case 1:
            z->cyc += 13;
            call(z, 0x38);
            break;

        case 2:
            z->cyc += 19;
            call(z, rw(z, (z->regs.i << 8) | z->int_data));
            break;

        default:
            fprintf(stderr, "unsupported interrupt mode %d\n", z->regs.im);
            break;
        }

        return;
    }
}

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
    z80->regs.im = 0;

    z80->regs.a = 0xFF;
    z80->regs.b = 0;
    z80->regs.c = 0;
    z80->regs.d = 0;
    z80->regs.e = 0;
    z80->regs.h = 0;
    z80->regs.l = 0;

    z80->regs.a_ = 0;
    z80->regs.b_ = 0;
    z80->regs.c_ = 0;
    z80->regs.d_ = 0;
    z80->regs.e_ = 0;
    z80->regs.h_ = 0;
    z80->regs.l_ = 0;
    z80->regs.f_ = 0;
    z80->regs.i = 0;
    z80->regs.r = 0;

    z80->regs.sf = 1;
    z80->regs.zf = 1;
    z80->regs.yf = 1;
    z80->regs.hf = 1;
    z80->regs.xf = 1;
    z80->regs.pf = 1;
    z80->regs.nf = 1;
    z80->regs.cf = 1;

    // Initialize state
    z80->running = 0;
    z80->paused = 0;
    z80->halted = 0;
    z80->cyc = 0;

    // Initialize synchronization primitives
    pthread_mutex_init(&z80->state_lock, NULL);
    pthread_cond_init(&z80->state_cond, NULL);

    // Initialize callbacks to NULL
    z80->read_io = NULL;
    z80->write_io = NULL;
    z80->read_memory = NULL;
    z80->write_memory = NULL;
    z80->user_data = NULL;

    // Initialize port-specific callbacks
    for (int i = 0; i < Z80_IO_PORTS; i++)
    {
        z80->port_callbacks[i].read_fn = NULL;
        z80->port_callbacks[i].write_fn = NULL;
    }

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
 * Set memory callbacks for pluggable RAM/ROM
 */
void z80_set_memory_callbacks(z80_emulator_t *z80,
                              z80_read_memory_t read_memory,
                              z80_write_memory_t write_memory,
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
 * Register port-specific IN callback
 */
void z80_register_port_in(z80_emulator_t *z80,
                          uint8_t port,
                          z80_read_io_t read_fn)
{
    if (!z80 || !read_fn)
        return;

    z80->port_callbacks[port].read_fn = read_fn;
}

/**
 * Register port-specific OUT callback
 */
void z80_register_port_out(z80_emulator_t *z80,
                           uint8_t port,
                           z80_write_io_t write_fn)
{
    if (!z80 || !write_fn)
        return;

    z80->port_callbacks[port].write_fn = write_fn;
}

/**
 * Set I/O callback context data
 */
void z80_set_io_callbacks(z80_emulator_t *z80, void *io_data)
{
    if (!z80)
        return;

    // Create context if it doesn't exist
    if (!z80->user_data)
    {
        z80_callback_context_t *ctx = malloc(sizeof(z80_callback_context_t));
        if (!ctx)
            return;
        ctx->memory_data = NULL;
        ctx->io_data = io_data;
        z80->user_data = ctx;
    }
    else
    {
        // Update existing context
        z80_callback_context_t *ctx = (z80_callback_context_t *)z80->user_data;
        ctx->io_data = io_data;
    }
}

// function to call when an INT is to be serviced
void z80_gen_int(z80_emulator_t *const z, uint8_t data)
{
    z->int_pending = 1;
    z->int_data = data;
}

// executes the next instruction in memory + handles interrupts
int z80_step(z80_emulator_t *const z)
{
    int cyc;

    if (z->halted)
    {
        cyc = exec_opcode(z, 0x00);
    }
    else
    {
        const uint8_t opcode = nextb(z);
        cyc = exec_opcode(z, opcode);
    }

    process_interrupts(z);
    return cyc;
}

// executes a non-prefixed opcode
int exec_opcode(z80_emulator_t *const z, uint8_t opcode)
{
    uint64_t cyc_before = z->cyc;
    z->cyc += cyc_00[opcode];
    inc_r(z);

    switch (opcode)
    {
    case 0x7F:
        z->regs.a = z->regs.a;
        break; // ld a,a
    case 0x78:
        z->regs.a = z->regs.b;
        break; // ld a,b
    case 0x79:
        z->regs.a = z->regs.c;
        break; // ld a,c
    case 0x7A:
        z->regs.a = z->regs.d;
        break; // ld a,d
    case 0x7B:
        z->regs.a = z->regs.e;
        break; // ld a,e
    case 0x7C:
        z->regs.a = z->regs.h;
        break; // ld a,h
    case 0x7D:
        z->regs.a = z->regs.l;
        break; // ld a,l

    case 0x47:
        z->regs.b = z->regs.a;
        break; // ld b,a
    case 0x40:
        z->regs.b = z->regs.b;
        break; // ld b,b
    case 0x41:
        z->regs.b = z->regs.c;
        break; // ld b,c
    case 0x42:
        z->regs.b = z->regs.d;
        break; // ld b,d
    case 0x43:
        z->regs.b = z->regs.e;
        break; // ld b,e
    case 0x44:
        z->regs.b = z->regs.h;
        break; // ld b,h
    case 0x45:
        z->regs.b = z->regs.l;
        break; // ld b,l

    case 0x4F:
        z->regs.c = z->regs.a;
        break; // ld c,a
    case 0x48:
        z->regs.c = z->regs.b;
        break; // ld c,b
    case 0x49:
        z->regs.c = z->regs.c;
        break; // ld c,c
    case 0x4A:
        z->regs.c = z->regs.d;
        break; // ld c,d
    case 0x4B:
        z->regs.c = z->regs.e;
        break; // ld c,e
    case 0x4C:
        z->regs.c = z->regs.h;
        break; // ld c,h
    case 0x4D:
        z->regs.c = z->regs.l;
        break; // ld c,l

    case 0x57:
        z->regs.d = z->regs.a;
        break; // ld d,a
    case 0x50:
        z->regs.d = z->regs.b;
        break; // ld d,b
    case 0x51:
        z->regs.d = z->regs.c;
        break; // ld d,c
    case 0x52:
        z->regs.d = z->regs.d;
        break; // ld d,d
    case 0x53:
        z->regs.d = z->regs.e;
        break; // ld d,e
    case 0x54:
        z->regs.d = z->regs.h;
        break; // ld d,h
    case 0x55:
        z->regs.d = z->regs.l;
        break; // ld d,l

    case 0x5F:
        z->regs.e = z->regs.a;
        break; // ld e,a
    case 0x58:
        z->regs.e = z->regs.b;
        break; // ld e,b
    case 0x59:
        z->regs.e = z->regs.c;
        break; // ld e,c
    case 0x5A:
        z->regs.e = z->regs.d;
        break; // ld e,d
    case 0x5B:
        z->regs.e = z->regs.e;
        break; // ld e,e
    case 0x5C:
        z->regs.e = z->regs.h;
        break; // ld e,h
    case 0x5D:
        z->regs.e = z->regs.l;
        break; // ld e,l

    case 0x67:
        z->regs.h = z->regs.a;
        break; // ld h,a
    case 0x60:
        z->regs.h = z->regs.b;
        break; // ld h,b
    case 0x61:
        z->regs.h = z->regs.c;
        break; // ld h,c
    case 0x62:
        z->regs.h = z->regs.d;
        break; // ld h,d
    case 0x63:
        z->regs.h = z->regs.e;
        break; // ld h,e
    case 0x64:
        z->regs.h = z->regs.h;
        break; // ld h,h
    case 0x65:
        z->regs.h = z->regs.l;
        break; // ld h,l

    case 0x6F:
        z->regs.l = z->regs.a;
        break; // ld l,a
    case 0x68:
        z->regs.l = z->regs.b;
        break; // ld l,b
    case 0x69:
        z->regs.l = z->regs.c;
        break; // ld l,c
    case 0x6A:
        z->regs.l = z->regs.d;
        break; // ld l,d
    case 0x6B:
        z->regs.l = z->regs.e;
        break; // ld l,e
    case 0x6C:
        z->regs.l = z->regs.h;
        break; // ld l,h
    case 0x6D:
        z->regs.l = z->regs.l;
        break; // ld l,l

    case 0x7E:
        z->regs.a = rb(z, get_hl(z));
        break; // ld a,(hl)
    case 0x46:
        z->regs.b = rb(z, get_hl(z));
        break; // ld b,(hl)
    case 0x4E:
        z->regs.c = rb(z, get_hl(z));
        break; // ld c,(hl)
    case 0x56:
        z->regs.d = rb(z, get_hl(z));
        break; // ld d,(hl)
    case 0x5E:
        z->regs.e = rb(z, get_hl(z));
        break; // ld e,(hl)
    case 0x66:
        z->regs.h = rb(z, get_hl(z));
        break; // ld h,(hl)
    case 0x6E:
        z->regs.l = rb(z, get_hl(z));
        break; // ld l,(hl)

    case 0x77:
        wb(z, get_hl(z), z->regs.a);
        break; // ld (hl),a
    case 0x70:
        wb(z, get_hl(z), z->regs.b);
        break; // ld (hl),b
    case 0x71:
        wb(z, get_hl(z), z->regs.c);
        break; // ld (hl),c
    case 0x72:
        wb(z, get_hl(z), z->regs.d);
        break; // ld (hl),d
    case 0x73:
        wb(z, get_hl(z), z->regs.e);
        break; // ld (hl),e
    case 0x74:
        wb(z, get_hl(z), z->regs.h);
        break; // ld (hl),h
    case 0x75:
        wb(z, get_hl(z), z->regs.l);
        break; // ld (hl),l

    case 0x3E:
        z->regs.a = nextb(z);
        break; // ld a,*
    case 0x06:
        z->regs.b = nextb(z);
        break; // ld b,*
    case 0x0E:
        z->regs.c = nextb(z);
        break; // ld c,*
    case 0x16:
        z->regs.d = nextb(z);
        break; // ld d,*
    case 0x1E:
        z->regs.e = nextb(z);
        break; // ld e,*
    case 0x26:
        z->regs.h = nextb(z);
        break; // ld h,*
    case 0x2E:
        z->regs.l = nextb(z);
        break; // ld l,*
    case 0x36:
        wb(z, get_hl(z), nextb(z));
        break; // ld (hl),*

    case 0x0A:
        z->regs.a = rb(z, get_bc(z));
        z->regs.mem_ptr = get_bc(z) + 1;
        break; // ld a,(bc)
    case 0x1A:
        z->regs.a = rb(z, get_de(z));
        z->regs.mem_ptr = get_de(z) + 1;
        break; // ld a,(de)
    case 0x3A:
    {
        const uint16_t addr = nextw(z);
        z->regs.a = rb(z, addr);
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld a,(**)

    case 0x02:
        wb(z, get_bc(z), z->regs.a);
        z->regs.mem_ptr = (z->regs.a << 8) | ((get_bc(z) + 1) & 0xFF);
        break; // ld (bc),a

    case 0x12:
        wb(z, get_de(z), z->regs.a);
        z->regs.mem_ptr = (z->regs.a << 8) | ((get_de(z) + 1) & 0xFF);
        break; // ld (de),a

    case 0x32:
    {
        const uint16_t addr = nextw(z);
        wb(z, addr, z->regs.a);
        z->regs.mem_ptr = (z->regs.a << 8) | ((addr + 1) & 0xFF);
    }
    break; // ld (**),a

    case 0x01:
        set_bc(z, nextw(z));
        break; // ld bc,**
    case 0x11:
        set_de(z, nextw(z));
        break; // ld de,**
    case 0x21:
        set_hl(z, nextw(z));
        break; // ld hl,**
    case 0x31:
        z->regs.sp = nextw(z);
        break; // ld sp,**

    case 0x2A:
    {
        const uint16_t addr = nextw(z);
        set_hl(z, rw(z, addr));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld hl,(**)

    case 0x22:
    {
        const uint16_t addr = nextw(z);
        ww(z, addr, get_hl(z));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld (**),hl

    case 0xF9:
        z->regs.sp = get_hl(z);
        break; // ld sp,hl

    case 0xEB:
    {
        const uint16_t de = get_de(z);
        set_de(z, get_hl(z));
        set_hl(z, de);
    }
    break; // ex de,hl

    case 0xE3:
    {
        const uint16_t val = rw(z, z->regs.sp);
        ww(z, z->regs.sp, get_hl(z));
        set_hl(z, val);
        z->regs.mem_ptr = val;
    }
    break; // ex (sp),hl

    case 0x87:
        z->regs.a = addb(z, z->regs.a, z->regs.a, 0);
        break; // add a,a
    case 0x80:
        z->regs.a = addb(z, z->regs.a, z->regs.b, 0);
        break; // add a,b
    case 0x81:
        z->regs.a = addb(z, z->regs.a, z->regs.c, 0);
        break; // add a,c
    case 0x82:
        z->regs.a = addb(z, z->regs.a, z->regs.d, 0);
        break; // add a,d
    case 0x83:
        z->regs.a = addb(z, z->regs.a, z->regs.e, 0);
        break; // add a,e
    case 0x84:
        z->regs.a = addb(z, z->regs.a, z->regs.h, 0);
        break; // add a,h
    case 0x85:
        z->regs.a = addb(z, z->regs.a, z->regs.l, 0);
        break; // add a,l
    case 0x86:
        z->regs.a = addb(z, z->regs.a, rb(z, get_hl(z)), 0);
        break; // add a,(hl)
    case 0xC6:
        z->regs.a = addb(z, z->regs.a, nextb(z), 0);
        break; // add a,*

    case 0x8F:
        z->regs.a = addb(z, z->regs.a, z->regs.a, z->regs.cf);
        break; // adc a,a
    case 0x88:
        z->regs.a = addb(z, z->regs.a, z->regs.b, z->regs.cf);
        break; // adc a,b
    case 0x89:
        z->regs.a = addb(z, z->regs.a, z->regs.c, z->regs.cf);
        break; // adc a,c
    case 0x8A:
        z->regs.a = addb(z, z->regs.a, z->regs.d, z->regs.cf);
        break; // adc a,d
    case 0x8B:
        z->regs.a = addb(z, z->regs.a, z->regs.e, z->regs.cf);
        break; // adc a,e
    case 0x8C:
        z->regs.a = addb(z, z->regs.a, z->regs.h, z->regs.cf);
        break; // adc a,h
    case 0x8D:
        z->regs.a = addb(z, z->regs.a, z->regs.l, z->regs.cf);
        break; // adc a,l
    case 0x8E:
        z->regs.a = addb(z, z->regs.a, rb(z, get_hl(z)), z->regs.cf);
        break; // adc a,(hl)
    case 0xCE:
        z->regs.a = addb(z, z->regs.a, nextb(z), z->regs.cf);
        break; // adc a,*

    case 0x97:
        z->regs.a = subb(z, z->regs.a, z->regs.a, 0);
        break; // sub a,a
    case 0x90:
        z->regs.a = subb(z, z->regs.a, z->regs.b, 0);
        break; // sub a,b
    case 0x91:
        z->regs.a = subb(z, z->regs.a, z->regs.c, 0);
        break; // sub a,c
    case 0x92:
        z->regs.a = subb(z, z->regs.a, z->regs.d, 0);
        break; // sub a,d
    case 0x93:
        z->regs.a = subb(z, z->regs.a, z->regs.e, 0);
        break; // sub a,e
    case 0x94:
        z->regs.a = subb(z, z->regs.a, z->regs.h, 0);
        break; // sub a,h
    case 0x95:
        z->regs.a = subb(z, z->regs.a, z->regs.l, 0);
        break; // sub a,l
    case 0x96:
        z->regs.a = subb(z, z->regs.a, rb(z, get_hl(z)), 0);
        break; // sub a,(hl)
    case 0xD6:
        z->regs.a = subb(z, z->regs.a, nextb(z), 0);
        break; // sub a,*

    case 0x9F:
        z->regs.a = subb(z, z->regs.a, z->regs.a, z->regs.cf);
        break; // sbc a,a
    case 0x98:
        z->regs.a = subb(z, z->regs.a, z->regs.b, z->regs.cf);
        break; // sbc a,b
    case 0x99:
        z->regs.a = subb(z, z->regs.a, z->regs.c, z->regs.cf);
        break; // sbc a,c
    case 0x9A:
        z->regs.a = subb(z, z->regs.a, z->regs.d, z->regs.cf);
        break; // sbc a,d
    case 0x9B:
        z->regs.a = subb(z, z->regs.a, z->regs.e, z->regs.cf);
        break; // sbc a,e
    case 0x9C:
        z->regs.a = subb(z, z->regs.a, z->regs.h, z->regs.cf);
        break; // sbc a,h
    case 0x9D:
        z->regs.a = subb(z, z->regs.a, z->regs.l, z->regs.cf);
        break; // sbc a,l
    case 0x9E:
        z->regs.a = subb(z, z->regs.a, rb(z, get_hl(z)), z->regs.cf);
        break; // sbc a,(hl)
    case 0xDE:
        z->regs.a = subb(z, z->regs.a, nextb(z), z->regs.cf);
        break; // sbc a,*

    case 0x09:
        addhl(z, get_bc(z));
        break; // add hl,bc
    case 0x19:
        addhl(z, get_de(z));
        break; // add hl,de
    case 0x29:
        addhl(z, get_hl(z));
        break; // add hl,hl
    case 0x39:
        addhl(z, z->regs.sp);
        break; // add hl,sp

    case 0xF3:
        z->regs.iff1 = 0;
        z->regs.iff2 = 0;
        break; // di
    case 0xFB:
        z->regs.iff_delay = 1;
        break; // ei
    case 0x00:
        break; // nop
    case 0x76:
        z->halted = 1;
        break; // halt

    case 0x3C:
        z->regs.a = inc(z, z->regs.a);
        break; // inc a
    case 0x04:
        z->regs.b = inc(z, z->regs.b);
        break; // inc b
    case 0x0C:
        z->regs.c = inc(z, z->regs.c);
        break; // inc c
    case 0x14:
        z->regs.d = inc(z, z->regs.d);
        break; // inc d
    case 0x1C:
        z->regs.e = inc(z, z->regs.e);
        break; // inc e
    case 0x24:
        z->regs.h = inc(z, z->regs.h);
        break; // inc h
    case 0x2C:
        z->regs.l = inc(z, z->regs.l);
        break; // inc l
    case 0x34:
    {
        uint8_t result = inc(z, rb(z, get_hl(z)));
        wb(z, get_hl(z), result);
    }
    break; // inc (hl)

    case 0x3D:
        z->regs.a = dec(z, z->regs.a);
        break; // dec a
    case 0x05:
        z->regs.b = dec(z, z->regs.b);
        break; // dec b
    case 0x0D:
        z->regs.c = dec(z, z->regs.c);
        break; // dec c
    case 0x15:
        z->regs.d = dec(z, z->regs.d);
        break; // dec d
    case 0x1D:
        z->regs.e = dec(z, z->regs.e);
        break; // dec e
    case 0x25:
        z->regs.h = dec(z, z->regs.h);
        break; // dec h
    case 0x2D:
        z->regs.l = dec(z, z->regs.l);
        break; // dec l
    case 0x35:
    {
        uint8_t result = dec(z, rb(z, get_hl(z)));
        wb(z, get_hl(z), result);
    }
    break; // dec (hl)

    case 0x03:
        set_bc(z, get_bc(z) + 1);
        break; // inc bc
    case 0x13:
        set_de(z, get_de(z) + 1);
        break; // inc de
    case 0x23:
        set_hl(z, get_hl(z) + 1);
        break; // inc hl
    case 0x33:
        z->regs.sp = z->regs.sp + 1;
        break; // inc sp
    case 0x0B:
        set_bc(z, get_bc(z) - 1);
        break; // dec bc
    case 0x1B:
        set_de(z, get_de(z) - 1);
        break; // dec de
    case 0x2B:
        set_hl(z, get_hl(z) - 1);
        break; // dec hl
    case 0x3B:
        z->regs.sp = z->regs.sp - 1;
        break; // dec sp

    case 0x27:
        daa(z);
        break; // daa

    case 0x2F:
        z->regs.a = ~z->regs.a;
        z->regs.nf = 1;
        z->regs.hf = 1;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
        break; // cpl

    case 0x37:
        z->regs.cf = 1;
        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
        break; // scf

    case 0x3F:
        z->regs.hf = z->regs.cf;
        z->regs.cf = !z->regs.cf;
        z->regs.nf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
        break; // ccf

    case 0x07:
    {
        z->regs.cf = z->regs.a >> 7;
        z->regs.a = (z->regs.a << 1) | z->regs.cf;
        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
    }
    break; // rlca (rotate left)

    case 0x0F:
    {
        z->regs.cf = z->regs.a & 1;
        z->regs.a = (z->regs.a >> 1) | (z->regs.cf << 7);
        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
    }
    break; // rrca (rotate right)

    case 0x17:
    {
        const bool cy = z->regs.cf;
        z->regs.cf = z->regs.a >> 7;
        z->regs.a = (z->regs.a << 1) | cy;
        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
    }
    break; // rla

    case 0x1F:
    {
        const bool cy = z->regs.cf;
        z->regs.cf = z->regs.a & 1;
        z->regs.a = (z->regs.a >> 1) | (cy << 7);
        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
    }
    break; // rra

    case 0xA7:
        land(z, z->regs.a);
        break; // and a
    case 0xA0:
        land(z, z->regs.b);
        break; // and b
    case 0xA1:
        land(z, z->regs.c);
        break; // and c
    case 0xA2:
        land(z, z->regs.d);
        break; // and d
    case 0xA3:
        land(z, z->regs.e);
        break; // and e
    case 0xA4:
        land(z, z->regs.h);
        break; // and h
    case 0xA5:
        land(z, z->regs.l);
        break; // and l
    case 0xA6:
        land(z, rb(z, get_hl(z)));
        break; // and (hl)
    case 0xE6:
        land(z, nextb(z));
        break; // and *

    case 0xAF:
        lxor(z, z->regs.a);
        break; // xor a
    case 0xA8:
        lxor(z, z->regs.b);
        break; // xor b
    case 0xA9:
        lxor(z, z->regs.c);
        break; // xor c
    case 0xAA:
        lxor(z, z->regs.d);
        break; // xor d
    case 0xAB:
        lxor(z, z->regs.e);
        break; // xor e
    case 0xAC:
        lxor(z, z->regs.h);
        break; // xor h
    case 0xAD:
        lxor(z, z->regs.l);
        break; // xor l
    case 0xAE:
        lxor(z, rb(z, get_hl(z)));
        break; // xor (hl)
    case 0xEE:
        lxor(z, nextb(z));
        break; // xor *

    case 0xB7:
        lor(z, z->regs.a);
        break; // or a
    case 0xB0:
        lor(z, z->regs.b);
        break; // or b
    case 0xB1:
        lor(z, z->regs.c);
        break; // or c
    case 0xB2:
        lor(z, z->regs.d);
        break; // or d
    case 0xB3:
        lor(z, z->regs.e);
        break; // or e
    case 0xB4:
        lor(z, z->regs.h);
        break; // or h
    case 0xB5:
        lor(z, z->regs.l);
        break; // or l
    case 0xB6:
        lor(z, rb(z, get_hl(z)));
        break; // or (hl)
    case 0xF6:
        lor(z, nextb(z));
        break; // or *

    case 0xBF:
        cp(z, z->regs.a);
        break; // cp a
    case 0xB8:
        cp(z, z->regs.b);
        break; // cp b
    case 0xB9:
        cp(z, z->regs.c);
        break; // cp c
    case 0xBA:
        cp(z, z->regs.d);
        break; // cp d
    case 0xBB:
        cp(z, z->regs.e);
        break; // cp e
    case 0xBC:
        cp(z, z->regs.h);
        break; // cp h
    case 0xBD:
        cp(z, z->regs.l);
        break; // cp l
    case 0xBE:
        cp(z, rb(z, get_hl(z)));
        break; // cp (hl)
    case 0xFE:
        cp(z, nextb(z));
        break; // cp *

    case 0xC3:
        jump(z, nextw(z));
        break; // jm **
    case 0xC2:
        cond_jump(z, z->regs.zf == 0);
        break; // jp nz, **
    case 0xCA:
        cond_jump(z, z->regs.zf == 1);
        break; // jp z, **
    case 0xD2:
        cond_jump(z, z->regs.cf == 0);
        break; // jp nc, **
    case 0xDA:
        cond_jump(z, z->regs.cf == 1);
        break; // jp c, **
    case 0xE2:
        cond_jump(z, z->regs.pf == 0);
        break; // jp po, **
    case 0xEA:
        cond_jump(z, z->regs.pf == 1);
        break; // jp pe, **
    case 0xF2:
        cond_jump(z, z->regs.sf == 0);
        break; // jp p, **
    case 0xFA:
        cond_jump(z, z->regs.sf == 1);
        break; // jp m, **

    case 0x10:
        cond_jr(z, --z->regs.b != 0);
        break; // djnz *
    case 0x18:
        z->regs.pc += (int8_t)nextb(z);
        break; // jr *
    case 0x20:
        cond_jr(z, z->regs.zf == 0);
        break; // jr nz, *
    case 0x28:
        cond_jr(z, z->regs.zf == 1);
        break; // jr z, *
    case 0x30:
        cond_jr(z, z->regs.cf == 0);
        break; // jr nc, *
    case 0x38:
        cond_jr(z, z->regs.cf == 1);
        break; // jr c, *

    case 0xE9:
        z->regs.pc = get_hl(z);
        break; // jp (hl)
    case 0xCD:
        call(z, nextw(z));
        break; // call

    case 0xC4:
        cond_call(z, z->regs.zf == 0);
        break; // cnz
    case 0xCC:
        cond_call(z, z->regs.zf == 1);
        break; // cz
    case 0xD4:
        cond_call(z, z->regs.cf == 0);
        break; // cnc
    case 0xDC:
        cond_call(z, z->regs.cf == 1);
        break; // cc
    case 0xE4:
        cond_call(z, z->regs.pf == 0);
        break; // cpo
    case 0xEC:
        cond_call(z, z->regs.pf == 1);
        break; // cpe
    case 0xF4:
        cond_call(z, z->regs.sf == 0);
        break; // cp
    case 0xFC:
        cond_call(z, z->regs.sf == 1);
        break; // cm

    case 0xC9:
        ret(z);
        break; // ret
    case 0xC0:
        cond_ret(z, z->regs.zf == 0);
        break; // ret nz
    case 0xC8:
        cond_ret(z, z->regs.zf == 1);
        break; // ret z
    case 0xD0:
        cond_ret(z, z->regs.cf == 0);
        break; // ret nc
    case 0xD8:
        cond_ret(z, z->regs.cf == 1);
        break; // ret c
    case 0xE0:
        cond_ret(z, z->regs.pf == 0);
        break; // ret po
    case 0xE8:
        cond_ret(z, z->regs.pf == 1);
        break; // ret pe
    case 0xF0:
        cond_ret(z, z->regs.sf == 0);
        break; // ret p
    case 0xF8:
        cond_ret(z, z->regs.sf == 1);
        break; // ret m

    case 0xC7:
        call(z, 0x00);
        break; // rst 0
    case 0xCF:
        call(z, 0x08);
        break; // rst 1
    case 0xD7:
        call(z, 0x10);
        break; // rst 2
    case 0xDF:
        call(z, 0x18);
        break; // rst 3
    case 0xE7:
        call(z, 0x20);
        break; // rst 4
    case 0xEF:
        call(z, 0x28);
        break; // rst 5
    case 0xF7:
        call(z, 0x30);
        break; // rst 6
    case 0xFF:
        call(z, 0x38);
        break; // rst 7

    case 0xC5:
        pushw(z, get_bc(z));
        break; // push bc
    case 0xD5:
        pushw(z, get_de(z));
        break; // push de
    case 0xE5:
        pushw(z, get_hl(z));
        break; // push hl
    case 0xF5:
        pushw(z, (z->regs.a << 8) | get_f(z));
        break; // push af

    case 0xC1:
        set_bc(z, popw(z));
        break; // pop bc
    case 0xD1:
        set_de(z, popw(z));
        break; // pop de
    case 0xE1:
        set_hl(z, popw(z));
        break; // pop hl
    case 0xF1:
    {
        uint16_t val = popw(z);
        z->regs.a = val >> 8;
        set_f(z, val & 0xFF);
    }
    break; // pop af

    case 0xDB:
    {
        const uint8_t port = nextb(z);
        const uint8_t a = z->regs.a;
        z->regs.a = z80_read_io_internal(z, port);
        z->regs.mem_ptr = (a << 8) | (z->regs.a + 1);
    }
    break; // in a,(n)

    case 0xD3:
    {
        const uint8_t port = nextb(z);
        z80_write_io_internal(z, port, z->regs.a);
        z->regs.mem_ptr = (port + 1) | (z->regs.a << 8);
    }
    break; // out (n), a

    case 0x08:
    {
        uint8_t a = z->regs.a;
        uint8_t f = get_f(z);

        z->regs.a = z->regs.a_;
        set_f(z, z->regs.f_);

        z->regs.a_ = a;
        z->regs.f_ = f;
    }
    break; // ex af,af'
    case 0xD9:
    {
        uint8_t b = z->regs.b, c = z->regs.c, d = z->regs.d, e = z->regs.e, h = z->regs.h, l = z->regs.l;

        z->regs.b = z->regs.b_;
        z->regs.c = z->regs.c_;
        z->regs.d = z->regs.d_;
        z->regs.e = z->regs.e_;
        z->regs.h = z->regs.h_;
        z->regs.l = z->regs.l_;

        z->regs.b_ = b;
        z->regs.c_ = c;
        z->regs.d_ = d;
        z->regs.e_ = e;
        z->regs.h_ = h;
        z->regs.l_ = l;
    }
    break; // exx

    case 0xCB:
        exec_opcode_cb(z, nextb(z));
        break;
    case 0xED:
        exec_opcode_ed(z, nextb(z));
        break;
    case 0xDD:
        exec_opcode_ddfd(z, nextb(z), &z->regs.ix);
        break;
    case 0xFD:
        exec_opcode_ddfd(z, nextb(z), &z->regs.iy);
        break;

    default:
        fprintf(stderr, "unknown opcode %02X\n", opcode);
        break;
    }

    return z->cyc - cyc_before;
}

// executes a DD/FD opcode (IZ = IX or IY)
int exec_opcode_ddfd(z80_emulator_t *const z, uint8_t opcode, uint16_t *const iz)
{
    uint64_t cyc_before = z->cyc;
    z->cyc += cyc_ddfd[opcode];
    inc_r(z);

#define IZD displace(z, *iz, nextb(z))
#define IZH (*iz >> 8)
#define IZL (*iz & 0xFF)

    switch (opcode)
    {
    case 0xE1:
        *iz = popw(z);
        break; // pop iz
    case 0xE5:
        pushw(z, *iz);
        break; // push iz

    case 0xE9:
        jump(z, *iz);
        break; // jp iz

    case 0x09:
        addiz(z, iz, get_bc(z));
        break; // add iz,bc
    case 0x19:
        addiz(z, iz, get_de(z));
        break; // add iz,de
    case 0x29:
        addiz(z, iz, *iz);
        break; // add iz,iz
    case 0x39:
        addiz(z, iz, z->regs.sp);
        break; // add iz,sp

    case 0x84:
        z->regs.a = addb(z, z->regs.a, IZH, 0);
        break; // add a,izh
    case 0x85:
        z->regs.a = addb(z, z->regs.a, *iz & 0xFF, 0);
        break; // add a,izl
    case 0x8C:
        z->regs.a = addb(z, z->regs.a, IZH, z->regs.cf);
        break; // adc a,izh
    case 0x8D:
        z->regs.a = addb(z, z->regs.a, *iz & 0xFF, z->regs.cf);
        break; // adc a,izl

    case 0x86:
        z->regs.a = addb(z, z->regs.a, rb(z, IZD), 0);
        break; // add a,(iz+*)
    case 0x8E:
        z->regs.a = addb(z, z->regs.a, rb(z, IZD), z->regs.cf);
        break; // adc a,(iz+*)
    case 0x96:
        z->regs.a = subb(z, z->regs.a, rb(z, IZD), 0);
        break; // sub (iz+*)
    case 0x9E:
        z->regs.a = subb(z, z->regs.a, rb(z, IZD), z->regs.cf);
        break; // sbc (iz+*)

    case 0x94:
        z->regs.a = subb(z, z->regs.a, IZH, 0);
        break; // sub izh
    case 0x95:
        z->regs.a = subb(z, z->regs.a, *iz & 0xFF, 0);
        break; // sub izl
    case 0x9C:
        z->regs.a = subb(z, z->regs.a, IZH, z->regs.cf);
        break; // sbc izh
    case 0x9D:
        z->regs.a = subb(z, z->regs.a, *iz & 0xFF, z->regs.cf);
        break; // sbc izl

    case 0xA6:
        land(z, rb(z, IZD));
        break; // and (iz+*)
    case 0xA4:
        land(z, IZH);
        break; // and izh
    case 0xA5:
        land(z, *iz & 0xFF);
        break; // and izl

    case 0xAE:
        lxor(z, rb(z, IZD));
        break; // xor (iz+*)
    case 0xAC:
        lxor(z, IZH);
        break; // xor izh
    case 0xAD:
        lxor(z, *iz & 0xFF);
        break; // xor izl

    case 0xB6:
        lor(z, rb(z, IZD));
        break; // or (iz+*)
    case 0xB4:
        lor(z, IZH);
        break; // or izh
    case 0xB5:
        lor(z, *iz & 0xFF);
        break; // or izl

    case 0xBE:
        cp(z, rb(z, IZD));
        break; // cp (iz+*)
    case 0xBC:
        cp(z, IZH);
        break; // cp izh
    case 0xBD:
        cp(z, *iz & 0xFF);
        break; // cp izl

    case 0x23:
        *iz += 1;
        break; // inc iz
    case 0x2B:
        *iz -= 1;
        break; // dec iz

    case 0x34:
    {
        uint16_t addr = IZD;
        wb(z, addr, inc(z, rb(z, addr)));
    }
    break; // inc (iz+*)

    case 0x35:
    {
        uint16_t addr = IZD;
        wb(z, addr, dec(z, rb(z, addr)));
    }
    break; // dec (iz+*)

    case 0x24:
        *iz = IZL | ((inc(z, IZH)) << 8);
        break; // inc izh
    case 0x25:
        *iz = IZL | ((dec(z, IZH)) << 8);
        break; // dec izh
    case 0x2C:
        *iz = (IZH << 8) | inc(z, IZL);
        break; // inc izl
    case 0x2D:
        *iz = (IZH << 8) | dec(z, IZL);
        break; // dec izl

    case 0x2A:
        *iz = rw(z, nextw(z));
        break; // ld iz,(**)
    case 0x22:
        ww(z, nextw(z), *iz);
        break; // ld (**),iz
    case 0x21:
        *iz = nextw(z);
        break; // ld iz,**

    case 0x36:
    {
        uint16_t addr = IZD;
        wb(z, addr, nextb(z));
    }
    break; // ld (iz+*),*

    case 0x70:
        wb(z, IZD, z->regs.b);
        break; // ld (iz+*),b
    case 0x71:
        wb(z, IZD, z->regs.c);
        break; // ld (iz+*),c
    case 0x72:
        wb(z, IZD, z->regs.d);
        break; // ld (iz+*),d
    case 0x73:
        wb(z, IZD, z->regs.e);
        break; // ld (iz+*),e
    case 0x74:
        wb(z, IZD, z->regs.h);
        break; // ld (iz+*),h
    case 0x75:
        wb(z, IZD, z->regs.l);
        break; // ld (iz+*),l
    case 0x77:
        wb(z, IZD, z->regs.a);
        break; // ld (iz+*),a

    case 0x46:
        z->regs.b = rb(z, IZD);
        break; // ld b,(iz+*)
    case 0x4E:
        z->regs.c = rb(z, IZD);
        break; // ld c,(iz+*)
    case 0x56:
        z->regs.d = rb(z, IZD);
        break; // ld d,(iz+*)
    case 0x5E:
        z->regs.e = rb(z, IZD);
        break; // ld e,(iz+*)
    case 0x66:
        z->regs.h = rb(z, IZD);
        break; // ld h,(iz+*)
    case 0x6E:
        z->regs.l = rb(z, IZD);
        break; // ld l,(iz+*)
    case 0x7E:
        z->regs.a = rb(z, IZD);
        break; // ld a,(iz+*)

    case 0x44:
        z->regs.b = IZH;
        break; // ld b,izh
    case 0x4C:
        z->regs.c = IZH;
        break; // ld c,izh
    case 0x54:
        z->regs.d = IZH;
        break; // ld d,izh
    case 0x5C:
        z->regs.e = IZH;
        break; // ld e,izh
    case 0x7C:
        z->regs.a = IZH;
        break; // ld a,izh
    case 0x45:
        z->regs.b = IZL;
        break; // ld b,izl
    case 0x4D:
        z->regs.c = IZL;
        break; // ld c,izl
    case 0x55:
        z->regs.d = IZL;
        break; // ld d,izl
    case 0x5D:
        z->regs.e = IZL;
        break; // ld e,izl
    case 0x7D:
        z->regs.a = IZL;
        break; // ld a,izl

    case 0x60:
        *iz = IZL | (z->regs.b << 8);
        break; // ld izh,b
    case 0x61:
        *iz = IZL | (z->regs.c << 8);
        break; // ld izh,c
    case 0x62:
        *iz = IZL | (z->regs.d << 8);
        break; // ld izh,d
    case 0x63:
        *iz = IZL | (z->regs.e << 8);
        break; // ld izh,e
    case 0x64:
        break; // ld izh,izh
    case 0x65:
        *iz = (IZL << 8) | IZL;
        break; // ld izh,izl
    case 0x67:
        *iz = IZL | (z->regs.a << 8);
        break; // ld izh,a
    case 0x26:
        *iz = IZL | (nextb(z) << 8);
        break; // ld izh,*

    case 0x68:
        *iz = (IZH << 8) | z->regs.b;
        break; // ld izl,b
    case 0x69:
        *iz = (IZH << 8) | z->regs.c;
        break; // ld izl,c
    case 0x6A:
        *iz = (IZH << 8) | z->regs.d;
        break; // ld izl,d
    case 0x6B:
        *iz = (IZH << 8) | z->regs.e;
        break; // ld izl,e
    case 0x6C:
        *iz = (IZH << 8) | IZH;
        break; // ld izl,izh
    case 0x6D:
        break; // ld izl,izl
    case 0x6F:
        *iz = (IZH << 8) | z->regs.a;
        break; // ld izl,a
    case 0x2E:
        *iz = (IZH << 8) | nextb(z);
        break; // ld izl,*

    case 0xF9:
        z->regs.sp = *iz;
        break; // ld sp,iz

    case 0xE3:
    {
        const uint16_t val = rw(z, z->regs.sp);
        ww(z, z->regs.sp, *iz);
        *iz = val;
        z->regs.mem_ptr = val;
    }
    break; // ex (sp),iz

    case 0xCB:
    {
        uint16_t addr = IZD;
        uint8_t op = nextb(z);
        exec_opcode_dcb(z, op, addr);
    }
    break;

    default:
    {
        // any other FD/DD opcode behaves as a non-prefixed opcode:
        exec_opcode(z, opcode);
        // R should not be incremented twice:
        z->regs.r = (z->regs.r & 0x80) | ((z->regs.r - 1) & 0x7f);
    }
    break;
    }

#undef IZD
#undef IZH
#undef IZL

    return z->cyc - cyc_before;
}

// executes a CB opcode
int exec_opcode_cb(z80_emulator_t *const z, uint8_t opcode)
{
    uint64_t cyc_before = z->cyc;
    z->cyc += 8;
    inc_r(z);

    // decoding instructions from http://z80.info/decoding.htm#cb
    uint8_t x_ = (opcode >> 6) & 3; // 0b11
    uint8_t y_ = (opcode >> 3) & 7; // 0b111
    uint8_t z_ = opcode & 7;        // 0b111

    uint8_t hl = 0;
    uint8_t *reg = 0;
    switch (z_)
    {
    case 0:
        reg = &z->regs.b;
        break;
    case 1:
        reg = &z->regs.c;
        break;
    case 2:
        reg = &z->regs.d;
        break;
    case 3:
        reg = &z->regs.e;
        break;
    case 4:
        reg = &z->regs.h;
        break;
    case 5:
        reg = &z->regs.l;
        break;
    case 6:
        hl = rb(z, get_hl(z));
        reg = &hl;
        break;
    case 7:
        reg = &z->regs.a;
        break;
    }

    switch (x_)
    {
    case 0:
    {
        switch (y_)
        {
        case 0:
            *reg = cb_rlc(z, *reg);
            break;
        case 1:
            *reg = cb_rrc(z, *reg);
            break;
        case 2:
            *reg = cb_rl(z, *reg);
            break;
        case 3:
            *reg = cb_rr(z, *reg);
            break;
        case 4:
            *reg = cb_sla(z, *reg);
            break;
        case 5:
            *reg = cb_sra(z, *reg);
            break;
        case 6:
            *reg = cb_sll(z, *reg);
            break;
        case 7:
            *reg = cb_srl(z, *reg);
            break;
        }
    }
    break; // rot[y] r[z]
    case 1:
    { // BIT y, r[z]
        cb_bit(z, *reg, y_);

        // in bit (hl), x/y flags are handled differently:
        if (z_ == 6)
        {
            z->regs.yf = GET_BIT(5, z->regs.mem_ptr >> 8);
            z->regs.xf = GET_BIT(3, z->regs.mem_ptr >> 8);
            z->cyc += 4;
        }
    }
    break;
    case 2:
        *reg &= ~(1 << y_);
        break; // RES y, r[z]
    case 3:
        *reg |= 1 << y_;
        break; // SET y, r[z]
    }

    if ((x_ == 0 || x_ == 2 || x_ == 3) && z_ == 6)
    {
        z->cyc += 7;
    }

    if (reg == &hl)
    {
        wb(z, get_hl(z), hl);
    }

    return z->cyc - cyc_before;
}

// executes a displaced CB opcode (DDCB or FDCB)
int exec_opcode_dcb(z80_emulator_t *const z, uint8_t opcode, uint16_t addr)
{
    uint64_t cyc_before = z->cyc;
    uint8_t val = rb(z, addr);
    uint8_t result = 0;

    // decoding instructions from http://z80.info/decoding.htm#ddcb
    uint8_t x_ = (opcode >> 6) & 3; // 0b11
    uint8_t y_ = (opcode >> 3) & 7; // 0b111
    uint8_t z_ = opcode & 7;        // 0b111

    switch (x_)
    {
    case 0:
    {
        // rot[y] (iz+d)
        switch (y_)
        {
        case 0:
            result = cb_rlc(z, val);
            break;
        case 1:
            result = cb_rrc(z, val);
            break;
        case 2:
            result = cb_rl(z, val);
            break;
        case 3:
            result = cb_rr(z, val);
            break;
        case 4:
            result = cb_sla(z, val);
            break;
        case 5:
            result = cb_sra(z, val);
            break;
        case 6:
            result = cb_sll(z, val);
            break;
        case 7:
            result = cb_srl(z, val);
            break;
        }
    }
    break;
    case 1:
    {
        result = cb_bit(z, val, y_);
        z->regs.yf = GET_BIT(5, addr >> 8);
        z->regs.xf = GET_BIT(3, addr >> 8);
    }
    break; // bit y,(iz+d)
    case 2:
        result = val & ~(1 << y_);
        break; // res y, (iz+d)
    case 3:
        result = val | (1 << y_);
        break; // set y, (iz+d)

    default:
        fprintf(stderr, "unknown XYCB opcode: %02X\n", opcode);
        break;
    }

    // ld r[z], rot[y] (iz+d)
    // ld r[z], res y,(iz+d)
    // ld r[z], set y,(iz+d)
    if (x_ != 1 && z_ != 6)
    {
        switch (z_)
        {
        case 0:
            z->regs.b = result;
            break;
        case 1:
            z->regs.c = result;
            break;
        case 2:
            z->regs.d = result;
            break;
        case 3:
            z->regs.e = result;
            break;
        case 4:
            z->regs.h = result;
            break;
        case 5:
            z->regs.l = result;
            break;
        case 6:
            wb(z, get_hl(z), result);
            break;
        case 7:
            z->regs.a = result;
            break;
        }
    }

    if (x_ == 1)
    {
        // bit instructions take 20 cycles, others take 23
        z->cyc += 20;
    }
    else
    {
        wb(z, addr, result);
        z->cyc += 23;
    }

    return z->cyc - cyc_before;
}

// executes a ED opcode
int exec_opcode_ed(z80_emulator_t *const z, uint8_t opcode)
{
    uint64_t cyc_before = z->cyc;
    z->cyc += cyc_ed[opcode];
    inc_r(z);
    switch (opcode)
    {
    case 0x47:
        z->regs.i = z->regs.a;
        break; // ld i,a
    case 0x4F:
        z->regs.r = z->regs.a;
        break; // ld r,a

    case 0x57:
        z->regs.a = z->regs.i;
        z->regs.sf = z->regs.a >> 7;
        z->regs.zf = z->regs.a == 0;
        z->regs.hf = 0;
        z->regs.nf = 0;
        z->regs.pf = z->regs.iff2;
        break; // ld a,i

    case 0x5F:
        z->regs.a = z->regs.r;
        z->regs.sf = z->regs.a >> 7;
        z->regs.zf = z->regs.a == 0;
        z->regs.hf = 0;
        z->regs.nf = 0;
        z->regs.pf = z->regs.iff2;
        break; // ld a,r

    case 0x45:
    case 0x55:
    case 0x5D:
    case 0x65:
    case 0x6D:
    case 0x75:
    case 0x7D:
        z->regs.iff1 = z->regs.iff2;
        ret(z);
        break; // retn
    case 0x4D:
        ret(z);
        break; // reti

    case 0xA0:
        ldi(z);
        break; // ldi
    case 0xB0:
    {
        ldi(z);

        if (get_bc(z) != 0)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
            z->regs.mem_ptr = z->regs.pc + 1;
        }
    }
    break; // ldir

    case 0xA8:
        ldd(z);
        break; // ldd
    case 0xB8:
    {
        ldd(z);

        if (get_bc(z) != 0)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
            z->regs.mem_ptr = z->regs.pc + 1;
        }
    }
    break; // lddr

    case 0xA1:
        cpi(z);
        break; // cpi
    case 0xA9:
        cpd(z);
        break; // cpd
    case 0xB1:
    {
        cpi(z);
        if (get_bc(z) != 0 && !z->regs.zf)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
            z->regs.mem_ptr = z->regs.pc + 1;
        }
        else
        {
            z->regs.mem_ptr += 1;
        }
    }
    break; // cpir
    case 0xB9:
    {
        cpd(z);
        if (get_bc(z) != 0 && !z->regs.zf)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
        }
        else
        {
            z->regs.mem_ptr += 1;
        }
    }
    break; // cpdr

    case 0x40:
        in_r_c(z, &z->regs.b);
        break; // in b, (c)
    case 0x48:
        in_r_c(z, &z->regs.c);
        break; // in c, (c)
    case 0x50:
        in_r_c(z, &z->regs.d);
        break; // in d, (c)
    case 0x58:
        in_r_c(z, &z->regs.e);
        break; // in e, (c)
    case 0x60:
        in_r_c(z, &z->regs.h);
        break; // in h, (c)
    case 0x68:
        in_r_c(z, &z->regs.l);
        break; // in l, (c)
    case 0x70:
    {
        uint8_t val;
        in_r_c(z, &val);
    }
    break; // in (c)
    case 0x78:
        in_r_c(z, &z->regs.a);
        z->regs.mem_ptr = get_bc(z) + 1;
        break; // in a, (c)

    case 0xA2:
        ini(z);
        break; // ini
    case 0xB2:
        ini(z);
        if (z->regs.b > 0)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
        }
        break; // inir
    case 0xAA:
        ind(z);
        break; // ind
    case 0xBA:
        ind(z);
        if (z->regs.b > 0)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
        }
        break; // indr

    case 0x41:
        z80_write_io_internal(z, z->regs.c, z->regs.b);
        break; // out (c), b
    case 0x49:
        z80_write_io_internal(z, z->regs.c, z->regs.c);
        break; // out (c), c
    case 0x51:
        z80_write_io_internal(z, z->regs.c, z->regs.d);
        break; // out (c), d
    case 0x59:
        z80_write_io_internal(z, z->regs.c, z->regs.e);
        break; // out (c), e
    case 0x61:
        z80_write_io_internal(z, z->regs.c, z->regs.h);
        break; // out (c), h
    case 0x69:
        z80_write_io_internal(z, z->regs.c, z->regs.l);
        break; // out (c), l
    case 0x71:
        z80_write_io_internal(z, z->regs.c, 0);
        break; // out (c), 0
    case 0x79:
        z80_write_io_internal(z, z->regs.c, z->regs.a);
        z->regs.mem_ptr = get_bc(z) + 1;
        break; // out (c), a

    case 0xA3:
        outi(z);
        break; // outi
    case 0xB3:
    {
        outi(z);
        if (z->regs.b > 0)
        {
            z->regs.pc -= 2;
            z->cyc += 5;
        }
    }
    break; // otir
    case 0xAB:
        outd(z);
        break; // outd
    case 0xBB:
    {
        outd(z);
        if (z->regs.b > 0)
        {
            z->regs.pc -= 2;
        }
    }
    break; // otdr

    case 0x42:
        sbchl(z, get_bc(z));
        break; // sbc hl,bc
    case 0x52:
        sbchl(z, get_de(z));
        break; // sbc hl,de
    case 0x62:
        sbchl(z, get_hl(z));
        break; // sbc hl,hl
    case 0x72:
        sbchl(z, z->regs.sp);
        break; // sbc hl,sp

    case 0x4A:
        adchl(z, get_bc(z));
        break; // adc hl,bc
    case 0x5A:
        adchl(z, get_de(z));
        break; // adc hl,de
    case 0x6A:
        adchl(z, get_hl(z));
        break; // adc hl,hl
    case 0x7A:
        adchl(z, z->regs.sp);
        break; // adc hl,sp

    case 0x43:
    {
        const uint16_t addr = nextw(z);
        ww(z, addr, get_bc(z));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld (**), bc

    case 0x53:
    {
        const uint16_t addr = nextw(z);
        ww(z, addr, get_de(z));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld (**), de

    case 0x63:
    {
        const uint16_t addr = nextw(z);
        ww(z, addr, get_hl(z));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld (**), hl

    case 0x73:
    {
        const uint16_t addr = nextw(z);
        ww(z, addr, z->regs.sp);
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld (**),sp

    case 0x4B:
    {
        const uint16_t addr = nextw(z);
        set_bc(z, rw(z, addr));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld bc, (**)

    case 0x5B:
    {
        const uint16_t addr = nextw(z);
        set_de(z, rw(z, addr));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld de, (**)

    case 0x6B:
    {
        const uint16_t addr = nextw(z);
        set_hl(z, rw(z, addr));
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld hl, (**)

    case 0x7B:
    {
        const uint16_t addr = nextw(z);
        z->regs.sp = rw(z, addr);
        z->regs.mem_ptr = addr + 1;
    }
    break; // ld sp,(**)

    case 0x44:
    case 0x54:
    case 0x64:
    case 0x74:
    case 0x4C:
    case 0x5C:
    case 0x6C:
    case 0x7C:
        z->regs.a = subb(z, 0, z->regs.a, 0);
        break; // neg

    case 0x46:
    case 0x66:
        z->regs.im = 0;
        break; // im 0
    case 0x56:
    case 0x76:
        z->regs.im = 1;
        break; // im 1
    case 0x5E:
    case 0x7E:
        z->regs.im = 2;
        break; // im 2
    case 0x67:
    {
        uint8_t a = z->regs.a;
        uint8_t val = rb(z, get_hl(z));
        z->regs.a = (a & 0xF0) | (val & 0xF);
        wb(z, get_hl(z), (val >> 4) | (a << 4));

        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
        z->regs.zf = z->regs.a == 0;
        z->regs.sf = z->regs.a >> 7;
        z->regs.pf = parity(z->regs.a);
        z->regs.mem_ptr = get_hl(z) + 1;
    }
    break; // rrd

    case 0x6F:
    {
        uint8_t a = z->regs.a;
        uint8_t val = rb(z, get_hl(z));
        z->regs.a = (a & 0xF0) | (val >> 4);
        wb(z, get_hl(z), (val << 4) | (a & 0xF));

        z->regs.nf = 0;
        z->regs.hf = 0;
        z->regs.xf = GET_BIT(3, z->regs.a);
        z->regs.yf = GET_BIT(5, z->regs.a);
        z->regs.zf = z->regs.a == 0;
        z->regs.sf = z->regs.a >> 7;
        z->regs.pf = parity(z->regs.a);
        z->regs.mem_ptr = get_hl(z) + 1;
    }
    break; // rld

    default:
        fprintf(stderr, "unknown ED opcode: %02X\n", opcode);
        break;
    }

    return z->cyc - cyc_before;
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
        int instruction_cycles = z80_step(z80);
        long total_ns = instruction_cycles * NS_PER_CYCLE;

        pthread_mutex_lock(&z80->state_lock);
        z80->cyc += instruction_cycles;
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
    memcpy(buffer + offset, &z80->cyc, sizeof(uint64_t));
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
    memcpy(&z80->cyc, buffer + offset, sizeof(uint64_t));
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
        value = get_f(z80);
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
        set_f(z80, value & 0xFF);
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

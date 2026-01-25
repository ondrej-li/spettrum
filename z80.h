/**
 * Z80 CPU Emulator Header
 *
 * Provides a portable Z80 emulator implementation supporting the full Z80 instruction set,
 * interrupts, and memory/I/O callbacks for integration with external systems.
 */

#ifndef Z80_H
#define Z80_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>

// Z80 Configuration
#define Z80_CLOCK_FREQ 3500000 // 3.5 MHz
#define Z80_MAX_MEMORY 65536   // 64KB address space
#define Z80_IO_PORTS 256       // Number of I/O ports

// Z80 Register file
typedef struct
{
    uint16_t pc, sp, ix, iy;                // special purpose registers
    uint16_t mem_ptr;                       // "wz" register
    uint8_t a, b, c, d, e, h, l;            // main registers
    uint8_t a_, b_, c_, d_, e_, h_, l_, f_; // alternate registers
    uint8_t i, r;                           // interrupt vector, memory refresh

    // flags: sign, zero, yf, half-carry, xf, parity/overflow, negative, carry
    bool sf : 1, zf : 1, yf : 1, hf : 1, xf : 1, pf : 1, nf : 1, cf : 1;

    // Interrupt/control
    uint8_t im;         // Interrupt mode (0, 1, or 2)
    uint8_t iff1, iff2; // Interrupt flip-flops

    uint8_t iff_delay;
} z80_registers_t;

// Function pointer types for I/O callbacks
typedef uint8_t (*z80_read_io_t)(void *user_data, uint8_t port);
typedef void (*z80_write_io_t)(void *user_data, uint8_t port, uint8_t value);

// Function pointer types for memory callbacks
typedef uint8_t (*z80_read_memory_t)(void *user_data, uint16_t addr);
typedef void (*z80_write_memory_t)(void *user_data, uint16_t addr, uint8_t value);

// Context holder for both memory and I/O callbacks
typedef struct
{
    void *memory_data;
    void *io_data;
} z80_callback_context_t;

// Port-specific I/O callback structure
typedef struct
{
    z80_read_io_t read_fn;   // Callback for IN instruction
    z80_write_io_t write_fn; // Callback for OUT instruction
} z80_port_callback_t;

// Z80 Emulator state
typedef struct
{
    z80_registers_t regs;

    // Thread state
    pthread_t thread;
    volatile bool running;
    volatile bool paused;
    volatile bool halted;
    volatile bool int_pending : 1, nmi_pending : 1;
    uint8_t int_data;
    pthread_mutex_t state_lock;
    pthread_cond_t state_cond;

    // Timing
    struct timespec last_cycle_time;
    uint64_t cyc;

    // I/O and memory callbacks (pluggable)
    z80_read_io_t read_io;
    z80_write_io_t write_io;
    z80_read_memory_t read_memory;
    z80_write_memory_t write_memory;
    void *user_data;

    // Port-specific I/O callbacks
    z80_port_callback_t port_callbacks[Z80_IO_PORTS];
} z80_emulator_t;

// Z80 Flags (F register bits)
#define Z80_FLAG_C 0x01  // Carry
#define Z80_FLAG_N 0x02  // Subtract
#define Z80_FLAG_PV 0x04 // Parity/Overflow
#define Z80_FLAG_H 0x10  // Half-carry
#define Z80_FLAG_Z 0x40  // Zero
#define Z80_FLAG_S 0x80  // Sign

/**
 * Initialize Z80 emulator
 * @return Pointer to new Z80 emulator instance, or NULL on error
 */
z80_emulator_t *z80_init(void);

/**
 * Clean up Z80 emulator
 * @param z80 Emulator instance to clean up
 */
void z80_cleanup(z80_emulator_t *z80);

/**
 * Set memory access callbacks
 * @param z80 Emulator instance
 * @param read_fn Function pointer for memory reads
 * @param write_fn Function pointer for memory writes
 * @param user_data User data passed to callbacks
 */
void z80_set_memory_callbacks(z80_emulator_t *z80,
                              z80_read_memory_t read_fn,
                              z80_write_memory_t write_fn,
                              void *user_data);

/**
 * Register port-specific IN callback
 * Called when CPU executes IN instruction for a specific port
 * @param z80 Emulator instance
 * @param port Port number (0-255)
 * @param read_fn Callback function that returns the port value
 */
void z80_register_port_in(z80_emulator_t *z80,
                          uint8_t port,
                          z80_read_io_t read_fn);

/**
 * Register port-specific OUT callback
 * Called when CPU executes OUT instruction for a specific port
 * @param z80 Emulator instance
 * @param port Port number (0-255)
 * @param write_fn Callback function that handles the written value
 */
void z80_register_port_out(z80_emulator_t *z80,
                           uint8_t port,
                           z80_write_io_t write_fn);

/**
 * Set I/O callback context data
 * @param z80 Emulator instance
 * @param io_data User data to pass to I/O callbacks
 */
void z80_set_io_callbacks(z80_emulator_t *z80, void *io_data);

/**
 * Execute a single Z80 instruction
 * @param z80 Emulator instance
 * @return Number of clock cycles consumed by the instruction
 */
int z80_step(z80_emulator_t *const z);

/**
 * Get register value by name
 * @param z80 Emulator instance
 * @param reg_name Register name (e.g., "A", "BC", "PC", "SP")
 * @return Register value
 */
uint16_t z80_get_register(z80_emulator_t *z80, const char *reg_name);

/**
 * Set register value by name
 * @param z80 Emulator instance
 * @param reg_name Register name
 * @param value Value to set
 */
void z80_set_register(z80_emulator_t *z80, const char *reg_name, uint16_t value);

/**
 * Get program counter
 * @param z80 Emulator instance
 * @return Current PC value
 */
uint16_t z80_get_pc(z80_emulator_t *z80);

/**
 * Set program counter
 * @param z80 Emulator instance
 * @param pc New PC value
 */
void z80_set_pc(z80_emulator_t *z80, uint16_t pc);

/**
 * Start asynchronous emulation
 * @param z80 Emulator instance
 * @return 0 on success
 */
int z80_start(z80_emulator_t *z80);

/**
 * Stop emulation
 * @param z80 Emulator instance
 */
void z80_stop(z80_emulator_t *z80);

/**
 * Pause emulation
 * @param z80 Emulator instance
 */
void z80_pause(z80_emulator_t *z80);

/**
 * Resume emulation
 * @param z80 Emulator instance
 */
void z80_resume(z80_emulator_t *z80);

/**
 * Save emulator state
 * @param z80 Emulator instance
 * @param buffer Pointer to output buffer
 * @param buffer_size Size of output buffer
 * @return Bytes written, or negative on error
 */
size_t z80_save_state(z80_emulator_t *z80, uint8_t *buffer, size_t buffer_size);

/**
 * Load emulator state
 * @param z80 Emulator instance
 * @param buffer Pointer to input buffer
 * @param buffer_size Size of input buffer
 * @return 0 on success, negative on error
 */
int z80_load_state(z80_emulator_t *z80, const uint8_t *buffer, size_t buffer_size);

/**
 * Generate an interrupt signal
 * @param z80 Emulator instance
 * @param data Data byte for interrupt (used in IM0 and IM2 modes)
 */
void z80_gen_int(z80_emulator_t *const z, uint8_t data);

void set_f(z80_emulator_t *const z, uint8_t val);
uint8_t get_f(z80_emulator_t *const z);
#endif // Z80_H

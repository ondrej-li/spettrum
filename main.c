/**
 * Spettrum - Z80 Emulator Main Entry Point
 *
 * Initializes emulator components (Z80 CPU, ULA graphics), processes command-line
 * arguments, and starts the main emulation loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>

#include "z80.h"
#include "ula.h"
#include "main.h"
#include "disasm.h"
#include "keyboard.h"
#include "z80snapshot.h"
#include "tap.h"

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
        convert_vram_to_matrix(&emulator->memory[SPETTRUM_VRAM_START], emulator->display->render_mode);

        // Render matrix to terminal
        ula_render_to_terminal();
    }

    return NULL;
}

/**
 * Append a message to the warning buffer (thread-safe)
 */
static void append_warning_buffer(spettrum_emulator_t *emulator, const char *format, ...)
{
    if (!emulator || !emulator->warning_buffer)
        return;

    va_list args;
    va_start(args, format);

    // Calculate space needed
    int needed = vsnprintf(NULL, 0, format, args);
    va_start(args, format); // Reset va_list

    // Expand buffer if needed (allocate in 4KB chunks)
    if (emulator->warning_buffer_pos + needed + 1 > emulator->warning_buffer_size)
    {
        size_t new_size = ((emulator->warning_buffer_pos + needed + 1 + 4095) / 4096) * 4096;
        char *new_buffer = realloc(emulator->warning_buffer, new_size);
        if (new_buffer)
        {
            emulator->warning_buffer = new_buffer;
            emulator->warning_buffer_size = new_size;
        }
        else
        {
            va_end(args);
            return; // Allocation failed, skip this warning
        }
    }

    // Append to buffer
    emulator->warning_buffer_pos += vsnprintf(
        emulator->warning_buffer + emulator->warning_buffer_pos,
        emulator->warning_buffer_size - emulator->warning_buffer_pos,
        format, args);

    va_end(args);
}

/**
 * Check for keyboard input (non-blocking)
 * Returns: character pressed, or -1 if none
 */
/**
 * Display CPU state and debug info when paused
 */
static void display_debug_info(spettrum_emulator_t *emulator)
{
    z80_registers_t *regs = &emulator->cpu->regs;
    z80_emulator_t *z80 = emulator->cpu;

    // Move to line 49 (bottom area)
    printf("\033[49;1H\033[K");
    printf("PC:%04X SP:%04X AF:%04X BC:%04X DE:%04X HL:%04X IX:%04X IY:%04X\n",
           regs->pc, regs->sp, (regs->a << 8) | get_f(z80),
           (regs->b << 8) | regs->c, (regs->d << 8) | regs->e,
           (regs->h << 8) | regs->l, regs->ix, regs->iy);

    printf("\033[50;1H\033[K");
    printf("Flags: S=%d Z=%d H=%d P=%d N=%d C=%d | Inst:%llu\n",
           regs->sf, regs->zf, regs->hf, regs->pf, regs->nf, regs->cf,
           emulator->total_instructions);

    // Show last few instructions
    printf("\033[51;1H\033[K");
    printf("Last instructions: ");
    for (int i = 0; i < 5; i++)
    {
        int idx = (emulator->history_index - 5 + i + 10) % 10;
        if (emulator->last_pc[idx] != 0 || i == 4)
            printf("%04X:%02X ", emulator->last_pc[idx], emulator->last_opcode[idx]);
    }

    printf("\033[52;1H\033[K[PAUSED - Ctrl-P:resume | [:slower | ]:faster | Ctrl-D:dump]\033[52;1H");
    fflush(stdout);
}

/**
 * Detect CPU anomalies (warnings collected, displayed after emulation)
 */
static void check_cpu_anomalies(spettrum_emulator_t *emulator)
{
    uint16_t pc = emulator->cpu->regs.pc;
    uint16_t sp = emulator->cpu->regs.sp;

    // PC executing in VRAM (bitmap or attributes - both wrong)
    if (pc >= SPETTRUM_VRAM_START && pc < SPETTRUM_VRAM_START + SPETTRUM_VRAM_SIZE)
    {
        emulator->warnings_pc_in_vram++;
        emulator->last_warn_pc = pc;
        emulator->warn_sp_at_fault = sp;

        // Save last 5 PC values from history
        for (int i = 0; i < 5; i++)
        {
            int idx = (emulator->history_index - 5 + i + 10) % 10;
            emulator->warn_pc_history[i] = emulator->last_pc[idx];
        }

        const char *area = (pc >= 0x5800) ? "attributes" : "bitmap";
        // Append to warning buffer instead of printing to screen
        append_warning_buffer(emulator, "  ⚠️  PC in VRAM %s (PC=0x%04X SP=0x%04X) [%llu times]\n",
                              area, pc, sp, emulator->warnings_pc_in_vram);
    }

    // Stack collision with screen memory
    if (sp >= SPETTRUM_VRAM_START && sp < SPETTRUM_VRAM_START + SPETTRUM_VRAM_SIZE)
    {
        emulator->warnings_sp_in_vram++;
        emulator->last_warn_sp = sp;
        emulator->warn_pc_at_sp_fault = pc;
        // Append to warning buffer instead of printing to screen
        append_warning_buffer(emulator, "  ⚠️  SP in VRAM (SP=0x%04X PC=0x%04X) [%llu times]\n",
                              sp, pc, emulator->warnings_sp_in_vram);
    }
}

/**
 * Display CPU anomaly summary
 */
static void display_anomaly_summary(spettrum_emulator_t *emulator)
{
    if (!emulator)
        return;

    printf("\n\n=== CPU Anomaly Summary ===\n");
    if (emulator->warnings_pc_in_vram > 0)
    {
        printf("⚠️  PC in VRAM: %llu occurrences\n", emulator->warnings_pc_in_vram);
        printf("   Last fault: PC=0x%04X, SP=0x%04X\n",
               emulator->last_warn_pc, emulator->warn_sp_at_fault);
        printf("   PC history before fault: ");
        for (int i = 0; i < 5; i++)
        {
            if (emulator->warn_pc_history[i] != 0 || i == 4)
                printf("0x%04X ", emulator->warn_pc_history[i]);
        }
        printf("-> 0x%04X\n", emulator->last_warn_pc);
    }
    if (emulator->warnings_sp_in_vram > 0)
    {
        printf("⚠️  SP in VRAM: %llu occurrences\n", emulator->warnings_sp_in_vram);
        printf("   Last fault: SP=0x%04X, PC=0x%04X\n",
               emulator->last_warn_sp, emulator->warn_pc_at_sp_fault);
    }
    if (emulator->warnings_pc_in_vram == 0 && emulator->warnings_sp_in_vram == 0)
    {
        printf("✓ No CPU anomalies detected\n");
    }

    // Display buffered warnings collected during emulation
    if (emulator->warning_buffer && emulator->warning_buffer_pos > 0)
    {
        printf("\nWarnings collected during emulation:\n");
        printf("%s", emulator->warning_buffer);
    }

    printf("Total instructions executed: %llu\n", emulator->total_instructions);
    fflush(stdout);
}

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig)
{
    (void)sig; // Unused
    if (g_emulator)
    {
        g_emulator->running = 0;
        display_anomaly_summary(g_emulator);
    }
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
    printf("  -h, --help                Show this help message\n");
    printf("  -v, --version             Show version information\n");
    printf("  -r, --rom FILE            Load ROM from file\n");
    printf("  -s, --snapshot FILE       Load Z80 snapshot file (restores CPU and memory state)\n");
    printf("  -t, --tap FILE            Load TAP tape image file (uses ROM loader by default)\n");
    printf("  -q, --quick-load          Quick-load TAP directly to memory (bypass ROM loader)\n");
    printf("  -d, --disk FILE           Load disk image from file\n");
    printf("  -i, --instructions NUM    Number of instructions to execute (0=unlimited, default=0)\n");
    printf("  -D, --disassemble FILE    Write disassembly to FILE\n");
    printf("  -m, --render-mode MODE    Rendering mode: block (2x2), braille (2x4, default), or ocr (32x24)\n");
    printf("  -k, --simulate-key STRING Simulate key presses (auto-replay starting at 3s, spaced 500ms)\n");
    printf("\n");
}

/**
 * Memory read callback for Z80 CPU
 */
static uint8_t emulator_read_memory(void *user_data, uint16_t addr)
{
    z80_callback_context_t *ctx = (z80_callback_context_t *)user_data;
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)ctx->memory_data;
    return emulator->memory[addr];
}

/**
 * Memory write callback for Z80 CPU
 */
static void emulator_write_memory(void *user_data, uint16_t addr, uint8_t value)
{
    z80_callback_context_t *ctx = (z80_callback_context_t *)user_data;
    spettrum_emulator_t *emulator = (spettrum_emulator_t *)ctx->memory_data;

    // First 16KB (0x0000-0x3FFF) is ROM - ignore writes
    if (addr < SPETTRUM_ROM_SIZE)
        return;

    emulator->memory[addr] = value;
}

/**
 * Keyboard port IN handler (port 0xFE and variants)
 *
 * The Spectrum reads keyboard state via port 0xFE.
 * This handler queries the current host keyboard state and returns it
 * in Spectrum keyboard matrix format.
 *
 * If a tape is being played, bit 6 (EAR) is overridden with tape data.
 */
static uint8_t keyboard_read_handler(void *user_data, uint16_t port)
{
    static uint64_t total_calls = 0;
    static uint64_t port_0xfe_calls = 0;
    static uint64_t tape_player_calls = 0;
    static FILE *port_log = NULL;

    total_calls++;

    // Open debug log on first call
    if (!port_log)
    {
        port_log = fopen("tap_port.log", "w");
        if (port_log)
        {
            fprintf(port_log, "=== Port Read Handler Debug ===\n\n");
            fflush(port_log);
        }
    }

    z80_callback_context_t *ctx = (z80_callback_context_t *)user_data;
    uint8_t result = keyboard_read_port(port);

    // Log first 50 calls regardless of port
    if (port_log && total_calls <= 50)
    {
        fprintf(port_log, "Call #%llu: port=0x%04X, ctx=%p", total_calls, port, (void *)ctx);
        if (ctx)
        {
            fprintf(port_log, ", io_data=%p", ctx->io_data);
            if (ctx->io_data)
            {
                spettrum_emulator_t *emulator = (spettrum_emulator_t *)ctx->io_data;
                fprintf(port_log, ", tape_player=%p", (void *)emulator->tape_player);
            }
        }
        fprintf(port_log, "\n");
        fflush(port_log);
    }

    // Check if this is a port 0xFE read
    if ((port & 0xFF) == 0xFE)
    {
        port_0xfe_calls++;
        if (port_log && port_0xfe_calls <= 20)
        {
            fprintf(port_log, "  Port 0xFE read #%llu (total call #%llu)\n", port_0xfe_calls, total_calls);
            fflush(port_log);
        }
    }

    // If tape player is active and reading from port 0xFE, inject EAR bit
    if (ctx && ctx->io_data)
    {
        spettrum_emulator_t *emulator = (spettrum_emulator_t *)ctx->io_data;
        if (emulator && emulator->tape_player && (port & 0xFF) == 0xFE)
        {
            tape_player_calls++;

            if (port_log && tape_player_calls <= 20)
            {
                fprintf(port_log, "  TAPE: Reading tape at cycle %llu\n", emulator->cpu->cyc);
                fflush(port_log);
            }

            // Bit 6 is EAR input - overwrite with tape data
            uint8_t ear_bit = tape_player_read_ear(emulator->tape_player, emulator->cpu->cyc);
            if (ear_bit)
                result |= 0x40; // Set bit 6
            else
                result &= ~0x40; // Clear bit 6

            if (port_log && tape_player_calls <= 20)
            {
                fprintf(port_log, "  TAPE: ear_bit=%u, result=0x%02X\n", ear_bit, result);
                fflush(port_log);
            }
        }
    }

    return result;
}

/**
 * Generic I/O read callback - fallback for unregistered ports
 * Returns 0xFF (all bits set) for unimplemented ports
 */
static uint8_t generic_io_read(void *user_data, uint16_t port)
{
    (void)user_data; // Not needed
    (void)port;      // Not needed
    return 0xFF;     // Default: all bits set (floating bus)
}

/**
 * Generic I/O write callback - fallback for unregistered ports
 * Handles border color (port 0xFE bits 0-2), keyboard row selection, and other ports
 */
static void generic_io_write(void *user_data, uint16_t port, uint8_t value)
{
    z80_callback_context_t *ctx = (z80_callback_context_t *)user_data;
    if (!ctx || !ctx->io_data)
        return;

    spettrum_emulator_t *emulator = (spettrum_emulator_t *)ctx->io_data;
    if (!emulator)
        return;

    // Port 0xFE - ULA control and keyboard row selection
    if ((port & 0xFF) == 0xFE)
    {
        // Bits 0-2: border color
        uint8_t border_color = value & 0x07;
        ula_set_border_color(emulator->display, border_color);

        // Bits 3-4: tape control (not implemented)
        // Bits 5-7: keyboard row selector
        // The ROM code uses OUT (C), B to set the row selector
        // which tells us which row of the keyboard matrix to scan
        keyboard_set_row_selector(value);

        // Bit 6-7: unused
    }
    // Other ports: silently ignore (not implemented)
}

/**
 * Initialize emulator components
 */
static spettrum_emulator_t *emulator_init(ula_render_mode_t render_mode)
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

    // Set I/O callbacks context data
    z80_set_io_callbacks(emulator->cpu, emulator);

    // Initialize ULA display
    emulator->display = ula_init(SPECTRUM_WIDTH, SPECTRUM_HEIGHT, &emulator->memory[SPETTRUM_VRAM_START], render_mode);
    if (!emulator->display)
    {
        fprintf(stderr, "Error: Failed to initialize ULA display\n");
        z80_cleanup(emulator->cpu);
        free(emulator);
        return NULL;
    }

    // Initialize keyboard handler (sets terminal to raw mode)
    if (keyboard_init() != 0)
    {
        fprintf(stderr, "Error: Failed to initialize keyboard\n");
        ula_cleanup(emulator->display);
        z80_cleanup(emulator->cpu);
        free(emulator);
        return NULL;
    }

    // Set generic I/O callbacks (fallback for all ports)
    emulator->cpu->read_io = generic_io_read;   // Default read handler for all ports
    emulator->cpu->write_io = generic_io_write; // Default write handler for all ports

    // Register keyboard port handlers (override the generic handler for these ports)
    // Each port reads a different row of the keyboard matrix
    z80_register_port_in(emulator->cpu, 0xFE, keyboard_read_handler); // CAPS SHIFT, Z, X, C, V
    z80_register_port_in(emulator->cpu, 0xFD, keyboard_read_handler); // A, S, D, F, G
    z80_register_port_in(emulator->cpu, 0xFB, keyboard_read_handler); // Q, W, E, R, T
    z80_register_port_in(emulator->cpu, 0xF7, keyboard_read_handler); // 1, 2, 3, 4, 5
    z80_register_port_in(emulator->cpu, 0xEF, keyboard_read_handler); // 6, 7, 8, 9, 0
    z80_register_port_in(emulator->cpu, 0xDF, keyboard_read_handler); // SPACE, SHIFT, ENTER, SYMBOL SHIFT
    z80_register_port_in(emulator->cpu, 0xBF, keyboard_read_handler); // P, O, I, U, Y
    z80_register_port_in(emulator->cpu, 0x7F, keyboard_read_handler); // M, N, B, T, G

    // Initialize tape player (will be set later if TAP file specified)
    emulator->tape_player = NULL;
    emulator->use_authentic_loading = 0;

    // Initialize warning buffer (initial 4KB)
    emulator->warning_buffer_size = 4096;
    emulator->warning_buffer = malloc(emulator->warning_buffer_size);
    emulator->warning_buffer_pos = 0;
    if (emulator->warning_buffer)
    {
        emulator->warning_buffer[0] = '\0';
    }

    // Initialize simulated keys (will be set later if -k option is used)
    emulator->simulated_keys = NULL;

    // Initialize ULA interrupt timing fields
    emulator->frame_cycle_count = 0;
    emulator->int_asserted = 0;
    emulator->int_asserted_time = 0;

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

    // Cleanup keyboard (restores terminal to normal mode)
    keyboard_cleanup();

    // Close tape player if active
    if (emulator->tape_player)
        tape_player_close(emulator->tape_player);

    if (emulator->cpu)
        z80_cleanup(emulator->cpu);

    if (emulator->display)
        ula_cleanup(emulator->display);

    // Free warning buffer
    if (emulator->warning_buffer)
        free(emulator->warning_buffer);

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
    printf("\nControls: Ctrl+P=pause | [/]=speed | Ctrl+S=step | Ctrl+D=debug | Ctrl+C=stop\n\n");
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

    // For simulated key timing
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    int simulated_key_index = 0;

    // Run Z80 CPU in main thread
    while (emulator->running && (instructions_to_run == 0 || instructions_executed < instructions_to_run))
    {
        // Check for control keys (keyboard input removed)
        int key = -1;

        if (key == 16) // Ctrl-P (ASCII 16)
        {
            if (emulator->step_mode)
            {
                // Exit step mode
                emulator->step_mode = 0;
                emulator->paused = 0;
                printf("\033[49;1H\033[K\033[50;1H\033[K\033[51;1H\033[K\033[52;1H\033[K\033[53;1H\033[K");
                printf("\033[48;1H\033[K[Running]\033[48;1H");
                fflush(stdout);
            }
            else
            {
                emulator->paused = !emulator->paused;
                if (emulator->paused)
                {
                    // Print pause message and debug info
                    display_debug_info(emulator);
                }
                else
                {
                    printf("\033[49;1H\033[K\033[50;1H\033[K\033[51;1H\033[K\033[52;1H\033[K\033[53;1H\033[K");
                    printf("\033[48;1H\033[K[Running]\033[48;1H");
                    fflush(stdout);
                }
            }
        }
        else if (key == 4) // Ctrl-D (ASCII 4)
        {
            // Dump registers (works anytime, auto-pauses if not paused)
            if (!emulator->paused)
                emulator->paused = 1;
            display_debug_info(emulator);
        }
        else if (key == 19) // Ctrl-S (ASCII 19)
        {
            // Toggle step mode or execute one step
            if (!emulator->step_mode)
            {
                // Enter step mode
                emulator->step_mode = 1;
                emulator->paused = 0; // Will pause after next instruction
                printf("\033[48;1H\033[K[STEP MODE - Ctrl-S:step | Ctrl-P:exit step mode]\033[48;1H");
                fflush(stdout);
            }
            else
            {
                // Execute one instruction in step mode
                emulator->paused = 0;
            }
        }
        else if (key == '[')
        {
            emulator->speed_delay += 100;
            if (emulator->speed_delay > 10000)
                emulator->speed_delay = 10000;
            printf("\033[48;1H\033[K[Speed delay: %d us]\033[48;1H", emulator->speed_delay);
            fflush(stdout);
        }
        else if (key == ']')
        {
            emulator->speed_delay -= 100;
            if (emulator->speed_delay < 0)
                emulator->speed_delay = 0;
            printf("\033[48;1H\033[K[Speed delay: %d us]\033[48;1H", emulator->speed_delay);
            fflush(stdout);
        }

        // Skip execution if paused
        if (emulator->paused)
        {
            usleep(10000); // Sleep 10ms while paused
            continue;
        }

        // Handle simulated key injection (auto-replay starting at 3 seconds, spaced 500ms apart)
        if (emulator->simulated_keys && emulator->simulated_keys[0] != '\0')
        {
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);

            // Calculate elapsed time in milliseconds
            long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                              (current_time.tv_nsec - start_time.tv_nsec) / 1000000;

            // First key starts at 3000ms (3 seconds)
            // Each subsequent key is 500ms after the previous one
            long key_delay_ms = 3000 + (simulated_key_index * 500);

            if (elapsed_ms >= key_delay_ms && simulated_key_index < (int)strlen(emulator->simulated_keys))
            {
                // Time to inject the next key
                char key_char = emulator->simulated_keys[simulated_key_index];
                keyboard_set_simulated_key(key_char);
                printf("[Key injected: %c at %ldms]\n", key_char, elapsed_ms);
                fflush(stdout);
                simulated_key_index++;
            }
        }

        // Check if memory dump was requested
        if (emulator->dump_memory)
        {
            emulator->dump_memory = 0;
            dump_memory_to_file(emulator);
        }

        // Get current PC and opcode for disassembly BEFORE z80_step increments PC
        uint16_t pc_before = emulator->cpu->regs.pc;

        // Execute a single instruction and accumulate cycles
        int instruction_cycles = z80_step(emulator->cpu);

        // Now capture the opcode that was actually executed (from the PC position before step)
        uint16_t pc = pc_before;
        uint8_t opcode = emulator->memory[pc];

        // Record in history
        emulator->last_pc[emulator->history_index] = pc;
        emulator->last_opcode[emulator->history_index] = opcode;
        emulator->history_index = (emulator->history_index + 1) % 10;
        emulator->cpu->cyc += instruction_cycles;

// ULA interrupt handling - trigger INT at ~50Hz (every ~70908 cycles at 3.5MHz)
// Spectrum: ~69888 T-states minimum from vertical sync
// Using 70908 for full frame with contention timing
#define SPECTRUM_FRAME_CYCLES 70908 // Cycles per 50Hz frame
#define INT_PULSE_CYCLES 32         // INT asserted for ~32 T-states

        emulator->frame_cycle_count += instruction_cycles;

        // Check if we've completed a frame
        if (emulator->frame_cycle_count >= SPECTRUM_FRAME_CYCLES)
        {
            // Reset frame counter
            emulator->frame_cycle_count -= SPECTRUM_FRAME_CYCLES;

            // Assert INT signal - generates interrupt if IM1 is enabled
            if (emulator->cpu->regs.iff1) // Check if interrupts are enabled
            {
                z80_gen_int(emulator->cpu, 0xFF); // Generate interrupt with data 0xFF for IM1
                emulator->int_asserted = 1;
                emulator->int_asserted_time = emulator->cpu->cyc;
            }
        }

        // Check for anomalies every 1000 instructions
        if (emulator->total_instructions % 1000 == 0)
        {
            check_cpu_anomalies(emulator);
        }

        // Log disassembly if enabled
        if (emulator->disasm_file)
        {
            log_instruction_disassembly(emulator, pc, opcode);
        }

        emulator->total_instructions++;
        instructions_executed++;

        // In step mode, pause after each instruction and show debug info
        if (emulator->step_mode)
        {
            emulator->paused = 1;
            display_debug_info(emulator);
        }

        // Apply speed delay if set
        if (emulator->speed_delay > 0)
        {
            usleep(emulator->speed_delay);
        }
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
    printf("Total cycles: %llu\n", emulator->cpu->cyc);
    printf("Final PC: 0x%04X\n", emulator->cpu->regs.pc);
    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char *argv[])
{
    const char *rom_file = NULL;
    const char *snapshot_file = NULL;
    const char *tap_file = NULL;
    const char *disk_file = NULL;
    const char *disasm_file = NULL;
    const char *simulated_keys = NULL;                     // Simulated key string for testing
    int use_authentic_tape_loading = 1;                    // Default: use ROM loader (authentic)
    uint64_t instructions_to_run = 0;                      // Default: unlimited
    ula_render_mode_t render_mode = ULA_RENDER_BRAILLE2X4; // Default: braille

    // Command-line options
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"rom", required_argument, 0, 'r'},
        {"snapshot", required_argument, 0, 's'},
        {"tap", required_argument, 0, 't'},
        {"quick-load", no_argument, 0, 'q'},
        {"disk", required_argument, 0, 'd'},
        {"instructions", required_argument, 0, 'i'},
        {"disassemble", required_argument, 0, 'D'},
        {"render-mode", required_argument, 0, 'm'},
        {"simulate-key", required_argument, 0, 'k'},
        {0, 0, 0, 0}};

    // Parse command-line arguments
    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "hvr:s:t:qd:i:D:m:k:", long_options, &option_index)) != -1)
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
        case 's':
            snapshot_file = optarg;
            break;
        case 't':
            tap_file = optarg;
            break;
        case 'q':
            use_authentic_tape_loading = 0; // Disable authentic loading (use quick-load)
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
        case 'm':
            if (strcmp(optarg, "block") == 0 || strcmp(optarg, "2x2") == 0)
            {
                render_mode = ULA_RENDER_BLOCK2X2;
            }
            else if (strcmp(optarg, "braille") == 0 || strcmp(optarg, "2x4") == 0)
            {
                render_mode = ULA_RENDER_BRAILLE2X4;
            }
            else if (strcmp(optarg, "ocr") == 0 || strcmp(optarg, "text") == 0)
            {
                render_mode = ULA_RENDER_OCR;
            }
            else
            {
                fprintf(stderr, "Error: Invalid render mode '%s'. Use 'block', 'braille', or 'ocr'\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'k':
            // Simulate keys for testing (string of characters)
            simulated_keys = optarg;
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
    spettrum_emulator_t *emulator = emulator_init(render_mode);
    if (!emulator)
    {
        fprintf(stderr, "Error: Failed to initialize emulator\n");
        return EXIT_FAILURE;
    }

    // Initialize dump counter
    emulator->dump_count = 0;

    // Initialize pause state and speed control
    emulator->paused = 0;
    emulator->speed_delay = 0;
    emulator->step_mode = 0;

    // Initialize debug tracking
    memset(emulator->last_pc, 0, sizeof(emulator->last_pc));
    memset(emulator->last_opcode, 0, sizeof(emulator->last_opcode));
    emulator->history_index = 0;
    emulator->total_instructions = 0;

    // Initialize anomaly tracking
    emulator->warnings_pc_in_vram = 0;
    emulator->warnings_sp_in_vram = 0;
    emulator->last_warn_pc = 0;
    emulator->last_warn_sp = 0;
    memset(emulator->warn_pc_history, 0, sizeof(emulator->warn_pc_history));
    emulator->warn_sp_at_fault = 0;
    emulator->warn_pc_at_sp_fault = 0;

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

    // Load Z80 snapshot if specified (restores CPU and memory state)
    if (snapshot_file)
    {
        if (z80_snapshot_load(snapshot_file, emulator->cpu, emulator->memory) != 0)
        {
            fprintf(stderr, "Error: Failed to load Z80 snapshot from '%s'\n", snapshot_file);
            emulator_cleanup(emulator);
            return EXIT_FAILURE;
        }
    }

    // Load TAP tape image if specified
    if (tap_file)
    {
        if (use_authentic_tape_loading)
        {
            // Authentic loading: ROM loader will read from port 0xFE (cassette EAR)
            emulator->tape_player = tape_player_init(tap_file);
            if (!emulator->tape_player)
            {
                fprintf(stderr, "Error: Failed to initialize tape player for '%s'\n", tap_file);
                emulator_cleanup(emulator);
                return EXIT_FAILURE;
            }
            emulator->use_authentic_loading = 1;
            printf("\n");
            printf("╔════════════════════════════════════════════════════════════════╗\n");
            printf("║  TAP TAPE LOADED - Authentic ROM Loading Mode                 ║\n");
            printf("╠════════════════════════════════════════════════════════════════╣\n");
            printf("║  The emulator will now boot the Spectrum ROM.                 ║\n");
            printf("║  Wait for the 'K' cursor to appear, then type:                ║\n");
            printf("║                                                                ║\n");
            printf("║      LOAD \"\"                                                   ║\n");
            printf("║                                                                ║\n");
            printf("║  and press ENTER to start loading from tape.                  ║\n");
            printf("║  Debug logs: tap.log and tap_port.log                         ║\n");
            printf("╚════════════════════════════════════════════════════════════════╝\n");
            printf("\n");
        }
        else
        {
            // Quick-load: directly load TAP data to memory (bypasses ROM loader)
            // Default load address: 0x5C00 (after color attributes)
            if (tap_load_to_memory(tap_file, emulator->memory, SPETTRUM_TOTAL_MEMORY, 0x5C00) != 0)
            {
                fprintf(stderr, "Error: Failed to load TAP file from '%s'\n", tap_file);
                emulator_cleanup(emulator);
                return EXIT_FAILURE;
            }
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

    // Set simulated keys if provided
    if (simulated_keys && simulated_keys[0] != '\0')
    {
        emulator->simulated_keys = simulated_keys;
        printf("Simulated keys to inject: '%s' (starting at 3s, spaced 500ms apart)\n", simulated_keys);
    }

    // Run emulation
    int result = emulator_run(emulator, instructions_to_run);

    // Display anomaly summary
    display_anomaly_summary(emulator);

    // Cleanup
    emulator_cleanup(emulator);

    return result;
}

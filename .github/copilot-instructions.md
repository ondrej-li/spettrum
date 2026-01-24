# Spettrum - Z80 Emulator

## Project Overview

**Spettrum** is a high-performance Z80 CPU emulator for the Sinclair ZX Spectrum computer with a terminal-based display renderer. The project implements:

- **Z80 CPU Emulation**: Full instruction set for the 8-bit Z80 processor (used in Sinclair Spectrum)
- **ULA (Uncommented Logic Array) Graphics**: Real-time video RAM to terminal rendering using Unicode block/braille characters
- **50Hz Frame Timing**: Accurate refresh rate matching original Spectrum hardware
- **ROM Loading**: Support for loading Spectrum ROM images
- **Debugging Tools**: CPU state inspection, disassembly logging, and anomaly detection

The emulator can run authentic Spectrum programs including system ROMs, with real-time display rendering in the terminal.

## Building

### Prerequisites
- **clang** compiler (or any C compiler)
- **POSIX-compatible system** (macOS, Linux)
- **pthread** library (for multi-threaded rendering)

### Build Commands

```bash
# Build the executable
make

# Build with debug symbols
make debug

# Run the emulator
make run
```

The executable is generated at `bin/spettrum`.

## Running the Emulator

### Basic Execution

```bash
# Run with default settings (using system ROMs if available)
./bin/spettrum

# View help and available options
./bin/spettrum --help
```

### Command-Line Options

```
  -h, --help                 Show help message
  -v, --version              Show version information
  -r, --rom FILE             Load ROM from file
  -d, --disk FILE            Load disk image from file
  -i, --instructions NUM     Number of instructions to execute (0=unlimited)
  -D, --disassemble FILE     Write disassembly output to FILE
  -m, --render-mode MODE     Rendering mode: 'block' (2x2) or 'braille' (2x4, default)
  -k, --simulate-key CHAR    Simulate a key press for testing
```

### ROM Files

The project includes two main ROM images:

1. **`rom/ZX_Spectrum_48k.rom`** - Standard Sinclair Spectrum 48K ROM
   - The official operating system and BASIC interpreter
   - Essential for normal Spectrum operation

2. **`rom/DiagROMv.173.rom`** - Spectrum Diagnostic ROM
   - Used for system diagnostics and testing
   - Useful for verifying emulator correctness

### Example Usage

```bash
# Run with standard 48K ROM
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom

# Run with diagnostic ROM and render in 2x2 block mode
./bin/spettrum --rom rom/DiagROMv.173.rom --render-mode block

# Execute with disassembly logging
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --disassemble disasm.log

# Run for a specific number of instructions
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --instructions 100000
```

## Debugging

### Disassembly Output

To capture detailed instruction-level debugging information:

```bash
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --disassemble disasm.log
```

This generates `disasm.log` containing:
- **Program Counter (PC)** - Instruction address
- **Opcodes** - Raw machine code bytes
- **Mnemonics** - Human-readable Z80 assembly instruction names
- **Operands** - Instruction arguments and addressing modes
- **CPU State** - Register and flag values at each instruction

The disassembly helps identify:
- Program flow and execution paths
- Anomalous execution (e.g., CPU jumping into video RAM)
- Stack pointer corruption
- Instruction sequences for debugging logic errors

### Debug Features

When the emulator runs, it tracks:
- **PC History**: Last 10 program counter values for context
- **Opcode History**: Last 10 executed opcodes
- **Anomalies**: Warnings when PC or SP enter video RAM (indicating bugs)
- **Instruction Count**: Total instructions executed
- **CPU State**: Registers, flags, and memory state

After emulation completes, any detected anomalies are displayed.

## Project Structure

```
spettrum/
├── main.c / main.h           Entry point, emulator state, command-line parsing
├── z80.c / z80.h             Z80 CPU emulation core (instruction execution)
├── ula.c / ula.h             Video RAM (VRAM) to terminal renderer
├── disasm.c / disasm.h       Disassembly and instruction formatting
├── keyboard.c / keyboard.h   Keyboard input handling
│
├── Makefile                  Build system
├── README.md                 Original project documentation
├── GITHUB_COPILOT_INSTRUCTIONS.md   (this file)
│
├── bin/                      Compiled executables
│   └── spettrum              Main emulator binary
│
├── obj/                      Compiled object files
│   ├── z80.o
│   ├── ula.o
│   ├── disasm.o
│   └── keyboard.o
│
├── rom/                      ROM images (Spectrum firmware)
│   ├── ZX_Spectrum_48k.rom   Standard Spectrum 48K ROM
│   ├── DiagROMv.173.rom      Diagnostic ROM for testing
│   ├── test.rom              Test ROM image
│   └── DiagROMv.173          (ROM backup)
│
└── tests/                    Unit tests
    ├── Makefile              Test build configuration
    ├── test_z80.c            Z80 CPU instruction tests
    ├── test_ula.c            ULA (graphics) rendering tests
    ├── test_bit.c            Bit manipulation tests
    ├── test_debug.c          Debug functionality tests
    ├── test_z80.dSYM/        Debug symbols for test_z80
    └── test_ula.dSYM/        Debug symbols for test_ula
```

### Core Modules

| Module | Purpose |
|--------|---------|
| **z80.c/h** | Z80 CPU instruction execution, register management, memory/I/O callbacks |
| **ula.c/h** | Video RAM rendering, converts pixel data to Unicode block characters, 50Hz timing |
| **disasm.c/h** | Z80 instruction disassembly for debugging and logging |
| **keyboard.c/h** | Host system keyboard input mapped to Spectrum keyboard matrix |
| **main.c/h** | Emulator initialization, command-line parsing, main event loop, thread management |

## Testing

Tests are located in the `tests/` directory:

```bash
# Build and run all tests
cd tests
make

# Run individual tests
./test_z80       # Z80 CPU instruction tests
./test_ula       # ULA/graphics rendering tests
./test_bit       # Bit operation tests
```

Each test file validates a specific component:
- **test_z80.c**: Executes Z80 instructions and verifies register changes
- **test_ula.c**: Tests pixel-to-character conversion for display rendering
- **test_bit.c**: Validates bit manipulation operations
- **test_debug.c**: Tests debug/disassembly functionality

## Memory Layout

The Spectrum uses 64KB of addressable memory:

```
0x0000 - 0x3FFF (16 KB)   ROM (read-only)
0x4000 - 0x5AFF (6 KB)    Video RAM (bitmap)
0x5B00 - 0x5BFF (256 B)   Attribute RAM (colors)
0x5C00 - 0xFFFF (42 KB)   User RAM (programs, data, stack)
```

Key memory constants:
- `SPETTRUM_VRAM_START = 0x4000` - Video RAM base address
- `SPETTRUM_VRAM_SIZE = 6912` - Total VRAM size (bitmap + attributes)

## Display Rendering

### Unicode Block Characters (2x2 Mode)

The emulator converts 2x2 pixel blocks to Unicode block characters:

```
0000 → ' '   1000 → ▘   1100 → ▀
0001 → ▗     1001 → ▚   1101 → ▜
0010 → ▖     1010 → ▌   1110 → ▛
0011 → ▄     1011 → ▙   1111 → █
0100 → ▝
0101 → ▐
0110 → ▞
0111 → ▟
```

### Braille Characters (2x4 Mode)

For finer detail, uses Braille unicode patterns.

### Frame Timing

- Maintains **exactly 50Hz** refresh rate (20ms per frame)
- Uses `clock_gettime()` for precise timing
- Sleeps for remaining frame time after rendering
- Microsecond-accurate timing

## Building Individually

To compile specific modules:

```bash
# Compile Z80 CPU module
clang -Wall -Wextra -O2 -pthread -c z80.c -o obj/z80.o

# Compile ULA display module
clang -Wall -Wextra -O2 -pthread -c ula.c -o obj/ula.o

# Compile disassembler
clang -Wall -Wextra -O2 -pthread -c disasm.c -o obj/disasm.o

# Compile keyboard handler
clang -Wall -Wextra -O2 -pthread -c keyboard.c -o obj/keyboard.o

# Link everything into executable
clang -Wall -Wextra -O2 -pthread -o bin/spettrum main.c obj/z80.o obj/ula.o obj/disasm.o obj/keyboard.o
```

## Common Debugging Workflows

### Trace Program Execution
```bash
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --disassemble debug.log
# Examine debug.log for instruction sequence and PC flow
```

### Check for Memory Access Anomalies
- Run emulator normally or with a test ROM
- After completion, check console output for warnings about:
  - PC executing in video RAM (indicates jump into graphics)
  - SP in video RAM (indicates stack corruption)

### Render Mode Comparison
```bash
# Block mode (2x2) - faster but coarser
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --render-mode block

# Braille mode (2x4) - slower but higher resolution
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --render-mode braille
```

## Performance Characteristics

- **No dynamic memory allocation** in the rendering loop
- **Minimal mutex contention** for thread-safe VRAM access
- **Efficient bit operations** for pixel access
- **Direct terminal writes** for low-latency display updates
- **Typical execution speed**: Millions of instructions per second

## Version

Current version: 0.1.0 (Beta)

Check with: `./bin/spettrum --version`

# Spettrum - Z80 Emulator

A high-performance Z80 CPU emulator for the Sinclair ZX Spectrum computer with terminal-based display rendering. The project implements authentic Spectrum emulation with real-time video rendering and comprehensive debugging tools.

## Features

- **Full Z80 CPU Emulation**: Complete instruction set support for the 8-bit Z80 processor
- **ULA Graphics Rendering**: Real-time video RAM to terminal rendering using Unicode block and braille characters
- **50Hz Frame Timing**: Accurate refresh rate matching original Spectrum hardware
- **ROM Loading**: Support for loading Spectrum ROM images
- **Keyboard Emulation**: Host system keyboard mapped to Spectrum keyboard matrix
- **Debugging Tools**: Disassembly logging, CPU state inspection, and anomaly detection
- **Thread-Safe Architecture**: Concurrent CPU execution and terminal rendering with mutex protection

## Building

### Prerequisites

- **clang** compiler (or any C compiler)
- **POSIX-compatible system** (macOS, Linux)
- **pthread** library

### Build Commands

```bash
make              # Build the executable
make debug        # Build with debug symbols
make run          # Run the emulator
make test         # Build and run all tests
```

The executable is generated at `bin/spettrum`.

## Running the Emulator

### Basic Execution

```bash
./bin/spettrum --help                                      # View available options
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom              # Run with 48K ROM
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

## ROM Files

The project includes ROM images in the `rom/` directory:

- **ZX_Spectrum_48k.rom** - Standard Sinclair Spectrum 48K ROM (essential for normal operation)
- **DiagROMv.173.rom** - Spectrum Diagnostic ROM (useful for system verification)
- **test.rom** - Test ROM image

## Project Structure

```
spettrum/
├── z80.c / z80.h           Z80 CPU emulation core
├── ula.c / ula.h           Video RAM (VRAM) to terminal renderer
├── disasm.c / disasm.h     Disassembly and instruction formatting
├── keyboard.c / keyboard.h Keyboard input handling
├── tap.c / tap.h           TAP file format support
├── z80snapshot.c / .h      Z80 snapshot file handling
├── main.c / main.h         Entry point and command-line parsing
├── Makefile                Build system
├── rom/                    ROM images
├── tests/                  Unit tests
└── z80s/                   Z80 snapshot files
```

## Memory Layout

The Spectrum uses 64KB of addressable memory:

```
0x0000 - 0x3FFF   ROM (16 KB)
0x4000 - 0x5AFF   Video RAM (6 KB bitmap)
0x5B00 - 0x5BFF   Attribute RAM (256 B colors)
0x5C00 - 0xFFFF   User RAM (42 KB)
```

## Display Rendering

### Unicode Block Characters (2x2 Mode)

Converts 2x2 pixel blocks to Unicode block characters for efficient terminal display:

```
0000 → ' '   1000 → ▘   1100 → ▀
0001 → ▗     1001 → ▚   1101 → ▜
0010 → ▖     1010 → ▌   1110 → ▛
0011 → ▄     1011 → ▙   1111 → █
0100 → ▝     1101 → ▝
0101 → ▐     1110 → ▞
0110 → ▞     1111 → ▟
```

### Braille Characters (2x4 Mode)

Uses Braille unicode patterns for higher resolution display (default mode).

### Frame Timing

- Maintains exactly **50Hz** refresh rate (20ms per frame)
- Uses `clock_gettime()` for precise timing
- Sleeps for remaining frame time after rendering

## Debugging

### Disassembly Output

Capture detailed instruction-level debugging:

```bash
./bin/spettrum --rom rom/ZX_Spectrum_48k.rom --disassemble debug.log
```

Generated log includes:

- Program Counter (PC)
- Raw opcodes
- Assembly mnemonics
- Operands and addressing modes
- CPU state (registers and flags)

### Debug Features

- **PC History**: Tracks last 10 program counter values
- **Opcode History**: Tracks last 10 executed opcodes
- **Anomaly Detection**: Warns when PC or SP enter video RAM (indicating bugs)
- **Instruction Count**: Total instructions executed
- **CPU State Inspection**: Registers, flags, and memory state

## Testing

Run tests with:

```bash
cd tests
make
./test_z80       # Z80 CPU instruction tests
./test_ula       # Graphics rendering tests
./test_bit       # Bit manipulation tests
```

## Performance

- No dynamic memory allocation in rendering loops
- Efficient bit operations for pixel access
- Minimal mutex contention for thread-safe VRAM access
- Direct terminal writes for low-latency display updates
- Typical execution speed: Millions of instructions per second

## Version

Current version: 0.1.0 (Beta)

Check with: `./bin/spettrum --version`

# ZX Spectrum ULA Terminal Renderer

A high-performance terminal-based renderer for ZX Spectrum video RAM using Unicode block characters, with proper 50Hz frame timing.

## Features

- **Efficient Pixel-to-Character Conversion**: Converts 2x2 pixel blocks from ZX Spectrum VRAM to Unicode block characters (▗, ▖, ▄, etc.)
- **Thread-Safe Rendering**: Uses mutex-protected matrix updates for concurrent VRAM reads and terminal writes
- **50Hz Frame Timing**: Measures rendering time and sleeps for remaining frame duration to maintain exact 50Hz refresh rate
- **Terminal Control**: Uses ANSI escape codes to position cursor at top-left and hide cursor during rendering

## Building

```bash
make test      # Run unit tests
make demo      # Build the demo application
make run-demo  # Run the demo (runs for 10 seconds with animation)
```

## Project Structure

- `ula.c` / `ula.h` - Core ULA (Uncommitted Logic Array) module
  - `convert_vram_to_matrix()` - Convert VRAM to character matrix
  - `ula_render_to_terminal()` - Render matrix with 50Hz timing
  - `ula_render_thread_func()` - Thread function for continuous rendering
- `demo.c` - Demonstration program with animated patterns
- `tests/` - Unit tests for pixel conversion
  - Tests for various pixel patterns and block characters
  - All 10 tests passing

## Memory Layout

ZX Spectrum video RAM uses a specific layout:

- Each byte represents 8 horizontal pixels in a single row
- Byte address: `y * 32 + (x / 8)`
- Bit position: `7 - (x % 8)` (MSB = leftmost pixel)
- Resolution: 256×192 pixels = 128×96 characters

## Rendering Process

1. **Pixel Retrieval**: Get 4 pixels from 2×2 block (TL, TR, BL, BR)
2. **Pattern Generation**: Combine pixels into 4-bit pattern (0-15)
3. **Character Mapping**: Select Unicode block character based on pattern
4. **Frame Timing**:
   - Start 20ms frame timer
   - Write 96 lines × 128 characters to terminal
   - Calculate elapsed time
   - Sleep for remaining time to maintain 50Hz

## 50Hz Timing Details

- Frame duration: 20ms (1/50Hz)
- Timer: Uses `clock_gettime(CLOCK_MONOTONIC, ...)`
- Sleep: Uses `usleep()` for remaining frame time
- Accurate to microsecond precision

## Example Usage

```c
// Set some pixels in VRAM
set_pixel(vram, 0, 0, 1);
set_pixel(vram, 1, 0, 1);

// Update matrix
convert_vram_to_matrix(vram);

// Render to terminal at 50Hz
ula_render_to_terminal();
```

## Unicode Block Characters

The renderer uses 16 Unicode block characters to represent all 2×2 pixel patterns:

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

## Performance Notes

- No dynamic memory allocation in rendering loop
- Minimal mutex contention
- Efficient bit operations for pixel access
- Direct terminal writes (unbuffered fflush for cursor positioning)

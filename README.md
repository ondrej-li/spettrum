# Spettrum - Z80 Emulator

A high-performance Z80 CPU emulator for the Sinclair ZX Spectrum computer with terminal-based display rendering. The project implements authentic Spectrum emulation with real-time video rendering and comprehensive debugging tools.

The emulator is written in Go. A legacy C implementation is kept in the
repository for reference and is not part of the release builds.

## Features

- **Full Z80 CPU Emulation**: Complete instruction set support for the 8-bit Z80 processor
- **ULA Graphics Rendering**: Real-time video RAM to terminal rendering using Unicode block and braille characters
- **50Hz Frame Timing**: Accurate refresh rate matching original Spectrum hardware
- **ROM Loading**: Support for loading Spectrum ROM images
- **Keyboard Emulation**: Host system keyboard mapped to Spectrum keyboard matrix
- **Debugging Tools**: Disassembly logging, CPU state inspection, and anomaly detection
- **Thread-Safe Architecture**: Concurrent CPU execution and terminal rendering with mutex protection

## Installation

Release archives and packages bundle **OpenSE BASIC**, a freely licensed 48K ROM
replacement, so an installed Spettrum runs with no extra downloads. A build from
source or a `go install` build has no ROM installed next to it, so pass one with
`--rom` or `$SPETTRUM_ROM`; see [ROMs](#roms).

### go install

Requires Go 1.25 or newer.

```bash
go install github.com/ondrej-li/spettrum/cmd/spettrum@latest
spettrum --version
```

### Build from source

```bash
git clone https://github.com/ondrej-li/spettrum.git
cd spettrum
go build -trimpath -o bin/spettrum ./cmd/spettrum
```

or with the Go make targets:

```bash
make -f Makefile.golang build    # writes bin/spettrum-go
make -f Makefile.golang test
make -f Makefile.golang vet
make -f Makefile.golang race
```

Release builds stamp the version with
`-ldflags "-X main.version=$(git describe --tags)"`; without it `--version`
reports the module version for `go install` builds and the built-in default
otherwise.

Both produce a CGO-free binary, so no C compiler or audio development headers
are needed. On Linux the result is linked against glibc and the sound card is
opened through PulseAudio or ALSA when available; if neither is present the
emulator still runs, silently.

## Running the Emulator

```bash
spettrum                                       # bundled OpenSE BASIC ROM
spettrum --rom ~/spectrum/ZX_Spectrum_48k.rom  # your own ROM image
spettrum --snapshot game.z80                   # .z80 snapshot
spettrum --tap game.tap                        # .tap cassette image
spettrum --tzx game.tzx                        # .tzx cassette image
spettrum --render-mode braille                 # change the terminal rendering
spettrum --headless --instructions 1000        # no terminal, useful in CI
```

### Command-Line Options

Run `spettrum --help` for the authoritative list. The most used flags:

```
  -rom FILE             Load 16 KB ROM image from file (default: bundled OpenSE BASIC)
  -snapshot FILE        Load .z80 snapshot file
  -tap FILE             Load .tap cassette file
  -tzx FILE             Load .tzx cassette file
  -render-mode MODE     Rendering mode: block, braille, ocr (default "ocr")
  -instructions NUM     Instructions to run (0=unlimited)
  -disassemble FILE     Write disassembly output to FILE
  -audio                Enable audio output (default true)
  -volume N             Audio volume 0-100 (default 50)
  -audio-file FILE      Capture the emulated audio to a WAV file
  -quick-load           Wind the tape instead of playing it in real time (default true)
  -turbo                Run unpaced, as fast as the host allows
  -headless             Run without a terminal or renderer (for tests/scripts)
  -no-terminal          Render without raw mode/alt screen (for pipes and logs)
  -full-refresh         Repaint every cell instead of only the changed ones
  -simulate-key KEY     Simulate a key press for testing
  -version              Show version information
```

## ROMs

### Bundled ROM: OpenSE BASIC

Release archives and packages ship **OpenSE BASIC 3.2.1**, a freely licensed
drop-in replacement for the 48K ROM:

- Copyright 1981 Nine Tiles Networks Ltd; 2011–2020 Source Solutions, Inc.
- Licensed GPL-2.0-or-later (alternatively CC BY-SA 3.0) — the license text is in
  `third_party/opense-basic/COPYING`, with the annotated source in
  `third_party/opense-basic/seBasic321.asm`
- Upstream: <https://zxdesign.itch.io/opense>, packaged for Debian as
  `opense-basic`

It is a complete 16 KB image that boots to a working BASIC editor; its banner
reads `© 1981 Nine Tiles Networks Ltd`. Because it is not the Amstrad ROM,
software that checks the original banner or calls absolute mid-ROM helper
addresses will notice the difference.

Spettrum's own code is MIT. OpenSE BASIC is a separate GPL-2.0-or-later data file
shipped next to the binary, not linked into it, so the two licenses stay
separate — worth knowing if you package Spettrum for a distribution.

### How Spettrum finds the ROM

In order:

1. the `--rom` flag,
2. the `SPETTRUM_ROM` environment variable,
3. `opense.rom` in the directory holding the binary, in `share/spettrum` beside
   or one level above it (`/usr/bin/spettrum` → `/usr/share/spettrum`), in
   `third_party/opense-basic` of a source checkout, in the current directory or
   its `rom/` subdirectory, and in `$XDG_DATA_HOME/spettrum`,
   `~/.local/share/spettrum`, `/usr/local/share/spettrum` and
   `/usr/share/spettrum`.

If nothing is found, Spettrum exits with a message saying where it looked for a
ROM.

### Using the original Amstrad ROM

The original ROM is not bundled: it is Amstrad's copyright, not open source, and
never has been. If you want it, point `--rom` at a copy you obtained:

```bash
spettrum --rom ~/spectrum/ZX_Spectrum_48k.rom
```

The image must be exactly 16384 bytes, with no header or container around it.
A 48K ROM can be obtained from:

- the [World of Spectrum ROM archive](https://worldofspectrum.org/archive/software/roms),
- the [Spectrum Computing](https://spectrumcomputing.co.uk/) archive,
- the data files shipped with other open-source Spectrum emulators, such as
  [Fuse](https://fuse-emulator.sourceforge.net/) (`48.rom`),
- your own Spectrum, by dumping the ROM from the machine (most flash-cartridge
  tools can do this).

Amstrad has publicly permitted the distribution of Spectrum ROM images for use
with emulators, so passing an image on is normally fine — but that permission is
between you and Amstrad, not something Spettrum grants, and it is narrower than an
open source license. Keep those images out of packages.

The `rom/` directory is git-ignored and only holds images you keep locally;
`.gitignore` excludes `*.rom` everywhere except the bundled OpenSE BASIC image.

## Project Structure

```
spettrum/
├── cmd/spettrum/          CLI entry point: flags, version, wiring
├── internal/emulator/     Module integration and main loop
├── third_party/opense-basic/  Bundled OpenSE BASIC ROM + license + source
├── pkg/z80/               Z80 CPU core and instruction set
├── pkg/ula/               Video RAM to terminal renderer (block/braille/ocr)
├── pkg/keyboard/          Spectrum keyboard matrix
├── pkg/beeper/            1-bit audio synthesis and host audio output
├── pkg/tap/, pkg/tzx/     Cassette image loaders
├── pkg/snapshot/          .z80 snapshot loader
├── Makefile.golang        Go build and test targets
├── Makefile               Legacy C build
└── tests/                 Legacy C unit tests
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

```bash
go test ./...          # all Go tests
go test -race ./...    # with the race detector
```

The legacy C implementation keeps its own tests in `tests/`:

```bash
cd tests && make
```

## Performance

- No dynamic memory allocation in rendering loops
- Efficient bit operations for pixel access
- Minimal mutex contention for thread-safe VRAM access
- Direct terminal writes for low-latency display updates
- Typical execution speed: Millions of instructions per second

## Version

Current version: 0.2.0

Check with: `spettrum --version`

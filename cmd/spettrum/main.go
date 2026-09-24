// Spettrum — Z80 Spectrum emulator (Go port)
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/defik74/spettrum/internal/emulator"
	"github.com/defik74/spettrum/pkg/beeper"
	"github.com/defik74/spettrum/pkg/ula"
)

const (
	versionMajor = 0
	versionMinor = 2
	versionPatch = 0
)

func main() {
	romFile := flag.String("rom", "", "Load ROM from file (default: embedded 48K ROM)")
	snapshotFile := flag.String("snapshot", "", "Load .z80 snapshot file")
	tapFile := flag.String("tap", "", "Load .tap cassette file")
	instructions := flag.Int("instructions", 0, "Instructions to run (0=unlimited)")
	disasmFile := flag.String("disassemble", "", "Write disassembly output to file")
	renderMode := flag.String("render-mode", "ocr", "Rendering mode: block, braille, ocr")
	simKey := flag.String("simulate-key", "", "Simulate a key press for testing")
	audio := flag.Bool("audio", true, "Enable audio output")
	volume := flag.Int("volume", 50, "Audio volume 0-100")
	audioFile := flag.String("audio-file", "", "Capture the emulated audio to a WAV file")
	audioRate := flag.Int("audio-rate", beeper.DefaultSampleRate, "Audio sample rate in Hz")
	audioLatency := flag.Int("audio-latency", int(beeper.DefaultLatency/time.Millisecond), "Audio buffer length in milliseconds")
	audioSelfTest := flag.Bool("audio-selftest", false, "Play a test tone and exit")
	quickLoad := flag.Bool("quick-load", true, "Wind the tape past at full speed instead of playing it in real time")
	headless := flag.Bool("headless", false, "Run without a terminal or renderer (for tests/scripts)")
	noTerminal := flag.Bool("no-terminal", false, "Render frames without raw mode/alt screen (for pipes and logs)")
	turbo := flag.Bool("turbo", false, "Run unpaced, as fast as the host allows")
	showVersion := flag.Bool("version", false, "Show version information")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Spettrum v%d.%d.%d — ZX Spectrum Emulator (Go)\n\n",
			versionMajor, versionMinor, versionPatch)
		fmt.Fprintf(os.Stderr, "Usage: %s [flags]\n\nFlags:\n", os.Args[0])
		flag.PrintDefaults()
	}

	flag.Parse()

	if *showVersion {
		fmt.Printf("Spettrum v%d.%d.%d (Go)\n", versionMajor, versionMinor, versionPatch)
		return
	}

	// Parse render mode
	var mode ula.RenderMode
	switch strings.ToLower(*renderMode) {
	case "block":
		mode = ula.RenderBlock
	case "braille":
		mode = ula.RenderBraille
	case "ocr":
		mode = ula.RenderOCR
	default:
		fmt.Fprintf(os.Stderr, "Invalid render mode: %s (use block, braille, or ocr)\n", *renderMode)
		os.Exit(1)
	}

	cfg := emulator.Config{
		ROMFile:      *romFile,
		SnapshotFile: *snapshotFile,
		TAPFile:      *tapFile,
		Instructions: *instructions,
		DisasmFile:   *disasmFile,
		RenderMode:   mode,
		SimKey:       *simKey,
		Audio:        *audio,
		Volume:       *volume,
		AudioFile:    *audioFile,
		AudioRate:    *audioRate,
		AudioLatency: time.Duration(*audioLatency) * time.Millisecond,
		QuickLoad:    *quickLoad,
		Headless:     *headless,
		NoTerminal:   *noTerminal,
		Unpaced:      *turbo,
	}

	if *audioSelfTest {
		if err := emulator.AudioSelfTest(cfg); err != nil {
			fmt.Fprintf(os.Stderr, "audio self test: %v\n", err)
			os.Exit(1)
		}
		return
	}

	emu := emulator.New(cfg)
	defer emu.Close()

	if err := emu.Init(); err != nil {
		fmt.Fprintf(os.Stderr, "Init error: %v\n", err)
		os.Exit(1)
	}

	if err := emu.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "Run error: %v\n", err)
		os.Exit(1)
	}
}

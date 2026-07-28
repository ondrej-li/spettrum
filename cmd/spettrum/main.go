// Spettrum — Z80 Spectrum emulator (Go port)
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/defik74/spettrum/internal/emulator"
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
	audio := flag.Bool("audio", true, "Enable audio")
	volume := flag.Int("volume", 50, "Audio volume 0-100")
	quickLoad := flag.Bool("quick-load", true, "Fast tape loading")
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
		QuickLoad:    *quickLoad,
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

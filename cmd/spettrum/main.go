// Spettrum — Z80 Spectrum emulator (Go port)
package main

import (
	"flag"
	"fmt"
	"os"
	"regexp"
	"runtime/debug"
	"strings"
	"time"

	"github.com/ondrej-li/spettrum/internal/emulator"
	"github.com/ondrej-li/spettrum/pkg/beeper"
	"github.com/ondrej-li/spettrum/pkg/ula"
)

// version is stamped into release binaries with -ldflags "-X main.version=1.2.3".
// It stays empty for local builds, where the module version recorded by
// `go install ...@vX.Y.Z` is used instead, falling back to defaultVersion.
var version = ""

// defaultVersion is reported by a plain local `go build` outside a release.
const defaultVersion = "0.2.0"

// pseudoVersion matches the pre-release part of a module pseudo-version
// ("v0.0.0-20240101120000-abcdef123456"), which `go build` records for a local
// checkout and which says nothing about the released version.
var pseudoVersion = regexp.MustCompile(`-\d{14}-`)

// buildVersion returns the version of this build, without a leading "v".
func buildVersion() string {
	if version != "" {
		return strings.TrimPrefix(version, "v")
	}
	if info, ok := debug.ReadBuildInfo(); ok {
		if v := info.Main.Version; v != "" && v != "(devel)" && !pseudoVersion.MatchString(v) {
			return strings.TrimPrefix(v, "v")
		}
	}
	return defaultVersion
}

func main() {
	romFile := flag.String("rom", "", "Load 16 KB ROM image from file (default: bundled OpenSE BASIC)")
	snapshotFile := flag.String("snapshot", "", "Load .z80 snapshot file")
	tapFile := flag.String("tap", "", "Load .tap cassette file")
	tzxFile := flag.String("tzx", "", "Load .tzx cassette file")
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
	fullRefresh := flag.Bool("full-refresh", false, "Repaint the whole frame every time instead of only the cells that changed")
	headless := flag.Bool("headless", false, "Run without a terminal or renderer (for tests/scripts)")
	noTerminal := flag.Bool("no-terminal", false, "Render frames without raw mode/alt screen (for pipes and logs)")
	turbo := flag.Bool("turbo", false, "Run unpaced, as fast as the host allows")
	showVersion := flag.Bool("version", false, "Show version information")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Spettrum v%s — ZX Spectrum Emulator (Go)\n\n", buildVersion())
		fmt.Fprintf(os.Stderr, "Usage: %s [flags]\n\nFlags:\n", os.Args[0])
		flag.PrintDefaults()
	}

	flag.Parse()

	if *showVersion {
		fmt.Printf("Spettrum v%s (Go)\n", buildVersion())
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
		TZXFile:      *tzxFile,
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
		FullRefresh:  *fullRefresh,
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

	romPath, err := findROM(cfg.ROMFile)
	if err != nil {
		fmt.Fprintln(os.Stderr, "No ROM image found.")
		fmt.Fprintln(os.Stderr, "Release archives and packages ship OpenSE BASIC next to the binary; a build")
		fmt.Fprintln(os.Stderr, "from source or with go install needs one passed explicitly, for example:")
		fmt.Fprintln(os.Stderr, "  spettrum --rom third_party/opense-basic/opense.rom")
		fmt.Fprintln(os.Stderr, "You can also point --rom (or SPETTRUM_ROM) at an original Spectrum ROM.")
		os.Exit(1)
	}
	cfg.ROMFile = romPath

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

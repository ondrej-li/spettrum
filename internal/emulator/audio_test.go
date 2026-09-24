package emulator

import (
	"math"
	"path/filepath"
	"testing"

	"github.com/defik74/spettrum/pkg/beeper"
	"github.com/defik74/spettrum/pkg/z80"
)

// The program below toggles the ULA speaker bit at a fixed rate. The ROM never
// makes a sound on its own, so the CPU is pointed at this instead of at the
// ROM's entry point.
const (
	beepOrigin = 0x8000
	beepDelay  = 120 // DJNZ count, one per half period
)

// beepProgram is:
//
//	LD A,$10     ; speaker high
//	OUT ($FE),A
//	LD B,delay
//	DJNZ $       ; busy wait
//	LD A,$00     ; speaker low
//	OUT ($FE),A
//	LD B,delay
//	DJNZ $       ; busy wait
//	JP beepOrigin
var beepProgram = []byte{
	0x3E, 0x10,
	0xD3, 0xFE,
	0x06, beepDelay,
	0x10, 0xFE,
	0x3E, 0x00,
	0xD3, 0xFE,
	0x06, beepDelay,
	0x10, 0xFE,
	0xC3, beepOrigin & 0xFF, beepOrigin >> 8,
}

// TestBeeperReachesTheAudioFile runs the beeper program and checks the audio it
// captured. It covers the whole chain at once: the OUT instruction reaching
// WriteIO, the cycle count stamped on it, the synthesiser and the WAV writer.
//
// The two things that can silently go wrong are asserted separately. The tone
// proves the pin changes arrive in the right order with the right timing, and
// the length proves the sample timeline follows emulated time rather than the
// host's, which is what a drifting audio clock would break.
func TestBeeperReachesTheAudioFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "beep.wav")

	emu := New(Config{
		Headless:     true,
		Instructions: 200_000,
		Audio:        false, // a test machine has no sound card
		AudioFile:    path,
		Volume:       50,
	})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	for i, b := range beepProgram {
		emu.WriteMemory(uint16(beepOrigin+i), b)
	}
	emu.cpu.Regs.PC = beepOrigin
	emu.cpu.Regs.SP = 0xFF00

	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	// Closes the sink, which is what completes the WAV header.
	emu.Close()

	samples, rate, channels, err := beeper.ReadWav(path)
	if err != nil {
		t.Fatalf("read capture: %v", err)
	}
	if rate != beeper.DefaultSampleRate {
		t.Errorf("captured at %d Hz, want %d", rate, beeper.DefaultSampleRate)
	}
	if channels != beeper.Channels {
		t.Errorf("captured %d channels, want %d", channels, beeper.Channels)
	}

	// The capture has to cover exactly the time the CPU ran for, no more and no
	// less. Integer arithmetic, so this is an equality and not an approximation.
	frames := len(samples) / channels
	wantFrames := int(emu.cpu.Cycles * uint64(rate) / uint64(z80.ClockFreq))
	if frames != wantFrames {
		t.Errorf("captured %d frames for %d emulated cycles, want %d",
			frames, emu.cpu.Cycles, wantFrames)
	}

	// And it has to be the tone the program played.
	//
	// One half period is LD B (7) + the DJNZ loop + LD A (7) + OUT (11), and the
	// low half also carries the JP back to the start.
	const djnz = 13*(beepDelay-1) + 8
	high := 7 + djnz + 7 + 11
	low := 7 + djnz + 7 + 11 + 10
	wantHz := float64(z80.ClockFreq) / float64(high+low)

	gotHz := toneFrequency(leftChannel(samples, channels), rate)
	if math.Abs(gotHz-wantHz) > 0.02*wantHz {
		t.Errorf("captured a %.1f Hz tone, want %.1f Hz", gotHz, wantHz)
	}
}

// TestBeeperSilenceWithoutAudioWrites checks the other half of the wiring: a
// machine that never touches the speaker bit must produce a silent capture
// rather than noise from an uninitialised synthesiser.
func TestBeeperSilenceWithoutAudioWrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "silent.wav")

	emu := New(Config{
		Headless:     true,
		Instructions: 20_000,
		Audio:        false,
		AudioFile:    path,
		Volume:       50,
	})
	if err := emu.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	defer emu.Close()

	// Run the ROM, which writes the speaker bit but never changes it.
	if err := emu.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	emu.Close()

	samples, _, _, err := beeper.ReadWav(path)
	if err != nil {
		t.Fatalf("read capture: %v", err)
	}
	if len(samples) == 0 {
		t.Fatal("nothing was captured")
	}
	peak := 0.0
	for _, s := range samples {
		peak = math.Max(peak, math.Abs(float64(s)))
	}
	if peak > 0.001 {
		t.Errorf("an idle machine produced a peak of %.4f, want silence", peak)
	}
}

// leftChannel drops the right channel from interleaved stereo audio.
func leftChannel(samples []float32, channels int) []float32 {
	out := make([]float32, 0, len(samples)/channels)
	for i := 0; i+channels <= len(samples); i += channels {
		out = append(out, samples[i])
	}
	return out
}

// toneFrequency estimates the pitch of the captured audio from its zero
// crossings, skipping the first 50ms while the synthesiser's filters settle.
func toneFrequency(samples []float32, sampleRate int) float64 {
	settle := sampleRate / 20
	if settle >= len(samples) {
		return 0
	}
	rest := samples[settle:]
	crossings := 0
	prev := 0
	for _, v := range rest {
		sign := 0
		switch {
		case v > 0:
			sign = 1
		case v < 0:
			sign = -1
		default:
			continue
		}
		if prev != 0 && sign != prev {
			crossings++
		}
		prev = sign
	}
	return float64(crossings) / 2 * float64(sampleRate) / float64(len(rest))
}

package beeper

import (
	"math"
	"testing"
)

const (
	testClockFreq  = 3_500_000
	testSampleRate = 44100
)

// TestSynthSampleCountMatchesEmulatedTime checks that the sample timeline is a
// function of emulated time alone: one second of CPU cycles has to yield one
// second of samples whatever chunk sizes the emulator happens to feed in. A
// synthesiser that rounded each chunk to a whole number of samples would instead
// gain or lose samples at every call.
func TestSynthSampleCountMatchesEmulatedTime(t *testing.T) {
	for _, chunk := range []int{1, 7, 1000, 20259, testClockFreq / 50} {
		if got := synthSampleCount(chunk); got != testSampleRate {
			t.Errorf("chunk %d: one second of cycles produced %d samples, want %d",
				chunk, got, testSampleRate)
		}
	}
}

// synthSampleCount feeds one second of cycles in the given chunk size and
// returns how many samples came out.
func synthSampleCount(chunk int) int {
	var out []float32
	total := 0
	s := NewSynth(testClockFreq, testSampleRate)
	for left := testClockFreq; left > 0; {
		n := min(chunk, left)
		out = s.Feed(n, 1, out[:0])
		total += len(out)
		left -= n
	}
	return total
}

// TestSynthDoesNotDriftOverLongRuns feeds ten minutes of emulated time in
// frame-sized chunks. A synthesiser that derived its position from wall-clock
// time, or from a running rate, would be short or long by hundreds of samples;
// one tied to the cycle counter is out by less than a sample.
func TestSynthDoesNotDriftOverLongRuns(t *testing.T) {
	const seconds = 600
	const frame = testClockFreq / 50 // 70000 cycles, as in the emulator

	var out []float32
	total := 0
	s := NewSynth(testClockFreq, testSampleRate)
	for left := testClockFreq * seconds; left > 0; {
		n := min(frame, left)
		out = s.Feed(n, 1, out[:0])
		total += len(out)
		left -= n
	}

	want := seconds * testSampleRate
	if diff := total - want; diff < -1 || diff > 1 {
		t.Errorf("%d seconds produced %d samples, want %d (out by %d)", seconds, total, want, diff)
	}
}

// TestSynthProducesTheEmulatedFrequency toggles the pin as the CPU would for a
// 1kHz square wave and measures the tone that comes back out.
func TestSynthProducesTheEmulatedFrequency(t *testing.T) {
	const wantHz = 1000.0
	const seconds = 1

	// A Z80 OUT loop at 1kHz toggles the pin every 1750 T-states.
	const halfPeriod = testClockFreq / (2 * wantHz)

	var out []float32
	s := NewSynth(testClockFreq, testSampleRate)
	level := 1.0
	for left := testClockFreq * seconds; left > 0; {
		n := min(int(halfPeriod), left)
		out = s.Feed(n, level, out)
		level = -level
		left -= n
	}

	gotHz := toneFrequency(out, testSampleRate)
	if math.Abs(gotHz-wantHz) > 0.01*wantHz {
		t.Errorf("measured %.1f Hz, want %.1f Hz", gotHz, wantHz)
	}
}

// TestSynthKeepsTheDutyCycle checks the pulse width survives synthesis, which is
// what makes the difference between a beep and a buzz audible.
func TestSynthKeepsTheDutyCycle(t *testing.T) {
	// The ROM's beeper routine produces pulses of roughly this shape: 2000
	// cycles low, 1000 cycles high.
	var out []float32
	s := NewSynth(testClockFreq, testSampleRate)
	for i := 0; i < 500; i++ {
		out = s.Feed(2000, -1, out)
		out = s.Feed(1000, 1, out)
	}

	// Skip the filters' settling time and count how much of the rest is positive.
	settled := out[testSampleRate/10:]
	positive := 0
	for _, v := range settled {
		if v > 0 {
			positive++
		}
	}
	got := float64(positive) / float64(len(settled))
	if math.Abs(got-1.0/3) > 0.02 {
		t.Errorf("duty cycle measured as %.3f, want %.3f", got, 1.0/3)
	}
}

// TestSynthSilencesAStaticPin checks the DC blocker. The pin level is unipolar,
// so a machine sitting idle would otherwise hold a constant offset on the
// speaker, which thumps when sound starts and clips when the volume is raised.
func TestSynthSilencesAStaticPin(t *testing.T) {
	var out []float32
	s := NewSynth(testClockFreq, testSampleRate)
	for i := 0; i < 50; i++ {
		out = s.Feed(testClockFreq/50, -1, out)
	}

	peak := 0.0
	for _, v := range out[testSampleRate/10:] { // allow the filter to settle
		peak = math.Max(peak, math.Abs(float64(v)))
	}
	if peak > 0.001 {
		t.Errorf("a static pin produced a peak of %.4f, want silence", peak)
	}
}

// TestSynthOutputIsSane feeds an extreme signal - the pin toggling every few
// cycles - and checks nothing in the filter chain blows up or goes non-finite.
func TestSynthOutputIsSane(t *testing.T) {
	var out []float32
	s := NewSynth(testClockFreq, testSampleRate)
	for i := 0; i < 500; i++ {
		out = s.Feed(8, 1, out)
		out = s.Feed(4, -1, out)
	}

	for i, v := range out {
		if math.IsNaN(float64(v)) || math.IsInf(float64(v), 0) {
			t.Fatalf("sample %d is %v", i, v)
		}
		if v > 1.5 || v < -1.5 {
			t.Fatalf("sample %d is %v, well outside the signal range", i, v)
		}
	}
}

// toneFrequency estimates the pitch of a periodic signal from its zero
// crossings, skipping the first 50ms while the filters settle.
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
	return float64(crossings) / 2 / (float64(len(rest)) / float64(sampleRate))
}

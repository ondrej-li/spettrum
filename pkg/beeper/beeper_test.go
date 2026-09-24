package beeper

import (
	"math"
	"slices"
	"testing"
)

// captureSink records the samples written to it, so the player can be tested
// without a sound card.
type captureSink struct {
	samples []float32
	closed  bool
	err     error
}

func (c *captureSink) WriteSamples(pcm []float32) error {
	if c.err != nil {
		return c.err
	}
	c.samples = append(c.samples, pcm...)
	return nil
}

func (c *captureSink) Close() error {
	c.closed = true
	return nil
}

func testPlayer(sink Sink) *Player {
	return NewPlayer(Config{
		ClockFreq:  testClockFreq,
		SampleRate: testSampleRate,
		Volume:     1,
		Sink:       sink,
	})
}

// TestPlayerSampleCountFollowsEmulatedTime checks the player's half of the
// timing contract: the number of samples it emits is decided by the CPU cycles
// it is given, not by how many times EndFrame was called or how long each call
// took.
func TestPlayerSampleCountFollowsEmulatedTime(t *testing.T) {
	const (
		frames      = 50
		frameCycles = 20259
	)
	sink := &captureSink{}
	p := testPlayer(sink)

	end := uint64(0)
	for f := 0; f < frames; f++ {
		end += frameCycles
		// Something for the synthesiser to do: toggle mid-frame.
		p.Update(end-frameCycles/2, f%2 == 0)
		if err := p.EndFrame(end); err != nil {
			t.Fatalf("EndFrame %d: %v", f, err)
		}
	}

	want := int(math.Floor(float64(end) * testSampleRate / testClockFreq))
	got := len(sink.samples) / Channels
	if diff := got - want; diff < -1 || diff > 1 {
		t.Errorf("%d cycles produced %d sample frames, want about %d", end, got, want)
	}
}

// TestPlayerIgnoresUnchangedLevels checks that only transitions reach the
// synthesiser. The ROM writes the speaker bit on every scanline whether it is
// changing or not, so a Level that is written twice is not a second event.
func TestPlayerIgnoresUnchangedLevels(t *testing.T) {
	noisy, quiet := &captureSink{}, &captureSink{}
	a, b := testPlayer(noisy), testPlayer(quiet)

	// The pin starts low, so the first write here changes nothing.
	a.Update(10, false)
	a.Update(20, true)
	a.Update(30, true)
	a.Update(40, false)
	b.Update(20, true)
	b.Update(40, false)

	for _, p := range []*Player{a, b} {
		if err := p.EndFrame(100_000); err != nil {
			t.Fatalf("EndFrame: %v", err)
		}
	}
	if !slices.Equal(noisy.samples, quiet.samples) {
		t.Error("a repeated pin level produced different audio; unchanged writes reached the synthesiser")
	}
}

// TestPlayerWithholdsSamplesUntilTheirTimeComes checks that EndFrame only
// renders the time it is told about. Rendering ahead would make the audio depend
// on how the emulator is called rather than on the emulated clock.
func TestPlayerWithholdsSamplesUntilTheirTimeIsDue(t *testing.T) {
	sink := &captureSink{}
	p := testPlayer(sink)

	if err := p.EndFrame(0); err != nil {
		t.Fatalf("EndFrame(0): %v", err)
	}
	if len(sink.samples) != 0 {
		t.Fatalf("EndFrame(0) produced %d samples, want none", len(sink.samples))
	}

	// Half a sample period cannot complete a sample.
	half := uint64(testClockFreq / testSampleRate / 2)
	if err := p.EndFrame(half); err != nil {
		t.Fatalf("EndFrame(half): %v", err)
	}
	if len(sink.samples) != 0 {
		t.Fatalf("half a sample period produced %d samples, want none", len(sink.samples))
	}

	// Rewinding (a repeated or stale cycle count) must be ignored rather than
	// producing samples out of order.
	if err := p.EndFrame(0); err != nil {
		t.Fatalf("EndFrame(0) again: %v", err)
	}
	if len(sink.samples) != 0 {
		t.Fatalf("going backwards produced %d samples, want none", len(sink.samples))
	}
}

// TestPlayerAppliesVolumeAndChannels checks the output shape: the Spectrum has
// one speaker, so both channels carry the same signal, scaled by the volume.
func TestPlayerAppliesVolumeAndChannels(t *testing.T) {
	const volume = 0.25
	sink := &captureSink{}
	p := NewPlayer(Config{
		ClockFreq:  testClockFreq,
		SampleRate: testSampleRate,
		Volume:     volume,
		Sink:       sink,
	})

	// A square wave rather than a single step: what should be measured here is
	// the tone, not the click that a step out of silence makes (which is twice
	// as loud, as it is on the real machine's AC coupled output).
	const half = 1750
	level := true
	for at := 0; at < testClockFreq/2; at += half {
		p.Update(uint64(at), level)
		level = !level
	}
	if err := p.EndFrame(testClockFreq / 2); err != nil {
		t.Fatalf("EndFrame: %v", err)
	}
	if len(sink.samples) == 0 {
		t.Fatal("no samples produced")
	}
	if len(sink.samples)%Channels != 0 {
		t.Fatalf("got %d samples, not a whole number of frames", len(sink.samples))
	}

	full := 0.0
	for i := 0; i < len(sink.samples); i += Channels {
		if sink.samples[i] != sink.samples[i+1] {
			t.Fatalf("frame %d differs between channels: %v, %v",
				i/Channels, sink.samples[i], sink.samples[i+1])
		}
		full = math.Max(full, math.Abs(float64(sink.samples[i])))
	}
	if full > 1 {
		t.Errorf("peak %.3f exceeds full scale", full)
	}

	// Once the initial click has decayed, a half scale square wave sits at the
	// configured volume.
	settled := sink.samples[len(sink.samples)/2:]
	peak := 0.0
	for i := 0; i < len(settled); i += Channels {
		peak = math.Max(peak, math.Abs(float64(settled[i])))
	}
	if math.Abs(peak-volume) > 0.01 {
		t.Errorf("settled peak is %.3f, want about %.3f", peak, volume)
	}
}

// TestPlayerClosesItsSink checks the sink is released, which is what finishes a
// WAV capture.
func TestPlayerClosesItsSink(t *testing.T) {
	sink := &captureSink{}
	p := testPlayer(sink)
	if err := p.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if !sink.closed {
		t.Error("closing the player did not close the sink")
	}
}

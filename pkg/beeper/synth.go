package beeper

import "math"

// filterCutoff is the cutoff of the synthesiser's one-pole low-pass filter. A
// square wave's harmonics run to infinity, and everything above Nyquist folds
// back into the audible range as a metallic hiss, so it has to be rolled off.
// 12 kHz keeps the harmonics that carry the tone and removes the ones that
// cannot be represented.
const filterCutoff = 12000.0

// dcBlockerPole sets the corner of the DC blocker at a few Hz. Low enough not to
// eat the bass of a low beep, high enough to remove the offset within a few
// milliseconds.
const dcBlockerPole = 0.9995

// Synth turns a stream of beeper pin levels, timestamped in CPU cycles, into PCM
// samples.
//
// A sample is produced for every clockFreq/sampleRate CPU cycles and holds the
// average pin level over that window. Averaging is not just a convenience, it is
// the anti-aliasing filter: the pin can be toggled far faster than the sample
// rate (an OUT loop reaches tens of kHz), and taking the instantaneous level at
// each sample point would alias all of that energy back down as an audible
// whistle.
//
// The result is then filtered, because a raw 1-bit output makes a poor
// loudspeaker signal: the Spectrum drives its speaker through a coupling
// capacitor, which is what stops the constant DC level of a unipolar output from
// pinning the cone at one end of its travel.
type Synth struct {
	clockFreq  int
	sampleRate int

	// pos is how many cycles have been fed in, counting from the start, and
	// emitted is how many samples have been produced. Sample boundaries are
	// derived from them in integer arithmetic, so the sample timeline can never
	// drift from the cycle timeline.
	pos     int64
	emitted int64
	// windowStart is where the sample being built began. A window is normally
	// assembled from several Feed calls, so its length is only known when it
	// closes.
	windowStart int64

	// accSum is the integral of the pin level over the part of the current
	// sample window that has already been fed in.
	accSum float64

	lp      float64 // low-pass filter state
	dcPrevX float64 // DC blocker input history
	dcPrevY float64 // DC blocker output history

	lpAlpha float64 // one-pole low-pass coefficient
	dcR     float64 // DC blocker pole
}

// NewSynth creates a synthesiser for a CPU running at clockFreq Hz whose audio
// is sampled at sampleRate Hz.
func NewSynth(clockFreq, sampleRate int) *Synth {
	if clockFreq <= 0 {
		clockFreq = 1
	}
	if sampleRate <= 0 {
		sampleRate = DefaultSampleRate
	}
	// One sample per cycle is the limit of placing sample boundaries on whole
	// cycles, and is far above any real audio rate. The clamp only stops a
	// nonsensical configuration from producing zero length windows.
	if sampleRate > clockFreq {
		sampleRate = clockFreq
	}

	// Keep the cutoff below Nyquist even at low sample rates.
	cutoff := math.Min(filterCutoff, 0.45*float64(sampleRate))
	twoPiFc := 2 * math.Pi * cutoff

	// The pin is low at power-on and the machine is silent, so the filters start
	// where a pin that has been low for ever would leave them. Starting them at
	// zero instead would turn the first, constant half of the waveform into a
	// step, which the DC blocker would then let through as a settling thump.
	low := levelValue(false)
	return &Synth{
		clockFreq:  clockFreq,
		sampleRate: sampleRate,
		lpAlpha:    twoPiFc / (float64(sampleRate) + twoPiFc),
		dcR:        dcBlockerPole,
		lp:         low,
		dcPrevX:    low,
	}
}

// Feed advances the synthesiser by cycles CPU cycles during which the beeper pin
// was held at level (either -1 or +1) and appends every sample that completes to
// dst, which it returns.
//
// Sample boundaries fall on whole cycles, and the arithmetic that finds them is
// integer arithmetic, so the number of samples produced over a run is exactly
// elapsedCycles * sampleRate / clockFreq. No rate error can accumulate, and the
// result does not depend on the sizes of the chunks the emulator happens to feed
// in.
func (s *Synth) Feed(cycles int, level float64, dst []float32) []float32 {
	if cycles <= 0 {
		return dst
	}
	end := s.pos + int64(cycles)
	clock := int64(s.clockFreq)
	rate := int64(s.sampleRate)

	for {
		// The cycle on which the next sample window ends: the smallest position
		// past this one that satisfies boundary*sampleRate >= k*clockFreq.
		boundary := ((s.emitted+1)*clock + rate - 1) / rate
		if boundary > end {
			break
		}
		n := boundary - s.pos
		s.accSum += level * float64(n)
		dst = append(dst, float32(s.filter(s.accSum/float64(boundary-s.windowStart))))
		s.accSum = 0
		s.pos = boundary
		s.windowStart = boundary
		s.emitted++
	}
	if end > s.pos {
		s.accSum += level * float64(end-s.pos)
		s.pos = end
	}
	return dst
}

// filter shapes one averaged pin level into the sample that is written out.
func (s *Synth) filter(v float64) float64 {
	// One-pole low-pass.
	s.lp += s.lpAlpha * (v - s.lp)

	// DC blocker: a one-pole high-pass that subtracts a slowly tracked mean. The
	// pin level is unipolar in hardware, so without this the samples would sit
	// at a constant offset: inaudible on its own, but it clips as soon as the
	// volume is raised and thumps whenever sound starts or stops.
	//
	// Note what this does to a level change out of silence: the step is twice the
	// height of a full square wave, so the attack of a sound is a click that
	// loud. That is what a real Spectrum does too - the speaker is driven through
	// a coupling capacitor - and the clamp in Player is what keeps it inside full
	// scale.
	y := s.lp - s.dcPrevX + s.dcR*s.dcPrevY
	s.dcPrevX = s.lp
	s.dcPrevY = y
	return y
}

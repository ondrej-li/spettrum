// Package beeper provides ZX Spectrum 1-bit audio emulation: it turns the CPU's
// writes to the ULA speaker bit into PCM audio for the host sound card.
//
// The machine has a single output line, so synthesis here is the job of turning
// a stream of pin transitions into samples. The rule that keeps the result in
// tune is that emulated time is the only clock: every sample is derived from the
// CPU T-state counter, so the audio cannot drift away from the 50Hz frame rate
// and never depends on how fast the host happens to be running.
package beeper

import (
	"errors"
	"math"
	"time"
)

const (
	// DefaultSampleRate is the sample rate used when none is configured.
	DefaultSampleRate = 44100
	// Channels is the channel count of the generated PCM. The Spectrum has one
	// speaker, so both channels carry the same signal.
	Channels = 2
	// DefaultLatency is the delay the audio output is tuned for. A whole frame of
	// samples is queued in one burst every 20ms, so the output has to buffer
	// several frames to smooth that pattern out; the same figure is also the
	// delay between the machine doing something and the user hearing it, so it is
	// kept as small as the burst pattern allows.
	DefaultLatency = 20 * time.Millisecond
)

// Sink receives the PCM a Player produces, as interleaved stereo float32 samples
// in the range -1..1.
type Sink interface {
	// WriteSamples queues samples for output.
	WriteSamples(pcm []float32) error
	// Close releases the sink. Calling it more than once is allowed.
	Close() error
}

// MultiSink fans the audio out to several sinks, so a run can be played and
// captured at the same time. A failure in one sink is reported but does not stop
// the others.
type MultiSink []Sink

// WriteSamples implements Sink.
func (m MultiSink) WriteSamples(pcm []float32) error {
	var err error
	for _, s := range m {
		err = errors.Join(err, s.WriteSamples(pcm))
	}
	return err
}

// Close implements Sink.
func (m MultiSink) Close() error {
	var err error
	for _, s := range m {
		err = errors.Join(err, s.Close())
	}
	return err
}

// Config configures a Player.
type Config struct {
	// ClockFreq is the CPU clock in T-states per second.
	ClockFreq int
	// SampleRate is the output sample rate in Hz.
	SampleRate int
	// Volume scales the output, 0 to 1.
	Volume float64
	// Sink receives the samples. A Player without a sink produces nothing.
	Sink Sink
}

// event is one change of the beeper pin.
type event struct {
	cycle uint64  // CPU T-state at which the change took effect
	level float64 // -1 while the pin is low, +1 while it is high
}

// Player turns the emulated beeper pin into PCM and feeds it to a Sink.
//
// The emulator drives it with Update for every write to the ULA speaker bit, and
// with EndFrame once per frame. Both are called from the emulation thread only,
// so the audio path needs no locking. EndFrame is allowed to block - a device
// sink waits for the sound card to consume the previous frame - and that is what
// keeps the emulator's own speed tied to the audio output.
type Player struct {
	synth  *Synth
	sink   Sink
	volume float64

	// events holds the pin changes seen during the current frame, in cycle
	// order; pin is the level they have been recorded up to.
	events []event
	pin    bool

	// lastCycle is the end of the emulated time already turned into samples and
	// level is the pin level the synthesiser is currently holding.
	lastCycle uint64
	level     float64

	// Scratch buffers, reused so a frame of audio costs no allocation.
	mono   []float32
	stereo []float32
}

// NewPlayer creates a player for the given CPU clock, sample rate and sink.
func NewPlayer(cfg Config) *Player {
	if cfg.SampleRate <= 0 {
		cfg.SampleRate = DefaultSampleRate
	}
	return &Player{
		synth:  NewSynth(cfg.ClockFreq, cfg.SampleRate),
		sink:   cfg.Sink,
		volume: clamp(cfg.Volume, 0, 1),
		level:  levelValue(false),
	}
}

// Update records a write to the ULA speaker bit at the given CPU cycle.
//
// Only transitions are kept. The pin is written far more often than it changes
// (the ROM writes it once per scanline even when nothing is playing), and the
// synthesiser only needs to know when the level actually moved.
func (p *Player) Update(cpuCycle uint64, high bool) {
	if high == p.pin {
		return
	}
	p.pin = high
	p.events = append(p.events, event{cycle: cpuCycle, level: levelValue(high)})
}

// EndFrame turns the emulated time since the previous call into samples and hands
// them to the sink. endCycle is the CPU's cycle counter, which advances with the
// emulated T-states and not with the wall clock.
//
// Because the sample timeline is a function of the cycle counter alone, a slow
// host produces the same audio as a fast one, only later; there is nothing to
// drift and nothing to re-synchronise.
func (p *Player) EndFrame(endCycle uint64) error {
	if p.sink == nil || endCycle <= p.lastCycle {
		return nil
	}
	pos := p.lastCycle

	p.mono = p.mono[:0]
	i := 0
	for ; i < len(p.events); i++ {
		ev := p.events[i]
		if ev.cycle > endCycle {
			break
		}
		if ev.cycle > pos {
			p.mono = p.synth.Feed(int(ev.cycle-pos), p.level, p.mono)
			pos = ev.cycle
		}
		p.level = ev.level
	}
	p.mono = p.synth.Feed(int(endCycle-pos), p.level, p.mono)

	// Anything at or after endCycle belongs to the next frame. Nothing is
	// normally left over, since events are only recorded while the CPU runs and
	// the CPU has stopped at endCycle, but dropping an event would silently
	// shift the audio, so they are carried forward explicitly.
	p.events = append(p.events[:0], p.events[i:]...)
	p.lastCycle = endCycle

	if len(p.mono) == 0 {
		return nil
	}
	return p.sink.WriteSamples(p.stereoize())
}

// stereoize applies the volume and duplicates the mono signal into the two
// channels the device expects.
func (p *Player) stereoize() []float32 {
	p.stereo = p.stereo[:0]
	for _, s := range p.mono {
		v := float32(clamp(float64(s)*p.volume, -1, 1))
		p.stereo = append(p.stereo, v, v)
	}
	return p.stereo
}

// Skip throws away the emulated time up to endCycle without producing any
// samples.
//
// It is for a stretch of emulated time that is not being played in real time,
// such as the emulator winding a tape past at full speed. Handing that audio to
// the sound card, which plays in real time, would only hold the emulator back;
// skipping it keeps the audio timeline lined up with the emulated one, so
// playing carries on in the right place rather than in a burst of catch-up.
func (p *Player) Skip(endCycle uint64) {
	if endCycle <= p.lastCycle {
		return
	}
	p.events = p.events[:0]
	p.lastCycle = endCycle
}

// Close releases the sink.
func (p *Player) Close() error {
	if p.sink == nil {
		return nil
	}
	return p.sink.Close()
}

// levelValue maps the state of the pin onto the signal the synthesiser
// integrates.
func levelValue(high bool) float64 {
	if high {
		return 1
	}
	return -1
}

// clamp limits v to the range lo..hi.
func clamp(v, lo, hi float64) float64 {
	return math.Min(math.Max(v, lo), hi)
}

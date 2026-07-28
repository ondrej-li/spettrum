// Package beeper provides ZX Spectrum 1-bit audio emulation.
// Converts CPU-level port writes to real-time PCM audio output.
package beeper

import (
	"sync"
	"time"
)

const (
	// CPU clock and sample rate
	CPUClock   = 3_500_000 // 3.5 MHz
	SampleRate = 44100     // 44.1 kHz
	Channels   = 2         // stereo

	// Ring buffer size (power of 2)
	ringSize = 4096
	ringMask = ringSize - 1
)

// Event records a state change on the beeper pin at a given CPU cycle.
type Event struct {
	CPUClock uint64
	Beeper   bool
}

// Player manages audio output. The audio output backend (CoreAudio on macOS,
// ALSA/PulseAudio on Linux) is configured via the AudioContext interface.
type Player struct {
	mu       sync.Mutex
	ring     [ringSize]Event
	readIdx  uint32
	writeIdx uint32

	// Volume (0.0 - 1.0)
	Volume float64

	// Rendering state
	renderedCycle uint64
	currentLevel  float64

	// Sample callback — called when audio is needed
	SampleCallback func(samples []float32)

	// Whether audio output is active
	active bool
}

// NewPlayer creates a new beeper audio player.
func NewPlayer(volume float64) *Player {
	return &Player{
		Volume: volume,
	}
}

// Start begins audio output.
func (p *Player) Start() error {
	p.active = true
	return nil
}

// Stop pauses audio output.
func (p *Player) Stop() {
	p.active = false
}

// Close shuts down the player.
func (p *Player) Close() error {
	p.Stop()
	return nil
}

// Update records a beeper state change at the given CPU cycle.
func (p *Player) Update(cpuCycle uint64, beeper bool) {
	p.mu.Lock()
	next := (p.writeIdx + 1) & ringMask
	if next != p.readIdx { // not full
		p.ring[p.writeIdx] = Event{CPUClock: cpuCycle, Beeper: beeper}
		p.writeIdx = next
	}
	p.mu.Unlock()
}

// ReadSamples generates PCM samples. Returns stereo float32 samples.
func (p *Player) ReadSamples(samples int) []float32 {
	p.mu.Lock()
	defer p.mu.Unlock()

	out := make([]float32, samples*Channels)
	cps := float64(CPUClock) / float64(SampleRate)

	// Initialize rendered cycle
	if p.renderedCycle == 0 && p.readIdx != p.writeIdx {
		p.renderedCycle = p.ring[p.readIdx].CPUClock
	}

	for i := 0; i < samples; i++ {
		frameCycle := p.renderedCycle + uint64(float64(i)*cps)

		// Pop events up to frameCycle
		for p.readIdx != p.writeIdx {
			ev := p.ring[p.readIdx]
			if ev.CPUClock > frameCycle {
				break
			}
			if ev.Beeper {
				p.currentLevel = p.Volume
			} else {
				p.currentLevel = -p.Volume
			}
			p.readIdx = (p.readIdx + 1) & ringMask
		}

		// Write stereo sample
		sample := float32(p.currentLevel)
		out[i*2] = sample
		out[i*2+1] = sample
	}

	p.renderedCycle += uint64(float64(samples) * cps)

	if p.SampleCallback != nil {
		p.SampleCallback(out)
	}

	return out
}

// DrainEvents discards all pending events and resets the clock.
func (p *Player) DrainEvents() {
	p.mu.Lock()
	p.readIdx = p.writeIdx
	p.mu.Unlock()
}

// Reset resets the player state.
func (p *Player) Reset() {
	p.mu.Lock()
	p.readIdx = 0
	p.writeIdx = 0
	p.renderedCycle = 0
	p.currentLevel = 0
	p.mu.Unlock()
}

// CPUClockToDuration converts CPU cycles to wall-clock time.
func CPUClockToDuration(cycles uint64) time.Duration {
	return time.Duration(float64(cycles) / float64(CPUClock) * float64(time.Second))
}


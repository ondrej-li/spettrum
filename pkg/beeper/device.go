package beeper

import (
	"encoding/binary"
	"errors"
	"io"
	"math"
	"sync"
	"time"

	"github.com/ebitengine/oto/v3"
)

// deviceBytesPerFrame is the size of one interleaved stereo float32 frame.
const deviceBytesPerFrame = Channels * 4

// deviceInitTimeout bounds how long opening the sound card may take. A machine
// with no working output device must not stop the emulator from starting.
const deviceInitTimeout = 3 * time.Second

// roomWaitLimit is how long a write may wait for the device to make room before
// the emulator gives up and carries on without it.
const roomWaitLimit = 100 * time.Millisecond

// maxDeviceStalls is how many writes in a row may time out before waiting is
// abandoned. Waiting is what ties the emulator's speed to the sound card, but a
// device that has stopped consuming completely must not keep slowing the
// emulator down; once it has, writes stop waiting and the audio is dropped until
// the device starts making room again.
const maxDeviceStalls = 3

// DeviceSink plays the audio on the host's sound card.
//
// oto pulls samples from an io.Reader on its own goroutine, so this sink is the
// producer half of a small hand-off buffer:
//
//	emulation thread --WriteSamples--> ring --> Read --> oto --> sound card
//
// The emulator produces a whole frame of audio in one burst every 20ms, and the
// ring plus oto's buffers turn those bursts into a steady stream. What ties the
// emulator's clock to the sound card's is that WriteSamples waits for room: the
// queue fills, and from then on every write completes at the moment the device
// has consumed a frame's worth of audio. That makes the device the clock the
// emulator runs on, so the two cannot drift apart and the queues stay where they
// are instead of draining or overflowing.
//
// The wait is bounded, because a device that stops consuming must not stop the
// emulator: after roomWaitLimit the oldest queued audio is dropped to make room
// for the newest, which keeps what is heard as close to what is on screen as the
// device allows.
type DeviceSink struct {
	ctx    *oto.Context
	player *oto.Player

	mu     sync.Mutex
	cond   *sync.Cond // signals the reader that audio is queued, and close
	ring   []byte
	head   int // read position in ring, always a whole number of frames
	count  int // valid bytes in ring, always a whole number of frames
	closed bool
	drops  uint64
	// stalls counts consecutive writes that timed out waiting for room.
	stalls int

	// roomWait is how long a write waits for room. It is a field only so tests
	// can keep it short.
	roomWait time.Duration
	// roomSignal wakes a writer when the reader has freed space. It is buffered,
	// so a notification is never lost when nobody is waiting.
	roomSignal chan struct{}

	scratch []byte
}

// NewDeviceSink opens the default output device. latency is the buffer each
// stage of the output is given - the queue here, oto's own buffer and the
// device's - so the delay between the machine doing something and the user
// hearing it is roughly three times that. It is kept small for that reason, and
// because a frame of samples arrives in one burst, a few frames is all the
// buffering that pattern needs.
func NewDeviceSink(sampleRate int, latency time.Duration) (*DeviceSink, error) {
	if sampleRate <= 0 {
		sampleRate = DefaultSampleRate
	}
	if latency <= 0 {
		latency = DefaultLatency
	}

	ctx, ready, err := oto.NewContext(&oto.NewContextOptions{
		SampleRate:      sampleRate,
		ChannelCount:    Channels,
		Format:          oto.FormatFloat32LE,
		BufferSize:      latency,
		ApplicationName: "Spettrum",
	})
	if err != nil {
		return nil, err
	}
	// The device opens asynchronously, so a machine whose audio backend never
	// answers is timed out instead of hanging the emulator.
	select {
	case <-ready:
	case <-time.After(deviceInitTimeout):
		return nil, errors.New("beeper: timed out opening the audio device")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	// The ring holds the same amount again as the device: together they are the
	// slack that covers a frame that takes longer than usual.
	bytesPerSecond := sampleRate * deviceBytesPerFrame
	ringBytes := int(latency.Seconds() * float64(bytesPerSecond))
	if minBytes := 2 * bytesPerSecond / 50; ringBytes < minBytes {
		ringBytes = minBytes // two frames, the most one write can carry
	}
	ringBytes = ringBytes / deviceBytesPerFrame * deviceBytesPerFrame

	d := &DeviceSink{
		ctx:        ctx,
		ring:       make([]byte, ringBytes),
		roomWait:   roomWaitLimit,
		roomSignal: make(chan struct{}, 1),
	}
	d.cond = sync.NewCond(&d.mu)
	d.player = ctx.NewPlayer(d)
	// oto buffers half a second per player unless told otherwise, which on its
	// own would put the sound half a second behind the screen. The device buffer
	// passed above and this one are the whole delay, so both are kept small.
	d.player.SetBufferSize(int(latency.Seconds() * float64(bytesPerSecond)))
	// A new player is not played, and therefore not mixed, until asked: without
	// this its buffer fills once and the queue behind it never drains.
	d.player.Play()
	return d, nil
}

// WriteSamples implements Sink.
func (d *DeviceSink) WriteSamples(pcm []float32) error {
	if len(pcm) == 0 {
		return nil
	}
	d.scratch = d.scratch[:0]
	for _, s := range pcm {
		d.scratch = binary.LittleEndian.AppendUint32(d.scratch, math.Float32bits(s))
	}
	// The queue is fed from a local view of the scratch buffer, so the buffer
	// itself keeps starting at the same place for the next call.
	rest := d.scratch

	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		return nil
	}

	// A block larger than the queue cannot be queued whole: keep its tail, the
	// part closest to the present.
	if len(rest) > len(d.ring) {
		rest = rest[len(rest)-len(d.ring):]
	}

	deadline := time.Now().Add(d.roomWait)
	waited := false
	if d.stalls < maxDeviceStalls {
		for {
			rest = d.appendLocked(rest)
			if len(rest) == 0 {
				if !waited {
					// There was room straight away, so the device is keeping up.
					d.stalls = 0
				}
				return nil
			}
			// Waiting has to happen without the lock: the reader needs it to make
			// room.
			d.mu.Unlock()
			roomed := d.waitForRoom(deadline)
			d.mu.Lock()
			if d.closed {
				return nil
			}
			waited = true
			if !roomed {
				d.stalls++
				break
			}
		}
	}

	// The device is not keeping up. Drop the oldest queued audio to make room
	// for the newest rather than falling further behind it.
	rest = d.appendLocked(rest)
	if len(rest) == 0 {
		// Room appeared after all: the device is keeping up again.
		d.stalls = 0
		return nil
	}
	drop := min(len(rest), d.count)
	d.head = (d.head + drop) % len(d.ring)
	d.count -= drop
	d.drops += uint64(drop)
	rest = d.appendLocked(rest)
	d.drops += uint64(len(rest))
	return nil
}

// appendLocked queues as much of scratch as the ring has room for and returns
// what is left. The caller must hold the lock.
func (d *DeviceSink) appendLocked(scratch []byte) []byte {
	room := (len(d.ring) - d.count) / deviceBytesPerFrame * deviceBytesPerFrame
	room = min(room, len(scratch))
	if room == 0 {
		return scratch
	}
	tail := (d.head + d.count) % len(d.ring)
	n := copy(d.ring[tail:], scratch[:room])
	copy(d.ring, scratch[n:room])
	d.count += room
	d.cond.Broadcast()
	return scratch[room:]
}

// waitForRoom blocks until the reader has made room, the deadline passes, or the
// sink is closed. The caller must not hold the lock.
func (d *DeviceSink) waitForRoom(deadline time.Time) bool {
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return false
	}
	timer := time.NewTimer(remaining)
	defer timer.Stop()
	select {
	case <-d.roomSignal:
		return true
	case <-timer.C:
		return false
	}
}

// Read implements io.Reader for oto. It hands over the queued samples and blocks
// while there are none, which is what stops oto from spinning between frames.
func (d *DeviceSink) Read(p []byte) (int, error) {
	// oto always reads whole frames; a short buffer must not be reported as the
	// end of the stream, so it is simply served nothing.
	n := len(p) / deviceBytesPerFrame * deviceBytesPerFrame
	if n == 0 {
		return 0, nil
	}

	d.mu.Lock()
	for d.count == 0 && !d.closed {
		d.cond.Wait()
	}
	if d.count == 0 {
		d.mu.Unlock()
		return 0, io.EOF
	}
	n = min(n, d.count)

	first := copy(p[:n], d.ring[d.head:])
	if first < n {
		copy(p[first:n], d.ring[:n-first])
	}
	d.head = (d.head + n) % len(d.ring)
	d.count -= n
	d.mu.Unlock()

	// Tell a waiting writer there is room again.
	select {
	case d.roomSignal <- struct{}{}:
	default:
	}
	return n, nil
}

// Dropped reports how many bytes of audio were discarded because the device
// could not keep up. Anything other than zero means the output glitched.
func (d *DeviceSink) Dropped() uint64 {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.drops
}

// Paced reports whether the device is keeping time: it does so as long as it is
// consuming the audio it is given. A device that has stopped making room is no
// longer usable as a clock, and the caller has to pace itself some other way.
func (d *DeviceSink) Paced() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return !d.closed && d.stalls < maxDeviceStalls
}

// Close stops the audio output.
func (d *DeviceSink) Close() error {
	d.closeQueue()

	// Order matters: Read has to be released before asking oto to stop reading,
	// since PauseAndStopReading waits for a read in flight to finish and a read
	// of an empty queue is waiting for samples that will never come.
	d.player.PauseAndStopReading()
	return d.ctx.Suspend()
}

// closeQueue marks the queue closed and releases anyone waiting on it. It is
// separate from Close so the queue can be exercised without a sound card.
func (d *DeviceSink) closeQueue() {
	d.mu.Lock()
	d.closed = true
	d.mu.Unlock()
	d.cond.Broadcast()
	select {
	case d.roomSignal <- struct{}{}:
	default:
	}
}

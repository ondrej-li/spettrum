package beeper

import (
	"encoding/binary"
	"io"
	"math"
	"slices"
	"sync"
	"testing"
	"time"
)

// newQueueSink builds a DeviceSink around a queue of the given number of frames.
// The audio device is never opened, so the hand-off buffer can be exercised on
// its own - it is the part that has to survive a host that cannot play audio at
// the rate the emulator produces it.
func newQueueSink(frames int) *DeviceSink {
	d := &DeviceSink{
		ring:       make([]byte, frames*deviceBytesPerFrame),
		roomWait:   time.Millisecond, // keep the tests quick
		roomSignal: make(chan struct{}, 1),
	}
	d.cond = sync.NewCond(&d.mu)
	return d
}

// frameValues builds n stereo frames carrying the values first..first+n-1, so
// the order they come back out in can be checked.
func frameValues(first, n int) []float32 {
	out := make([]float32, 0, n*Channels)
	for i := 0; i < n; i++ {
		v := float32(first + i)
		out = append(out, v, v)
	}
	return out
}

// readFrames pulls up to max frames out of the queue and decodes them.
func readFrames(t *testing.T, d *DeviceSink, max int) []float32 {
	t.Helper()
	buf := make([]byte, max*deviceBytesPerFrame)
	n, err := d.Read(buf)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if n%deviceBytesPerFrame != 0 {
		t.Fatalf("Read returned %d bytes, not a whole number of frames", n)
	}
	out := make([]float32, 0, n/4)
	for i := 0; i+4 <= n; i += 4 {
		out = append(out, math.Float32frombits(binary.LittleEndian.Uint32(buf[i:])))
	}
	return out
}

// expectSamples compares decoded frames against what was written.
func expectSamples(t *testing.T, got, want []float32) {
	t.Helper()
	if !slices.Equal(got, want) {
		t.Fatalf("read %v, want %v", got, want)
	}
}

// TestDeviceSinkQueueWrapAround writes past the end of the ring and checks the
// samples still come out in the order they went in.
func TestDeviceSinkQueueWrapAround(t *testing.T) {
	d := newQueueSink(4)

	if err := d.WriteSamples(frameValues(0, 3)); err != nil { // frames 0,1,2
		t.Fatalf("WriteSamples: %v", err)
	}
	expectSamples(t, readFrames(t, d, 2), frameValues(0, 2))
	// The ring now holds frame 2 at its very end, so this write wraps.
	if err := d.WriteSamples(frameValues(10, 2)); err != nil { // frames 10,11
		t.Fatalf("WriteSamples: %v", err)
	}
	want := append(frameValues(2, 1), frameValues(10, 2)...)
	expectSamples(t, readFrames(t, d, 8), want)
	if d.Dropped() != 0 {
		t.Errorf("%d bytes were dropped, want none", d.Dropped())
	}
}

// TestDeviceSinkDropsOldestWhenFull checks the policy for a device that does not
// keep up: what is dropped is the oldest audio, so what is heard stays in step
// with what is on screen instead of falling further and further behind it. The
// short roomWait set up by newQueueSink is what makes the write give up quickly.
func TestDeviceSinkDropsOldestWhenFull(t *testing.T) {
	d := newQueueSink(4)

	if err := d.WriteSamples(frameValues(0, 2)); err != nil { // frames 0,1
		t.Fatalf("WriteSamples: %v", err)
	}
	// Four more frames into a queue that holds four: the two oldest go.
	if err := d.WriteSamples(frameValues(2, 4)); err != nil { // frames 2,3,4,5
		t.Fatalf("WriteSamples: %v", err)
	}

	expectSamples(t, readFrames(t, d, 4), frameValues(2, 4))
	if d.Dropped() == 0 {
		t.Error("audio was dropped but Dropped reports none")
	}
}

// TestDeviceSinkWaitsForRoom checks the pacing that keeps the emulator in step
// with a sound card running on its own crystal: a write that has nowhere to go
// waits for the reader rather than dropping audio.
func TestDeviceSinkWaitsForRoom(t *testing.T) {
	d := newQueueSink(2)
	d.roomWait = 500 * time.Millisecond

	if err := d.WriteSamples(frameValues(0, 2)); err != nil { // fills the queue
		t.Fatalf("WriteSamples: %v", err)
	}

	done := make(chan error, 1)
	go func() { done <- d.WriteSamples(frameValues(100, 1)) }()

	// With the queue full, the write must not finish (or drop anything).
	select {
	case err := <-done:
		t.Fatalf("a full queue accepted a write instead of waiting (%v)", err)
	case <-time.After(5 * time.Millisecond):
	}
	if d.Dropped() != 0 {
		t.Fatalf("audio was dropped although a reader was about to make room")
	}

	// Freeing a frame lets the waiting write through, in order and intact.
	expectSamples(t, readFrames(t, d, 1), frameValues(0, 1))
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("WriteSamples: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("the write did not resume once there was room")
	}
	expectSamples(t, readFrames(t, d, 1), frameValues(1, 1))
	expectSamples(t, readFrames(t, d, 1), frameValues(100, 1))
	if d.Dropped() != 0 {
		t.Errorf("dropped %d bytes, want none", d.Dropped())
	}
}

// TestDeviceSinkTrimsOversizedBlocks checks a single block larger than the queue
// is trimmed to what fits rather than being rejected outright.
func TestDeviceSinkTrimsOversizedBlocks(t *testing.T) {
	d := newQueueSink(2)

	if err := d.WriteSamples(frameValues(0, 8)); err != nil { // frames 0..7
		t.Fatalf("WriteSamples: %v", err)
	}
	// The tail of the block is kept: it is the part closest to the present.
	expectSamples(t, readFrames(t, d, 2), frameValues(6, 2))
}

// TestDeviceSinkReadBlocksThenEndsOnClose checks the two states oto depends on:
// a read of an empty queue waits for audio, and it is released with EOF when the
// sink is closed. Without that release, shutting the emulator down would wait
// for ever on the audio thread.
func TestDeviceSinkReadBlocksThenEndsOnClose(t *testing.T) {
	d := newQueueSink(4)

	type result struct {
		n   int
		err error
	}
	done := make(chan result, 1)
	go func() {
		n, err := d.Read(make([]byte, deviceBytesPerFrame))
		done <- result{n, err}
	}()

	select {
	case r := <-done:
		t.Fatalf("an empty queue returned (%d, %v), want a blocking read", r.n, r.err)
	case <-time.After(20 * time.Millisecond):
	}

	d.closeQueue()
	select {
	case r := <-done:
		if r.err != io.EOF {
			t.Errorf("read of a closed queue returned error %v, want EOF", r.err)
		}
	case <-time.After(time.Second):
		t.Fatal("closing the sink did not release the blocked read")
	}
}

// TestDeviceSinkStopsWaitingOnAStalledDevice checks the fallback for a device
// that stops consuming: once a few writes have timed out, the emulator stops
// waiting for it rather than being dragged down to its speed. It also checks the
// other half of that, which is that waiting resumes if the device comes back.
func TestDeviceSinkStopsWaitingOnAStalledDevice(t *testing.T) {
	d := newQueueSink(2)
	d.roomWait = 20 * time.Millisecond

	// A block bigger than the queue is trimmed, so this fills it without waiting.
	if err := d.WriteSamples(frameValues(0, 4)); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	// Nobody is reading, so these writes each give up after roomWait.
	for i := 0; i < maxDeviceStalls; i++ {
		if err := d.WriteSamples(frameValues(100, 1)); err != nil {
			t.Fatalf("WriteSamples: %v", err)
		}
	}
	dropped := d.Dropped()
	if dropped == 0 {
		t.Fatal("a queue nobody reads dropped nothing")
	}

	// The next write must not pause the emulator for the full roomWait again.
	start := time.Now()
	if err := d.WriteSamples(frameValues(200, 1)); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if elapsed := time.Since(start); elapsed > d.roomWait/2 {
		t.Errorf("a stalled device held the emulator for %v", elapsed)
	}

	// If the device starts keeping up again, the queue drains and the next write
	// needs no dropping at all.
	dropped = d.Dropped()
	readFrames(t, d, 2)
	if err := d.WriteSamples(frameValues(300, 1)); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if d.Dropped() != dropped {
		t.Errorf("dropped %d more bytes after the device caught up", d.Dropped()-dropped)
	}
	expectSamples(t, readFrames(t, d, 1), frameValues(300, 1))
}

// TestDeviceSinkReadReturnsQueuedAudio checks a read of a non-empty queue hands
// over what is there instead of waiting for more.
func TestDeviceSinkReadReturnsQueuedAudio(t *testing.T) {
	d := newQueueSink(4)
	if err := d.WriteSamples(frameValues(0, 1)); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	// Asking for more frames than are queued must not turn into a wait.
	expectSamples(t, readFrames(t, d, 8), frameValues(0, 1))
}

// TestDeviceSinkReadsWholeFramesOnly checks a part-frame buffer is not mistaken
// for the end of the stream.
func TestDeviceSinkReadsWholeFramesOnly(t *testing.T) {
	d := newQueueSink(4)
	if err := d.WriteSamples(frameValues(0, 1)); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if n, err := d.Read(make([]byte, deviceBytesPerFrame-1)); err != nil || n != 0 {
		t.Errorf("Read of a part frame returned (%d, %v), want (0, nil)", n, err)
	}
}

// TestDeviceSinkConcurrentAccess exercises the sink the way it is used: the
// emulation thread writing whole frames while oto's goroutine reads them out.
// Run with -race it also covers the locking around the ring.
func TestDeviceSinkConcurrentAccess(t *testing.T) {
	const (
		writes = 200
		frames = 10
	)
	d := newQueueSink(4)

	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		buf := make([]byte, 3*deviceBytesPerFrame)
		for {
			_, err := d.Read(buf)
			if err == io.EOF {
				return
			}
			if err != nil {
				t.Errorf("Read: %v", err)
				return
			}
		}
	}()

	for i := 0; i < writes; i++ {
		if err := d.WriteSamples(frameValues(i*frames, frames)); err != nil {
			t.Fatalf("WriteSamples: %v", err)
		}
	}
	d.closeQueue()

	select {
	case <-readerDone:
	case <-time.After(time.Second):
		t.Fatal("the reader did not stop after the sink was closed")
	}
}

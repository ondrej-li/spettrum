package beeper

import (
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// TestWavSinkRoundTrip writes samples and reads them back, which is how the
// end-to-end audio test inspects a capture.
func TestWavSinkRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "roundtrip.wav")
	const rate = 22050

	w, err := NewWavSink(path, rate)
	if err != nil {
		t.Fatalf("NewWavSink: %v", err)
	}
	// Three stereo frames.
	want := []float32{0, 0, 0.5, -0.5, 1, -1}
	if err := w.WriteSamples(want); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	got, gotRate, channels, err := ReadWav(path)
	if err != nil {
		t.Fatalf("ReadWav: %v", err)
	}
	if gotRate != rate {
		t.Errorf("sample rate %d, want %d", gotRate, rate)
	}
	if channels != Channels {
		t.Errorf("%d channels, want %d", channels, Channels)
	}
	if len(got) != len(want) {
		t.Fatalf("read %d samples, wrote %d", len(got), len(want))
	}
	for i := range want {
		if math.Abs(float64(got[i]-want[i])) > 1.0/32767 {
			t.Errorf("sample %d is %v, want %v", i, got[i], want[i])
		}
	}
}

// TestWavSinkHeaderRecordsSizes checks the sizes patched in by Close: a player
// that trusts the header would otherwise see an empty file.
func TestWavSinkHeaderRecordsSizes(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sizes.wav")
	w, err := NewWavSink(path, DefaultSampleRate)
	if err != nil {
		t.Fatalf("NewWavSink: %v", err)
	}
	const frames = 1000
	pcm := make([]float32, frames*Channels)
	if err := w.WriteSamples(pcm); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if len(data) != wavHeaderSize+frames*Channels*2 {
		t.Fatalf("file is %d bytes, want %d", len(data), wavHeaderSize+frames*Channels*2)
	}
	if got := binary.LittleEndian.Uint32(data[4:]); int(got) != len(data)-8 {
		t.Errorf("RIFF size is %d, want %d", got, len(data)-8)
	}
	if got := binary.LittleEndian.Uint32(data[40:]); int(got) != len(data)-wavHeaderSize {
		t.Errorf("data size is %d, want %d", got, len(data)-wavHeaderSize)
	}
	// The header must describe 16-bit stereo PCM at the requested rate.
	if got := binary.LittleEndian.Uint16(data[22:]); int(got) != Channels {
		t.Errorf("header says %d channels, want %d", got, Channels)
	}
	if got := binary.LittleEndian.Uint16(data[34:]); got != 16 {
		t.Errorf("header says %d bits per sample, want 16", got)
	}
}

// TestWavSinkIsIdempotentOnClose checks a second Close is harmless. The emulator
// closes the file explicitly and again when it shuts down.
func TestWavSinkIsIdempotentOnClose(t *testing.T) {
	path := filepath.Join(t.TempDir(), "twice.wav")
	w, err := NewWavSink(path, DefaultSampleRate)
	if err != nil {
		t.Fatalf("NewWavSink: %v", err)
	}
	if err := w.WriteSamples([]float32{0.5, 0.5}); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("first Close: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("second Close: %v", err)
	}
}

// TestReadWavRejectsRubbish checks a non-WAV file is reported rather than
// half-read, so a mistyped path does not look like silence.
func TestReadWavRejectsRubbish(t *testing.T) {
	path := filepath.Join(t.TempDir(), "not-a-wav.txt")
	if err := os.WriteFile(path, []byte("this is not audio"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	if _, _, _, err := ReadWav(path); err == nil {
		t.Error("ReadWav accepted a file that is not a WAV")
	}
}

// TestMultiSinkFansOut checks that playing and capturing at the same time gives
// both sinks the same audio.
func TestMultiSinkFansOut(t *testing.T) {
	first, second := &captureSink{}, &captureSink{}
	m := MultiSink{first, second}
	pcm := []float32{0.1, 0.1, -0.2, -0.2}
	if err := m.WriteSamples(pcm); err != nil {
		t.Fatalf("WriteSamples: %v", err)
	}
	if err := m.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	for i, s := range []*captureSink{first, second} {
		if len(s.samples) != len(pcm) {
			t.Fatalf("sink %d got %d samples, want %d", i, len(s.samples), len(pcm))
		}
		if !s.closed {
			t.Errorf("sink %d was not closed", i)
		}
	}
}

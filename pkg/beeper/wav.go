package beeper

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
)

const (
	// wavHeaderSize is the size of the canonical RIFF/WAVE header written in
	// front of the sample data.
	wavHeaderSize = 44
	// wavMaxData is the largest data chunk a RIFF file can describe, since the
	// chunk sizes are 32-bit. At 44.1kHz stereo it is about six hours of audio.
	wavMaxData = 0xFFFFFFFF - 36
	// wavFlushSize is how much audio is buffered before it is written out.
	wavFlushSize = 64 << 10
)

// WavSink writes the audio to a 16-bit stereo PCM WAV file.
//
// This is the dependency-free way to hear - or inspect - what the emulator
// generates: it needs no sound card, so it also works on headless machines and
// in tests, and two runs of the same program produce byte-identical files.
type WavSink struct {
	f          *os.File
	sampleRate int
	dataBytes  uint64
	buf        []byte
	err        error
}

// NewWavSink creates (or truncates) path and writes the WAV header. The header's
// sizes are only known once the file is complete, so they are filled in by
// Close.
func NewWavSink(path string, sampleRate int) (*WavSink, error) {
	if sampleRate <= 0 {
		sampleRate = DefaultSampleRate
	}
	f, err := os.Create(path)
	if err != nil {
		return nil, err
	}
	h := wavHeader(sampleRate, 0)
	if _, err := f.Write(h[:]); err != nil {
		f.Close()
		return nil, err
	}
	return &WavSink{f: f, sampleRate: sampleRate}, nil
}

// WriteSamples implements Sink.
func (w *WavSink) WriteSamples(pcm []float32) error {
	if w.err != nil {
		return w.err
	}
	if w.f == nil {
		return errors.New("beeper: WAV sink is closed")
	}
	for _, s := range pcm {
		if w.dataBytes+2 > wavMaxData {
			w.err = fmt.Errorf("beeper: %s would exceed the 4GB limit of a WAV file", w.f.Name())
			return w.err
		}
		v := int16(math.Round(clamp(float64(s), -1, 1) * 32767))
		w.buf = binary.LittleEndian.AppendUint16(w.buf, uint16(v))
		w.dataBytes += 2
	}
	if len(w.buf) >= wavFlushSize {
		w.err = w.flush()
	}
	return w.err
}

// Close flushes the remaining audio, patches the header sizes and closes the
// file.
func (w *WavSink) Close() error {
	if w.f == nil {
		return nil
	}
	err := w.err
	if err == nil {
		err = w.flush()
	}
	if _, seekErr := w.f.Seek(0, io.SeekStart); seekErr != nil && err == nil {
		err = seekErr
	}
	h := wavHeader(w.sampleRate, w.dataBytes)
	if _, writeErr := w.f.Write(h[:]); writeErr != nil && err == nil {
		err = writeErr
	}
	if closeErr := w.f.Close(); closeErr != nil && err == nil {
		err = closeErr
	}
	w.f = nil
	return err
}

// flush writes out the buffered samples.
func (w *WavSink) flush() error {
	if len(w.buf) == 0 {
		return nil
	}
	_, err := w.f.Write(w.buf)
	w.buf = w.buf[:0]
	return err
}

// wavHeader builds the 44-byte RIFF/WAVE header for 16-bit stereo PCM.
func wavHeader(sampleRate int, dataBytes uint64) [wavHeaderSize]byte {
	const (
		channels  = Channels
		bits      = 16
		blockSize = channels * bits / 8
	)
	var h [wavHeaderSize]byte
	copy(h[0:], "RIFF")
	binary.LittleEndian.PutUint32(h[4:], uint32(36+dataBytes))
	copy(h[8:], "WAVE")
	copy(h[12:], "fmt ")
	binary.LittleEndian.PutUint32(h[16:], 16) // fmt chunk size
	binary.LittleEndian.PutUint16(h[20:], 1)  // PCM
	binary.LittleEndian.PutUint16(h[22:], channels)
	binary.LittleEndian.PutUint32(h[24:], uint32(sampleRate))
	binary.LittleEndian.PutUint32(h[28:], uint32(sampleRate*blockSize)) // byte rate
	binary.LittleEndian.PutUint16(h[32:], blockSize)
	binary.LittleEndian.PutUint16(h[34:], bits)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], uint32(dataBytes))
	return h
}

// ReadWav reads a 16-bit PCM WAV file back into interleaved float32 samples.
//
// It exists so a capture can be checked without external tools: the tests use it
// to verify that what the emulator wrote is what the beeper pin did.
func ReadWav(path string) (samples []float32, sampleRate, channels int, err error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, 0, 0, err
	}
	if len(data) < 12 || string(data[0:4]) != "RIFF" || string(data[8:12]) != "WAVE" {
		return nil, 0, 0, errors.New("beeper: not a RIFF/WAVE file")
	}

	var bits int
	var pcm []byte
	for off := 12; off+8 <= len(data); {
		id := string(data[off : off+4])
		size := int(binary.LittleEndian.Uint32(data[off+4:]))
		body := data[off+8:]
		if size > len(body) {
			return nil, 0, 0, fmt.Errorf("beeper: truncated %q chunk", id)
		}
		switch id {
		case "fmt ":
			if len(body) < 16 {
				return nil, 0, 0, errors.New("beeper: truncated fmt chunk")
			}
			if format := binary.LittleEndian.Uint16(body[0:]); format != 1 {
				return nil, 0, 0, fmt.Errorf("beeper: unsupported WAV format %d", format)
			}
			channels = int(binary.LittleEndian.Uint16(body[2:]))
			sampleRate = int(binary.LittleEndian.Uint32(body[4:]))
			bits = int(binary.LittleEndian.Uint16(body[14:]))
		case "data":
			pcm = body[:size]
		}
		off += 8 + size + size%2 // chunks are word aligned
	}

	switch {
	case pcm == nil:
		return nil, 0, 0, errors.New("beeper: no data chunk")
	case bits != 16:
		return nil, 0, 0, fmt.Errorf("beeper: unsupported sample width %d", bits)
	case channels < 1:
		return nil, 0, 0, errors.New("beeper: no channels")
	}

	samples = make([]float32, 0, len(pcm)/2)
	for i := 0; i+1 < len(pcm); i += 2 {
		v := int16(binary.LittleEndian.Uint16(pcm[i:]))
		samples = append(samples, float32(v)/32767)
	}
	return samples, sampleRate, channels, nil
}

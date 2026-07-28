// Package romdata embeds the default ZX Spectrum 48K ROM into the binary.
package romdata

import _ "embed"

// DefaultROM contains the 16KB ZX Spectrum 48K ROM image.
//
//go:embed ZX_Spectrum_48k.rom
var DefaultROM []byte

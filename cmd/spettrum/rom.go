package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// defaultROMName is the image that release archives and packages install next to
// the binary. It is OpenSE BASIC, a freely licensed replacement for the 48K ROM;
// the Amstrad image is never bundled.
const defaultROMName = "opense.rom"

// findROM resolves the ROM image to load. An explicit path wins, then
// SPETTRUM_ROM, then the places the bundled image is normally installed in.
func findROM(flagValue string) (string, error) {
	if flagValue != "" {
		return flagValue, nil
	}
	if env := os.Getenv("SPETTRUM_ROM"); env != "" {
		return env, nil
	}
	exe, _ := os.Executable()
	cwd, _ := os.Getwd()
	home, _ := os.UserHomeDir()
	for _, dir := range romDirs(exe, cwd, home) {
		path := filepath.Join(dir, defaultROMName)
		if info, err := os.Stat(path); err == nil && !info.IsDir() {
			return path, nil
		}
	}
	return "", fmt.Errorf("no %s found", defaultROMName)
}

// romDirs lists the directories searched for the bundled ROM, most specific
// first. It covers the release archive layout (share/spettrum beside the
// binary), the package layout (../share/spettrum for /usr/bin), a source
// checkout, and the usual system and user data directories.
func romDirs(exe, cwd, home string) []string {
	dirs := []string{}
	if exe != "" {
		exeDir := filepath.Dir(exe)
		dirs = append(dirs,
			exeDir,
			filepath.Join(exeDir, "share", "spettrum"),
			filepath.Join(exeDir, "..", "share", "spettrum"),
			filepath.Join(exeDir, "..", "third_party", "opense-basic"),
		)
	}
	if cwd != "" {
		dirs = append(dirs,
			cwd,
			filepath.Join(cwd, "rom"),
			filepath.Join(cwd, "third_party", "opense-basic"),
		)
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		dirs = append(dirs, filepath.Join(xdg, "spettrum"))
	}
	if home != "" {
		dirs = append(dirs, filepath.Join(home, ".local", "share", "spettrum"))
	}
	return append(dirs,
		filepath.Join("/usr", "local", "share", "spettrum"),
		filepath.Join("/usr", "share", "spettrum"),
	)
}

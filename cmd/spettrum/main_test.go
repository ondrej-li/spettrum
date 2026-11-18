package main

import "testing"

func TestBuildVersion(t *testing.T) {
	t.Cleanup(func() { version = "" })

	version = ""
	if got := buildVersion(); got != defaultVersion {
		t.Fatalf("buildVersion() without stamping = %q, want %q", got, defaultVersion)
	}

	version = "v1.2.3"
	if got := buildVersion(); got != "1.2.3" {
		t.Fatalf("buildVersion() with stamping = %q, want %q", got, "1.2.3")
	}
}

func TestFindROM(t *testing.T) {
	if got, err := findROM("/explicit/rom.bin"); err != nil || got != "/explicit/rom.bin" {
		t.Fatalf("findROM(explicit) = %q, %v", got, err)
	}

	t.Setenv("SPETTRUM_ROM", "/from/env.rom")
	if got, err := findROM(""); err != nil || got != "/from/env.rom" {
		t.Fatalf("findROM(SPETTRUM_ROM) = %q, %v", got, err)
	}
}

func TestROMDirs(t *testing.T) {
	t.Setenv("XDG_DATA_HOME", "")
	dirs := romDirs("/opt/spettrum/bin/spettrum", "/src/spettrum", "/home/user")
	want := []string{
		"/opt/spettrum/bin",
		"/opt/spettrum/bin/share/spettrum",
		"/opt/spettrum/share/spettrum",
		"/opt/spettrum/third_party/opense-basic",
		"/src/spettrum",
		"/src/spettrum/rom",
		"/src/spettrum/third_party/opense-basic",
		"/home/user/.local/share/spettrum",
		"/usr/local/share/spettrum",
		"/usr/share/spettrum",
	}
	if len(dirs) != len(want) {
		t.Fatalf("romDirs returned %d entries, want %d: %v", len(dirs), len(want), dirs)
	}
	for i := range want {
		if dirs[i] != want[i] {
			t.Errorf("romDirs[%d] = %q, want %q", i, dirs[i], want[i])
		}
	}

	t.Setenv("XDG_DATA_HOME", "/xdg")
	dirs = romDirs("", "", "/home/user")
	if got := dirs[0]; got != "/xdg/spettrum" {
		t.Errorf("XDG dir = %q, want %q", got, "/xdg/spettrum")
	}
}

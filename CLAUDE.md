# CLAUDE.md — spettrum

**Read [`.github/copilot-instructions.md`](.github/copilot-instructions.md)** —
it is 289 lines of project-specific rules (including "never create files unless
asked") and it is the authority for how this repository is meant to be worked on.
This file is the map around it.

## What this is

A high-performance Z80 CPU emulator for the ZX Spectrum with a terminal renderer:
ULA output drawn as Unicode blocks/braille at 50 Hz, ROM loading, and debugging
tools (state inspection, disassembly logs, anomaly detection).

Two trees live here:

| Tree | What | Where |
|---|---|---|
| **Go** (primary) | the emulator: `cmd/spettrum`, `internal/emulator`, `pkg/{z80,ula,tap,tzx,snapshot,beeper,keyboard}` | `Makefile.golang` |
| **C** (legacy) | the original implementation kept for reference: `main.c`, `z80.c`, `ula.c`, `tap.c`, … | `Makefile` |

Go code is where changes belong. Touch the C tree only when the task is
explicitly about it.

## Build and test

CI (`.github/workflows/c.yml`, `ubuntu-latest`) runs two jobs on every push and PR:

- **c-build** — `make clean && make` (the C tree must keep compiling)
- **go-build** — `make -f Makefile.golang vet build test race`, i.e. `go vet`,
  `go build`, `go test`, `go test -race`

```sh
go build -o bin/spettrum ./cmd/spettrum
go test ./... && go test -race ./...
```

Everything is pure Go plus a C tree that compiles with gcc; there is no macOS-only
piece and no signed build. It builds on Linux, which is where CI runs it.

Releases: `.goreleaser.yaml` and a release workflow exist on the
`wip/2026-11-release-and-opense` branch but are **not on `main`** — treat
`main` as "build and test only" until that branch is merged.

## Deploy

There is no deploy: the binary is the deliverable, and no release pipeline is on
`main`. If you need one, that is a change worth its own PR rather than a side
effect.

## Conventions

- `make -f Makefile.golang` is the entry point for everything Go; do not add a
  parallel script.
- Tests exist per package and are the definition of working: `go test ./...` and
  `go test -race ./...` both pass before a change is called done.
- The C tree is a reference implementation. When Go behaviour is ambiguous, the C
  file is the specification — read it, do not edit it to match the Go code.
- Follow `.github/copilot-instructions.md` on file creation and on how to report
  findings; it is stricter than the usual repository.

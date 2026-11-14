# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- `make package` runs `govulncheck ./...` first (`make vuln`) and stops on a
  finding, so a release cannot be built past one.

## [0.1.0] - 2026-09-30

### Added

- `timeout [-fpv] [-k DURATION] [-s SIGNAL] DURATION COMMAND [ARG]...`, following
  POSIX.1-2024 with the GNU long options (`--foreground`, `--kill-after`,
  `--preserve-status`, `--signal`, `--verbose`, `--help`, `--version`).
- Descendants timed out through a process group of its own; SIGCONT after the
  signal; every terminating signal forwarded; the command's signal death
  mimicked without a core file; inherited dispositions and mask passed on.
- Time asleep is not counted.
- `-s 0` and `-s EXIT` name the null signal, as GNU accepts them.
- `-v` reports every signal sent to the command, forwarded ones included.

### Internal

- Project scaffold: Makefile with the five C safety layers and a positive
  control for each, e2e test harness, signing and Homebrew templates.
- `make test-gnu`: the same cases run against this `timeout` and GNU
  coreutils `timeout` 9.7 in a Linux container; only the differences listed in
  ADR-0003 are allowed.
- Specification ([RFP](docs/en/timeout-rfp.md)) and architecture decision
  records ([ADR-0001 to ADR-0005](docs/en/adr/)).

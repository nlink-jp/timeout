# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [0.1.0] - Unreleased

### Added

- `timeout [-fpv] [-k DURATION] [-s SIGNAL] DURATION COMMAND [ARG]...`, following
  POSIX.1-2024 with the GNU long options (`--foreground`, `--kill-after`,
  `--preserve-status`, `--signal`, `--verbose`, `--help`, `--version`).
- Descendants timed out through a process group of its own; SIGCONT after the
  signal; every terminating signal forwarded; the command's signal death
  mimicked without a core file; inherited dispositions and mask passed on.
- Time asleep is not counted.

### Internal

- Project scaffold: Makefile with the five C safety layers and a positive
  control for each, e2e test harness, signing and Homebrew templates.
- Specification ([RFP](docs/en/timeout-rfp.md)) and architecture decision
  records ([ADR-0001 to ADR-0005](docs/en/adr/)).

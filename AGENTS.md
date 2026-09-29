# AGENTS.md — timeout

## Summary

`timeout` for macOS: runs a command with a time limit, following POSIX.1-2024
XCU `timeout` with the GNU coreutils options (`--foreground`, `--kill-after`,
`--preserve-status`, `--signal`, `--verbose`). Written in C (libc only),
because Go cannot preserve inherited signal dispositions, terminate itself by
the child's signal, or forward every signal (ADR-0001). Ships darwin/arm64
only, minimum macOS 12. Time spent asleep is not counted (as GNU on Linux).
The specification is `docs/en/timeout-rfp.md` (ja: `docs/ja/timeout-rfp.ja.md`).

The e2e tests are Go (standard library only); `go.mod` exists for them alone:
module `github.com/nlink-jp/timeout`.

## Build & Test

```bash
make build      # dist/timeout — shipped flags, signed if a Developer ID is present
make build-test # dist/test/timeout — ASan + UBSan + bounds safety + pattern init
make lint       # clang --analyze; any finding fails
make controls   # proves each safety layer fails on a planted bug
make test       # controls + lint + e2e against dist/test/timeout AND dist/timeout
make package    # dist/timeout-<version>-darwin-arm64.zip, notarized
make verify-release
```

## Structure

```
timeout/
├── src/main.c              # options, the fork, the kqueue main loop, exit status
├── src/sigstate.c          # dispositions, the three signal groups, the counting handler, self-termination
├── src/duration.c          # DURATION parsing (pure)
├── src/signames.c          # signal names <-> numbers (pure)
├── tests/unit/             # unit tests + input stress for the pure modules and self-termination
├── tests/helpers/helper.c  # the child the e2e tests run: prints dispositions, ignores/blocks/catches signals
├── e2e/                    # Go tests that drive the built binary (TIMEOUT_BIN)
├── tests/controls/         # one planted bug per safety layer + run.sh
├── experiments/adr-0002/   # signal-handling experiments (probe.c, run.sh) and their raw logs
├── scripts/                # vendored org templates (signing, notarize, brew) — do not edit
├── docs/{en,ja}/           # RFP and ADRs
└── Makefile
```

## Gotchas

- **Deliberate departures from the org Go scaffold**: the build is clang, not
  `go build`; there is no `main.go`/cobra; `PLATFORMS` is darwin/arm64 only
  (no linux/windows builds — Linux has GNU timeout). The Makefile keeps the
  org's `dist/` output, signing, `package`, `verify-release` and brew targets.
- **`go test ./...` alone fails on purpose**: `TIMEOUT_BIN` must name the
  binary under test; `make test` sets it twice (test build, shipped build).
- **ASan hides inherited dispositions**: it installs SEGV/BUS/FPE handlers
  before `main`. The test build runs with
  `ASAN_OPTIONS=handle_segv=0:handle_sigbus=0:handle_sigfpe=0`, and every e2e
  test also runs against the shipped build.
- **`clang --analyze -Werror` does not fail on findings**; the Makefile uses
  `-Xanalyzer -analyzer-werror`. **UBSan does not fail** without
  `-fno-sanitize-recover=all`. The controls prove both.
- **`-fbounds-safety` annotations**: `main` takes
  `char *__null_terminated *__counted_by(argc) argv`. The one call to `execvp`
  needs `__unsafe_forge_null_terminated` (ADR-0001).
- **Signals sent to a child right after fork are lost if the parent ignores
  them**, and **ignoring a synchronous signal makes a real fault spin**
  (measured) — the reasons for ADR-0002's three groups.
- **macOS gives a SEGV sent with `kill` the same siginfo as a real NULL
  dereference** (`si_code=2`, `si_pid=0`, `si_addr=0x0`). The synchronous
  handler therefore counts arrivals per main-loop epoch and resets at the 8th
  (ADR-0002 revision 3); the main loop must start a new epoch after every
  `kevent` return and re-arm the handler after forwarding.
- **macOS kills a stopped process with a default-action TERM by itself**, so a
  stopped `sleep` proves nothing about SIGCONT; the e2e test uses a command that
  catches TERM, which stays pending until SIGCONT (mutation-checked).
- An e2e command that leaves a grandchild holding stdout makes Go's `Wait`
  block until the grandchild exits; use `exec` in `sh -c` where a test times
  the result.
- `experiments/adr-0002/run.sh` takes minutes (thousands of forks); run it in
  the background. Its logs are timestamped and never overwritten.
- Whether a core file was written cannot be observed from outside on this Mac
  (`/cores` is root-only, and even a control shows no core flag), so core
  suppression must be checked by a unit test, not an e2e test.

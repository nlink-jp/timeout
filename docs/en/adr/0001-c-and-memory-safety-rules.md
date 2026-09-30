# ADR-0001: Write timeout in C, under enforced memory-safety rules

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | The independent verification of RFP revision 1 showed Go cannot meet the standard; the operator chose C on the condition that memory bounds and automatic variables are handled with full care |

## Context

POSIX.1-2024 XCU `timeout` requires three things a Go program cannot do
(measured on go1.27.1 / macOS 27, 2026-09-29):

1. **Preserve inherited dispositions.** The Go runtime installs its own
   handler at start-up for signals inherited as ignored, except HUP and INT, so
   `signal.Ignored(SIGTERM)` is false under `trap '' TERM`, and a child started
   with `os/exec` gets TERM and PIPE at the default action.
2. **Terminate itself with the child's signal.** After `signal.Reset`, a
   self-kill with QUIT exits 2 (goroutine dump) and PIPE and USR1 are survived.
3. **Forward every terminating signal.** SEGV, BUS and ILL crash the runtime;
   PIPE and PROF never reach `signal.Notify`.

The standard, GNU and FreeBSD are all written against C's fork, exec and
sigaction, where each requirement is direct. C also gives GNU's duration
parsing (`strtod`) and GNU's 126/127 and PATH semantics (`execvp`) for free.

C's cost is memory safety. The operator's condition: memory bounds and
automatic variables must be handled with full care. Measured with Apple clang
21 against planted bugs, no single safety mechanism catches everything:

| Planted bug | Warnings | Static analysis | ASan | `-fbounds-safety` |
|---|---|---|---|---|
| Automatic variable uninitialized on one path | caught | caught | — | — |
| Returning the address of an automatic variable | caught | caught | — | — |
| memcpy past the end of an array | missed | missed | caught | — |
| Index 3 into a 3-element array | — | — | — | trapped (a normal build silently reads) |

## Decision

Write the utility in C against libc only, and hold it to the rules below,
each enforced by a machine check.

### Coding rules

1. **Automatic variables** are initialized at their declaration (structs with
   `= {0}`). Their address never leaves the function: not returned, not stored
   in a static or in a struct that outlives the call. No VLAs, no `alloca`. An
   array on the stack is fixed-length and at most **1 KiB**.
2. **Memory bounds**: user input is never copied into a fixed-length buffer —
   argv is used through its pointers. No `strcpy`, `strcat`, `sprintf`, `gets`.
   Formatted output goes through `snprintf`, whose return value is checked and
   whose truncation is handled. Every array index is range-checked before use;
   lengths are `sizeof` or `size_t` constants.
3. **No dynamic allocation.** A future need is decided in a new ADR.
4. **Numeric conversion**: `isfinite` and the range are checked before a
   `double` becomes an integer or a `timespec` (an out-of-range conversion is
   undefined behaviour). Additions of durations are checked for overflow and
   clamped. No implicit conversions among `int`, `pid_t` and `size_t`.
5. **The forked child** only restores signal dispositions and the mask,
   closes the kqueue, calls `execvp`, and `_exit`s. Its error report is
   `write(2)` of fixed text, the command name from argv and `strerror`'s
   constant text (the process is single-threaded); no `printf` family and no
   stdio. (Amended 2026-09-30 to state what the code does, after the release
   review.)
6. **Signal handlers** exist only for the five synchronous signals (ADR-0002),
   call nothing but `sigaction`, and touch only their own automatic variables
   and the signal module's `volatile sig_atomic_t` arrival counters, indexed
   only after a range check. (Amended 2026-09-29 with ADR-0002 revision 3,
   which needs the counters; originally: automatic variables only.)

### Enforcement: five layers

| Layer | Content | Where |
|---|---|---|
| 1. Warnings | `-std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized -Wsometimes-uninitialized -Wreturn-stack-address` | every build |
| 2. Static analysis | `clang --analyze -Xanalyzer -analyzer-werror` | `make lint` |
| 3. Bounds checking | `-fbounds-safety` | shipped and test builds |
| 4. Run-time detection | shipped: `-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong`; test: ASan + UBSan with `-fno-sanitize-recover=all`, `-ftrivial-auto-var-init=pattern` | `make build` / `make build-test` |
| 5. Input stress | generated inputs (boundary values, very long strings, invalid bytes) through the duration and signal-name parsers under the test build | `make test` |

Each layer has a **positive control** — a file with a planted bug that the
layer must reject, with the diagnostic text checked — run first by
`make test` (`tests/controls/run.sh`). The e2e tests run against both the
test build and the shipped build.

### The one sanctioned bounds exception

Under `-fbounds-safety`, `main` takes
`char *__null_terminated *__counted_by(argc) argv`. The NULL at `argv[argc]`
lies outside the counted range, so the single call that hands the command's
argument vector to `execvp` uses `__unsafe_forge_null_terminated`. No other
`__unsafe_*` construct is allowed in the utility. The unit tests have one:
the input-stress generator passes its buffer to the parsers with
`__unsafe_null_terminated_from_indexable(buf, &buf[len])`, naming the NUL it
just wrote. The e2e helper (`tests/helpers/helper.c`) has the same `execvp`
exception as the utility.

### Writing under `-fbounds-safety` (measured 2026-09-29)

Accepted without any `__unsafe_*` construct:

- assigning `SIG_IGN`, `SIG_DFL` or a handler directly to `sa_handler`;
- passing string literals and `argv` elements to `const char *` parameters;
- `strcmp` on `argv` elements;
- `snprintf` into a local array, then `write(fd, buf, (size_t)n)` with the
  checked count.

Rejected, and therefore avoided (each would need an `__unsafe_*` conversion):

- passing a local `char` array to a `const char *` parameter, which bounds
  safety treats as null-terminated — pass the array with its length as
  `const char *__counted_by(len)` instead;
- passing `SIG_IGN`/`SIG_DFL` through a function-pointer parameter — pass a
  selector (an enum) and assign inside;
- returning a counted buffer as `const char *` — write into the caller's
  buffer and return nothing.

## Consequences

- Every requirement of the standard is implementable directly.
- The project departs from the organization's Go scaffold: the build is
  clang, there is no `main.go` or cobra, and Go is used only for the e2e tests.
  AGENTS.md records this.
- A bug caught by any layer fails `make test`; a layer that stops catching
  fails its control. Measured on the scaffold: removing
  `-fno-sanitize-recover=all` makes the UBSan control fail.
- Two layers do not fail by default and would have passed silently: static
  analysis with `-Werror` alone, and UBSan without `-fno-sanitize-recover`.
- Apple's SDK treats `_FORTIFY_SOURCE` above 2 as 2, so 2 is written.
- Apple clang has no libFuzzer, so layer 5 is our own generator.

## Alternatives considered

- **Go (the organization standard).** Rejected: the three measured failures
  above.
- **Go with cgo.** A constructor running before the runtime can record
  inherited dispositions, and a C helper can terminate the process by signal.
  Forwarding SEGV/BUS/ILL still collides with the runtime's own handlers.
  Rejected as fighting the runtime.
- **Swift.** No `fork` (marked unavailable); `posix_spawn` would have to
  reproduce `execvp`'s shebang-less fallback and 126/127 split by hand.
  Rejected.
- **C with fewer layers.** The measurement shows each layer misses something
  another catches. Rejected.

## References

- [RFP](../timeout-rfp.md) §3
- POSIX.1-2024 XCU `timeout`: https://pubs.opengroup.org/onlinepubs/9799919799/utilities/timeout.html
- `tests/controls/`

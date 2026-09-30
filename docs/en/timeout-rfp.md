# RFP: timeout

> Generated: 2026-09-29
> Status: Accepted (revision 3, approved 2026-09-29; corrected during implementation — see the ADRs)

## 1. Problem Statement

macOS has no `timeout` command for running a command under a time limit (measured
on macOS 27.0: neither `timeout` nor `gtimeout` exists). Scripts written for Linux,
and the `timeout N cmd` that coding agents write by reflex, fail with
`command not found` (exit status 127), and that failure gets misread as the
command's own failure (2026-09-23: a `--version` check across 56 binaries nearly
reported every one as failing). The `perl -e 'alarm N; exec @ARGV'` substitute has
three measured defects — a command that ignores SIGALRM is never stopped,
grandchildren are left behind as orphans, and the exit status is 142, which cannot
be told apart from other failures. The intended user is anyone running shell
scripts or coding agents on this Mac. The tool provides a command with the same
name and behaviour as the POSIX.1-2024 and GNU coreutils `timeout`, so existing
scripts run unchanged.

## 2. Functional Specification

### Commands / API Surface

```
timeout [-fpv] [-k duration] [-s signal] duration command [argument...]
timeout --help | --version
```

| Short | Long (GNU-compatible) | Meaning |
|-------|-----------------------|---------|
| `-f` | `--foreground` | Time out the command itself only. No process group is created, so the command can read the terminal from a script and receives the terminal's Ctrl-C. Descendants are not timed out; SIGCONT is not sent |
| `-p` | `--preserve-status` | On timeout, return the command's status instead of 124 |
| `-k duration` | `--kill-after=duration` | Send SIGKILL if the command is still alive this long after the first signal — the timeout's or a forwarded one, even with a duration of 0 (as GNU). No effect if the `-k` duration is 0 |
| `-s signal` | `--signal=signal` | Signal to send on timeout (default TERM). Names are case-insensitive, `SIG` prefix optional; numbers 0–31 are accepted (macOS numbers; 0 and `EXIT` are the null signal, as GNU) |
| `-v` | `--verbose` | Report the signal sent on timeout to standard error |
| — | `--help` / `--version` | Usage (stdout, exit 0) / version (stdout, exit 0) |

**Option syntax**: parsed with libc `getopt_long` given GNU's own optstring
`"+fk:ps:v"`. The leading `+` stops parsing at the first operand (`timeout 5 ls -l`
passes `-l` to `ls`; without `+` the option is stolen — measured). Accepts `--` as
terminator, attached arguments (`-k5`, `-sKILL`), long options with `=` or a
separate argument (`--kill-after=5` / `--kill-after 5`) and unambiguous
abbreviations (`--pres`); an ambiguous abbreviation (`--ver`) is an error
(measured with macOS libc; same as GNU). Invalid usage prints the reason to
standard error and exits 125.

**Duration**: read with `strtod` in the C locale and multiplied by the optional
unit `s`/`m`/`h`/`d` (seconds when absent). The forms `strtod` accepts (fractions,
exponents, hexadecimal `0x10`=16, `0x1d`=29 seconds, `0x1p0`) match GNU. The
decimal point is always `.` (the program never calls `setlocale`). `0` and `-0`
disable the limit (signal propagation still happens — as GNU). Negative values,
NaN and trailing garbage exit 125. Edge values follow GNU (`dtimespec_bound`): a
tiny positive value, and an underflow where `strtod` returns 0 with `ERANGE`
(`1e-400`), are rounded up to the smallest unit (never read as "0 = no limit"
from the value alone). An overflow (`1e400`) and infinity are clamped to the
largest value the timer accepts (the limit is set in an ADR; kqueue rejects
`INT64_MAX` with `NOTE_SECONDS` as `ERANGE` and accepts 1e10 seconds — measured).

**Behaviour (per POSIX.1-2024 XCU `timeout`)**

- Without `-f`: `setpgid(0, 0)` makes timeout a process group leader before the
  child starts (a failure because it is already a session leader is ignored, as
  GNU does). On timeout the signal goes **to the child's pid directly** and then
  **to the whole group** (so the child itself is reached even if it changed its
  group, as GNU does). What timeout sends to itself is not relayed.
- After any signal other than SIGKILL, SIGCONT is sent the same way (the standard
  requires it for a stopped child and allows it otherwise; GNU always sends it,
  and so do we. Not with `-f`).
- **Forwarding**: when timeout itself receives one of the following signals, it
  sends it at once to the same recipients as on timeout. This counts as the first
  signal and starts the `-k` clock.
  - every signal whose default action in macOS `signal(3)` is terminate or core,
    except SIGKILL and SIGSTOP: HUP INT QUIT ILL TRAP ABRT EMT FPE BUS SEGV SYS
    PIPE ALRM TERM XCPU XFSZ VTALRM PROF USR1 USR2
  - the signal given with `-s` (even if its default action is not to terminate —
    the standard's ASYNCHRONOUS EVENTS)
  - a signal timeout **inherited as ignored** is not forwarded (the child inherits
    it as ignored too, which is consistent).
- SIGTTIN / SIGTTOU are ignored (as the standard's wording says).
- SIGCHLD is default in the parent (inherited as ignored, `waitpid` fails with
  `ECHILD` and the exit status is lost — measured). The child restores the
  inherited disposition.
- **Inherited dispositions**: all signal dispositions and the mask are recorded
  right after start-up, and the child restores exactly that record before exec.
  The sole exception is the timeout signal, which is default in the child.

**Exit status**

| Situation | Exit status |
|-----------|-------------|
| Timed out (no `-p`; the command ended before any `-k` SIGKILL) | 124 |
| Timed out and `-k` sent SIGKILL (also with `-f`) | terminates itself with SIGKILL (137 in the shell; with `-f`, GNU does `exit(137)` — a known difference) |
| Timed out with `-s KILL` (no `-f`) | dies from the group-wide SIGKILL itself (137) |
| Timed out with `-f -s KILL` (no `-k`, no `-p`) | 124 (per the standard; GNU gives 137 — a known difference) |
| timeout's own error (bad arguments, fork failure, …) | 125 |
| Command found but not executable | 126 |
| Command not found | 127 |
| Command exited | its exit status |
| Command killed by a signal (not a timeout, or with `-p`) | terminates itself with the same signal; `RLIMIT_CORE` is set to 0 so no core is written |

126 versus 127 follows the result of `execvp` (`ENOENT` → 127, anything else →
126). PATH search, and running an executable without a shebang through
`/bin/sh`, also follow `execvp` (as GNU; measured on macOS).

### Input / Output

- Standard input and output go straight to the command; timeout never reads or
  writes them. File descriptors 3 and above pass to the child unchanged.
- Standard error carries only timeout's own diagnostics (errors, `-v` reports).
  The `-v` format is `timeout: sending signal TERM to command 'sleep'`, always
  with ASCII `'` quotes (GNU uses ‘ ’ in some locales — a known difference).

### Configuration

None (no config file, no environment variables; only `PATH` for locating the
command).

### External Dependencies

- Runtime: none (no external services, no credentials). libc only.
- Build: Apple clang from Xcode. No third-party libraries or tools.
- Tests: the Go standard library (tests that drive the built binary). Only the
  differential tests use podman and GNU coreutils on a Linux image (as the test
  oracle only; never shipped).

## 3. Design Decisions

### Order of precedence for the specification

1. **POSIX.1-2024** (IEEE Std 1003.1-2024 XCU `timeout`) is normative.
2. Where the standard says implementation-defined or unspecified, follow
   **GNU coreutils** (the 9.12 manual and source).
3. **Where GNU contradicts the standard, follow the standard.** Known differences:
   - With `-p`, a timed-out child killed by a signal: GNU exits 128+n; we
     terminate ourselves with the same signal.
   - SIGTTIN/SIGTTOU in the child: GNU always resets them to default; we keep
     the inherited disposition.
   - With `-f`, after the `-k` SIGKILL: GNU does `exit(137)`; we terminate
     ourselves with SIGKILL (choosing the standard's "as if `-p`"; the shell sees
     137 either way).
   - `-f -s KILL` (no `-k`): GNU gives 137; we give 124 (per the standard's EXIT
     STATUS).
   - The `-v` quotes (above).

  The table of differences lives in an ADR, and the differential tests treat it
  as the list of expected differences.

### Name

`timeout` (command and repository). Any other name would not help existing
scripts or agents' habits. Neither macOS nor Homebrew (core or the tap) has
anything of that name (checked 2026-09-29).

### C, not Go (changed from revision 1)

The independent verification and a reproduction (2026-09-29) established that Go
cannot meet three requirements of the standard:

- At start-up the Go runtime installs its own handler for signals inherited as
  ignored, except HUP/INT. So `signal.Ignored` returns false and the child gets
  the default disposition (under `trap '' TERM`, TERM and PIPE were DFL in the
  child).
- Killing itself after `signal.Reset`, QUIT gave exit 2 and PIPE and USR1 were
  survived (it cannot terminate itself with the same signal).
- SEGV/BUS/ILL crash the runtime and PIPE/PROF never reach Notify (not every
  signal can be forwarded).

cgo patches some of this, but forwarding collides with the runtime. The standard,
GNU and FreeBSD all assume C's fork/exec/sigaction, and in C all three
requirements are direct. Parsing durations with `strtod`, and 126/127 and PATH
search through `execvp`, also match GNU for free. The only dependency is libc.
The deviations from the organization's Go scaffold (clang for the build, Go for
the outer tests) are recorded in AGENTS.md.

### How signals are received

Every signal is received through kqueue `EVFILT_SIGNAL` (which records delivery
attempts even while a signal is ignored or blocked — `kqueue(2)`; measured for all
22 signals on 2026-09-29). The child's exit is watched with
`EVFILT_PROC`/`NOTE_EXIT` and the timeout with `EVFILT_TIMER` (`EV_ONESHOT`).
Signals fall into three groups.

| Group | Setting in the parent | Why |
|---|---|---|
| The forwarding set (except the five synchronous ones below), the `-s` signal, SIGCHLD | **default, blocked** | If ignored, a signal sent to the child between fork and the restoration of its dispositions is discarded by XNU at generation (292 of 300 lost — measured). Blocked with the default action, it stays pending in the child and is delivered once the child has restored its dispositions and unblocked |
| The five that can arise synchronously (SEGV BUS ILL TRAP FPE) | a minimal counting handler | Ignored or blocked, a bug in timeout itself (a NULL dereference, a `-fbounds-safety` trap = SIGTRAP) makes it spin, never timing out or exiting (measured). macOS gives a signal sent with `kill` the same siginfo as a real fault (measured), so the handler counts arrivals per main-loop epoch and restores the default action only at the 8th within one epoch: a real fault re-executes at once and dies; a sent signal lets the main loop run, which forwards it and re-arms the handler (ADR-0002 revision 3). The handler calls nothing but `sigaction` |
| SIGTTIN / SIGTTOU | ignored | required by the standard |

- The child, still blocked, restores the inherited dispositions, then restores
  the inherited mask, then calls `execvp`.
- What timeout sends itself (`kill(0, sig)`, or to its own pid) is already
  recorded in kqueue when kill returns (measured). The record's `data` is a
  count, so subtracting the number timeout sent separates what arrived from
  outside at the same moment.
- Just before terminating itself, timeout discards its own pending signals by
  setting them to ignored, then sets only the chosen signal to default, unblocks
  it and sends it (so no other pending signal kills it first).
- If registering the child's exit watch fails with `ESRCH`, the child is taken as
  already exited and `waitpid` follows (happens with children that end at once —
  measured).
- The program stays single-threaded, which satisfies the restrictions on what a
  forked child may call.
- This approach is proven by experiment at the start of Phase 1.

### Time asleep is not counted

User decision (2026-09-29; matches GNU on Linux). Grounds:
- GNU uses a relative `timer_create(CLOCK_REALTIME)` timer, and Linux measures
  relative `CLOCK_REALTIME` timers on `CLOCK_MONOTONIC`
  (`kernel/time/hrtimer.c`). Linux `CLOCK_MONOTONIC` stops during suspend, so GNU
  `timeout` on Linux does not count time asleep.
- On macOS the kqueue timer's default clock is used (the `mach_absolute_time`
  family, the same as `clock_gettime(3)`'s `CLOCK_UPTIME_RAW`, which does not
  advance during sleep); `NOTE_MACH_CONTINUOUS_TIME` is not set.
- The wall clock (`CLOCK_REALTIME`) is not used, because clock adjustments make
  it jump.
- The behaviour is confirmed by the real-machine sleep measurement in Phase 1.

(Up to revision 2 this said "time asleep counts = the same as GNU on Linux"; the
grounds were backwards. The second independent verification found it, and the
user decided again.)

### Rules for memory bounds and automatic variables (user instruction 2026-09-29)

The rules are enforced by five layers of mechanical checks. On 2026-09-29 each
layer was measured with Apple clang 21 against code with planted bugs (table
below). No single layer is enough.

| Planted bug | Warnings | Static analysis | ASan | `-fbounds-safety` |
|---|---|---|---|---|
| Automatic variable uninitialized on one path | caught | caught | — | — |
| Returning the address of an automatic variable | caught | caught | — | — |
| memcpy past the end of an array | missed | missed | caught | — |
| Index 3 into a 3-element array | — | — | — | trapped at run time (a normal build silently reads) |

**Coding rules**

- **Automatic variables**: initialize every one at its declaration (structs with
  `= {0}`). Never let an address escape the function (no returning it, no storing
  it in a static or a struct). No VLAs and no `alloca`. Arrays on the stack are
  small and fixed-length only (the limit is set in the ADR).
- **Memory bounds**: never copy user input into a fixed-length buffer (argv is
  used through its pointers). No `strcpy`/`strcat`/`sprintf`/`gets`. Formatted
  output uses `snprintf` with its return value checked and truncation handled.
  Every array index is range-checked before use, and lengths are held as
  `sizeof` or `size_t` constants (e.g. the signal-name table).
- **No dynamic allocation** (if a need appears, the reason goes in an ADR).
- **Numeric conversion**: check `isfinite` and the range before converting a
  `double` to an integer or `timespec` (out-of-range conversion is undefined
  behaviour). Time additions are checked for overflow and clamped to the
  maximum. No implicit conversions among `int`/`pid_t`/`size_t`
  (`-Wconversion -Wsign-conversion`).
- **The forked child** only restores dispositions and the mask, calls `execvp`,
  and `_exit`s. Errors are reported with `write` and fixed text, never through
  `printf`.
- **Signal handlers** (the five synchronous ones only) call nothing but
  `sigaction` and touch nothing but their automatic variables and the
  `volatile sig_atomic_t` arrival counters (ADR-0001 rule 6, amended).

**Mechanical enforcement (five layers)**

| Layer | Content | Where |
|---|---|---|
| 1. Warnings | `-std=c17 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wvla -Wformat=2 -Wimplicit-fallthrough -Wuninitialized -Wsometimes-uninitialized -Wreturn-stack-address` | every build |
| 2. Static analysis | `clang --analyze -Xanalyzer -analyzer-werror` (with `-Werror` alone, findings still give rc=0 — measured) | `make lint` (called from `make test`) |
| 3. Bounds checking | `-fbounds-safety` (usable together with ASan — measured) | both the shipped and the test build |
| 4. Run-time detection | shipped: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2` (Apple's SDK treats any value above 2 as 2). tests: ASan + UBSan with `-fno-sanitize-recover=all` (without it UBSan reports and carries on with rc=0 — measured), `-ftrivial-auto-var-init=pattern` | `make build` / `make test` |
| 5. Input stress | large generated inputs (boundary values, very long strings, invalid bytes) fed to the duration and signal-name parsers under ASan (in place of libFuzzer, which Apple clang lacks) | `make test` |

- **A positive control per layer**: each layer has a small file with a planted
  bug, and `make test` first confirms that the layer really fails on it (a check
  that never fails cannot be known to work).
- **ASan and signals**: ASan installs its own SEGV/BUS/FPE handlers before main,
  so the test build runs with
  `ASAN_OPTIONS=handle_segv=0:handle_sigbus=0:handle_sigfpe=0`. The tests that
  drive the binary run against **both the test build and the shipped build**.
- **What `-fbounds-safety` needs** (proven by experiment in the second
  verification): `int main(int argc, char *__null_terminated *__counted_by(argc)
  argv)`, and `strtod`'s endptr as `char *__null_terminated`. The single call to
  `execvp` needs `__unsafe_forge_null_terminated`, because the NULL at
  `argv[argc]` lies outside the counted range — the one exception to the rules,
  recorded in an ADR.

### Reaching descendants, and residual risks

Descendants are reached through the process group only. macOS has no mechanism
equivalent to the standard's other method (becoming the reaper of orphaned
descendants): the SDK's `usr/include` has no `procctl`,
`PR_SET_CHILD_SUBREAPER` or `PR_SET_PDEATHSIG` (checked 2026-09-29).

Accepted residual risks (recorded with reasons in an ADR before implementation):
1. Descendants that leave the process group themselves (`setsid`, daemonizing)
   are not reached. The standard acknowledges this in its RATIONALE.
2. If timeout itself is SIGKILLed, the child survives (no way to signal a child
   when its parent dies). The GNU manual gives the same warning.
3. Without `-f`, timeout and the command are in a different process group:
   - A kill sent by an outer party (an agent's harness, for example) to the
     original group does not reach them; with duration 0 they stay. Sending to
     timeout's own pid is forwarded — the README says so.
   - A command run from a script that reads the terminal is stopped by SIGTTIN,
     and the terminal's Ctrl-C does not reach it. Same as GNU. The README
     explains when to use `-f`.
4. Eight sends of the same synchronous signal (SEGV BUS ILL TRAP FPE) within
   one main-loop iteration are taken for a real fault, and timeout dies by it
   without forwarding. Measured: no burst of 20 sends at 0–200 µs reached it
   (added with ADR-0002 revision 3).

### Out of scope

Linux / Windows builds (Linux has GNU's), locale-dependent decimal commas and
translated messages, non-GNU extensions. Dropping linux/windows from the
organization's Makefile template (4 platforms, darwin arm64 only) is recorded in
AGENTS.md.

### Complements

No similar tool exists in the organization. It replaces the perl-alarm
workaround used in agent operation.

## 4. Development Plan

### Phase 0: ADRs (written before implementation)

- ADR-0001 Why C, and the rules for memory bounds and automatic variables (the
  rules, the five layers, the `__unsafe_forge_null_terminated` exception)
- ADR-0002 How signals are received (the three groups, kqueue) and how inherited
  dispositions are restored
- ADR-0003 Precedence between POSIX and GNU, and the table of known differences
- ADR-0004 Residual risks 1–4
- ADR-0005 Time (asleep not counted, edge values, the upper limit)

### Phase 1: Core

- **Experiments first** (pass criteria written before running them):
  - Blocked default-action signals are recorded by kqueue, and none sent to the
    child right after fork are lost (0 of 300 with the second verification's
    reproduction).
  - The handler for the five synchronous signals: signals sent from outside are
    forwarded, and a real fault (NULL dereference, `-fbounds-safety` trap) ends
    the process instead of spinning.
  - The self-termination procedure is not pre-empted by another pending signal.
- Duration and signal-name parsing (pure functions) with C unit tests and input
  stress tests. The self-termination preparation (setting `RLIMIT_CORE` to 0 and
  so on) is also unit-tested, because on this Mac whether a core was written
  cannot be observed from outside (`/cores` is root-only, and even the control
  shows no core flag).
- Child spawn, process group, sending to the child's pid and the group, SIGCONT,
  `-k`, `-f`, `-p`, forwarding, SIGTTIN/SIGTTOU, SIGCHLD, restoring dispositions
  and the mask, exit status and self-termination by signal.
- Tests that drive the built binary (Go standard library; a small C helper reads
  the child's dispositions):
  - `make test` builds the C first and passes the binary's location in an
    environment variable. When `go test ./...` runs alone and finds no binary, it
    **fails** rather than skipping.
  - the three measured perl-alarm scenarios (ignoring command, surviving
    grandchild, exit status)
  - 124–127, a script without a shebang, a non-executable file on PATH, a child
    that ends at once
  - inherited ignores (HUP, TERM, QUIT, PIPE, USR1, TTIN, CHLD) reach the child
  - forwarding of every signal in the set and of the `-s` signal, and
    self-termination by each
  - signals sent right after fork are not lost
  - a stopped command, a child that changes its group, `-f`+`-k`, `-s KILL`,
    `-f -s KILL`
  - a kill from outside to the original group (pins residual risk 3)
  - tiny, underflowing, `-0`, NaN, infinite and out-of-range durations
  - time-dependent tests have a set tolerance and are repeated to expose jitter
- **Exit criterion (sleep measurement)**: on this Mac, with the user's hands
  once. Run `timeout 120 sleep 600`, put the machine to sleep, wake it after at
  least 3 minutes, and record that it does not end on waking but ends with 124
  once the awake time adds up to about 120 seconds, with the times. No settings
  are changed and `pmset` is not used.

### Phase 2: Features

- Finishing GNU compatibility: long options and abbreviations, `-v` output,
  signal numbers, `--help` / `--version`.
- **Differential tests against GNU** (approved by the user 2026-09-29), as a
  separate `make test-gnu`: compare exit statuses and behaviour for the same
  inputs against GNU coreutils `timeout` on Linux under podman.
  - It **fails** when podman is absent (never skips) and is always run in the
    release procedure.
  - Signals are compared by name, not number (numbers differ between Linux and
    macOS).
  - The known differences in ADR-0003 are treated as expected.
  - A positive control comes first, to catch defects in the harness itself
    (platform selection, privileges, toolchain).

### Phase 3: Release

- README.md / README.ja.md, CHANGELOG.md, AGENTS.md.
- Developer ID signing + notarization, darwin-arm64 build only, GitHub release.
- Add formula `timeout` to homebrew-tap.
- Add as a util-series submodule; update the util-series README catalog row,
  the org profile and the nlink-web-site catalog; run `check-org.sh`.
- Feed back to knowledge:
  - POSIX.1-2024 defines `timeout`.
  - Clocks: GNU `timeout` on Linux does not count time asleep; macOS
    `CLOCK_MONOTONIC` and `NOTE_MACH_CONTINUOUS_TIME` do, the kqueue default does
    not.
  - Go cannot preserve inherited dispositions, terminate itself by signal or
    forward every signal (measured).
  - Ignoring signals loses those sent to a just-forked child, and ignoring
    synchronous signals makes a fault spin.
  - What each C safety layer catches (measured table), and that `--analyze` and
    UBSan do not fail by default.

Each phase can be reviewed independently. An independent-context verification
pass runs at design confirmation (approval of this RFP) and before release (the
design verification stops after two passes; what remains is settled by the Phase
1 experiments).

## 5. Required API Scopes / Permissions

None.

## 6. Series Placement

Series: util-series
Reason: a general-purpose CLI with no external service, used as a building block
in pipes and scripts.

## 7. External Platform Constraints

No external services. macOS (XNU) constraints:

- No facility to reap orphaned descendants (FreeBSD
  `procctl(PROC_REAP_ACQUIRE)`, Linux `PR_SET_CHILD_SUBREAPER`) → process group
  method only (checked in the SDK headers).
- No parent-death signal (Linux `PR_SET_PDEATHSIG`) → residual risk 2.
- A signal generated while ignored is discarded at generation (measured in a
  just-forked child). Ignoring or blocking the synchronous fault signals makes a
  fault spin (measured).
- Clocks: the kqueue timer's default and `CLOCK_UPTIME_RAW`
  (= `mach_absolute_time`) do not advance during sleep; `CLOCK_MONOTONIC` and
  `NOTE_MACH_CONTINUOUS_TIME` do.
- Apple clang has no libFuzzer (`-fsanitize=fuzzer`). Apple's SDK treats
  `_FORTIFY_SOURCE` as at most 2.

---

## Discussion Log

- **2026-09-29 premise measured**: macOS 27.0 has no `timeout` / `gtimeout`. The
  perl-alarm substitute measured — (A) a command doing `trap "" ALRM` ran its full
  3 s under a 1 s limit, rc=0; (B) with `sh -c 'sleep 30 & wait'` only the parent
  died and `sleep 30` survived with PPID 1; (C) rc=142 on timeout.
- **Specification sources**: confirmed on pubs.opengroup.org that `timeout` is new
  in POSIX.1-2024 (Issue 8). Confirmed the compatible extensions and exit status
  137 in the GNU coreutils 9.12 manual.
- **Alternatives**: Homebrew coreutils (`gtimeout`) has a different name, so it
  does not help existing scripts, and conflicts with the policy against adopting
  community tools. FreeBSD's `timeout.c` depends on the reaper facility and does
  not port to macOS as is.
- **User decisions (2026-09-29)**: name `timeout` / include GNU compatibility /
  run container differential tests against GNU. Time asleep was first "counted",
  later changed (below).
- **Independent verification of revision 1 (2026-09-29)**: 3 high, 7 medium and 6
  low findings plus many gaps. The 3 high findings (Go cannot preserve inherited
  dispositions, terminate itself by signal, or forward every signal) were
  reproduced by the author (HUP/TERM/PIPE ignored in the parent →
  `Ignored(TERM)=false`, TERM/PIPE=DFL in the child; self-kill gave exit 2 for
  QUIT and PIPE/USR1 were survived). Revision 1's "Go can meet the standard" is
  withdrawn.
- **User decision (2026-09-29)**: switch to C, **taking full care over memory
  bounds and automatic variables** — made concrete as the rules and mechanical
  enforcement in §3. The `golang.org/x/sys` approval is therefore no longer
  needed.
- **Triage of revision 1's findings (2026-09-29)**: SIGTTIN/SIGTTOU are ignored
  (as the standard says). Where GNU and the standard conflict, the standard wins
  and a table of differences is kept. SIGCONT is always sent, as GNU (no need to
  detect a stopped child). The differential tests fail rather than skip, and
  compare signals by name. Edge-case durations follow GNU's rules. Residual risk
  3 added. The sleep measurement uses this Mac with the user's help, not a
  borrowed machine. ADRs come before implementation (Phase 0). Negative claims
  now cite the SDK headers. Catalog updates added. All gaps are in the
  specification and plan.
- **Independent verification of revision 2 (2026-09-29)**: 2 high, 6 medium and 6
  low. High 1 (ignoring synchronous signals makes a fault spin) was reproduced by
  the author (SEGV ignored, NULL dereference → did not return until the 3 s
  cut-off). High 2 (signals to a just-forked child lost, 292/300) is consistent
  with ignored signals being discarded at generation.
- **User decision (2026-09-29)**: told that "time asleep counts = the same as GNU
  on Linux" had the grounds backwards (GNU on Linux does not count it), the user
  decided again: **time asleep is not counted**.
- **Triage of revision 2's findings (2026-09-29, approved by the user)**: a
  minimal handler for the five synchronous signals, and the other received
  signals blocked with the default action and taken from kqueue ("no handlers"
  revised). `ESRCH` means already exited. SIGCHLD default in the parent. The
  `-f`/KILL combinations follow the standard and go in the difference table.
  Failing flags for static analysis and UBSan, plus a positive control per layer.
  ASan's SEGV/BUS/FPE handling switched off, and the outer tests also run against
  the shipped build. The `-s` signal is forwarded too. Edge values follow GNU.
  FORTIFY is 2. The layer count and the platform wording corrected. Core-file
  suppression is unit-tested. Timer details and what `-fbounds-safety` needs are
  written down.
- **Design verification stops after two passes (2026-09-29, approved by the
  user)**: the findings got lighter and what remains is settled by experiment, so
  it moves to Phase 0 (ADRs) and the Phase 1 experiments.
- **Phase 1 experiments (2026-09-29)**: the first run passed its criteria, but
  its log showed that macOS gives a SEGV sent with `kill` the same siginfo as a
  real NULL dereference, so the handler's `si_code` test was void and criterion
  3 too weak. Revision 2 (always reset, re-arm in the main loop) failed the
  strengthened criterion: 2 sends 10 µs apart killed timeout once in 200. The
  user chose revision 3 (count arrivals per main-loop epoch, reset at the 8th),
  which passed every criterion (0/1000 at each gap) and survived every burst
  of 20 sends; the remaining case is residual risk 4. ADR-0001 rule 6 amended to
  allow the counters; ADR-0001 also records which idioms `-fbounds-safety`
  accepts.

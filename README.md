# timeout

Run a command with a time limit, on macOS.

macOS ships no `timeout` command, so scripts written for Linux — and the
`timeout 10 cmd` that coding agents write out of habit — fail with
`command not found`. This `timeout` follows the POSIX.1-2024 specification
of the utility, with the GNU coreutils options, so those scripts run unchanged.

## Features

- The POSIX.1-2024 `timeout` options `-f`, `-k`, `-p`, `-s`, and the GNU long
  options `--foreground`, `--kill-after`, `--preserve-status`, `--signal`,
  `--verbose`.
- Times out the command's descendants too (unless `-f`), and wakes a stopped
  command with SIGCONT after the signal.
- Forwards every terminating signal it receives to the command, and ends with
  the command's own signal when the command is killed by one.
- The command inherits exactly the signal dispositions and mask `timeout` was
  started with (only the timeout signal is reset to default), so it still
  ignores SIGHUP under `nohup`.
- Time spent asleep is not counted, as with GNU `timeout` on Linux.
- Written in C against libc only; built with bounds checking and checked by
  static analysis and sanitizers.

## Installation

Build from source (Apple silicon, macOS 12 or later):

```bash
make build            # dist/timeout
cp dist/timeout /usr/local/bin/
```

## Usage

```
timeout [OPTION]... DURATION COMMAND [ARG]...
```

DURATION is a number with an optional unit: `s` seconds (the default), `m`
minutes, `h` hours, `d` days — `10`, `1.5m`, `2h`. `0` means no time limit.

| Option | Meaning |
|--------|---------|
| `-s SIG`, `--signal=SIG` | Signal to send on timeout (default `TERM`); a name such as `INT` or `SIGINT`, or a number |
| `-k DUR`, `--kill-after=DUR` | Also send `KILL` if the command is still running this long after the first signal |
| `-p`, `--preserve-status` | Exit with the command's status even when it timed out |
| `-f`, `--foreground` | Time out the command only, not its descendants; lets it read the terminal |
| `-v`, `--verbose` | Report each signal sent on timeout to standard error |
| `--help`, `--version` | Show usage or version |

Options come before DURATION; everything after the command name belongs to the
command (`timeout 5 ls -l`). Use `--` before a DURATION that starts with `-`.

```bash
timeout 10 make test                 # TERM after 10 seconds
timeout -s INT 5m ./server           # INT after 5 minutes
timeout -k 10 1h ./batch             # TERM after 1 hour, KILL 10 seconds later
timeout -f 30 ./interactive-tool     # the tool can read the terminal
```

### Exit status

| Status | Meaning |
|--------|---------|
| 124 | The command timed out (without `-p`) |
| 125 | `timeout` itself failed (bad arguments, …) |
| 126 | The command was found but could not be run |
| 127 | The command was not found |
| 137 | `KILL` was sent by `-k`, or by `-s KILL` without `-f` |
| other | The command's own exit status; if the command was killed by a signal, `timeout` ends by the same signal |

### When to use `-f`

Without `-f`, `timeout` puts itself and the command in a process group of their
own, so it can stop every descendant. Two things follow:

- A command run from a script that reads the terminal is stopped (SIGTTIN), and
  Ctrl-C from the terminal does not reach it. Use `-f` for interactive commands.
- A kill sent to the process group that started `timeout` (by an agent harness
  or a CI runner, for example) does not reach it. Send the signal to
  `timeout`'s own process ID instead: it forwards the signal to the command.

With `-f`, only the command itself is timed out; its children are not.

### Limits

- A descendant that leaves the process group itself (a daemon, `setsid`) is not
  reached.
- If `timeout` is killed with `KILL`, the command keeps running (macOS has no
  way to take a child down with its parent).
- Eight or more copies of the same `SEGV`, `BUS`, `ILL`, `TRAP` or `FPE` sent to
  `timeout` within an instant are taken for a crash of `timeout` itself, which
  then ends without forwarding them.

### Differences from GNU `timeout`

Where GNU departs from POSIX.1-2024, this `timeout` follows the standard:

- With `-p`, a timed-out command killed by a signal makes `timeout` end by that
  signal (GNU exits 128+n).
- The command keeps SIGTTIN and SIGTTOU as inherited (GNU resets them).
- `-f -s KILL` exits 124 (GNU: 137).
- `-v` always quotes with `'`.

The full table is in [ADR-0003](docs/en/adr/0003-posix-and-gnu-precedence.md).

## Building

Requires Xcode (Apple clang) and Go (for the tests only).

```bash
make build   # dist/timeout
make test    # safety-layer controls, static analysis, unit tests, e2e tests
make test-gnu  # compare outcomes with GNU timeout in a Linux container (podman)
```

## Documentation

- [RFP](docs/en/timeout-rfp.md) — the specification
- [Architecture decision records](docs/en/adr/)

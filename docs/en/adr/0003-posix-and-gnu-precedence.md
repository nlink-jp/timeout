# ADR-0003: POSIX first, GNU second, and the known differences

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | GNU coreutils `timeout` contradicts POSIX.1-2024 in several places; the differential tests need a list of expected differences |

## Context

The tool exists so that scripts written for Linux run unchanged, which argues
for GNU behaviour. But the normative text is POSIX.1-2024 XCU `timeout`, and in
a few places GNU does what the standard's RATIONALE explicitly criticizes. The
differential tests against GNU (Phase 2) would fail on each of those unless
they are listed.

## Decision

1. **POSIX.1-2024 XCU `timeout` is normative.**
2. Where the standard says implementation-defined or unspecified, **follow GNU
   coreutils** (manual and source).
3. Where GNU contradicts the standard, **follow the standard** and list the
   case below. The differential tests treat this table as the expected
   differences; any other difference is a bug.

### Known differences from GNU

| Case | GNU | timeout (this project) | Standard |
|---|---|---|---|
| `-p`, timed out, child killed by signal *n* | exits 128+*n* | terminates itself with *n* | EXIT STATUS: mimic the wait status by self-termination |
| SIGTTIN/SIGTTOU in the child | always reset to default | inherited disposition kept | ASYNCHRONOUS EVENTS and RATIONALE: dispositions inherited unchanged |
| `-f`, and `-k` sent SIGKILL | `exit(137)` | terminates itself with SIGKILL (shell sees 137 either way) | EXIT STATUS: 124 or as if `-p` — "as if `-p`" chosen |
| `-f -s KILL`, no `-k`, no `-p` | 137 | 124 | EXIT STATUS: 124 when `-k` not given |
| `-v` quotes | locale-dependent (‘ ’ under UTF-8) | always ASCII `'` | not specified; no locale handling (RFP §3 Out of scope) |
| `-k` with a duration of 0, and a signal forwarded | `-k` has no effect when the main duration is 0 (manual) | `-k` starts at the forwarded signal | DESCRIPTION: a propagated signal "shall be treated as the first signal" (added 2026-09-29, found while implementing) |

### Unspecified points settled by following GNU

- SIGCONT is sent after every signal except SIGKILL (not with `-f`), not only
  to a stopped child.
- The signal is sent to the child's pid directly and then to the group.
- `setpgid` failing because timeout is already a session leader is ignored.
- Duration syntax and edge values follow GNU's `strtod` and
  `dtimespec_bound` (ADR-0005).
- Option parsing uses GNU's optstring `"+fk:ps:v"` and long options, with
  unambiguous abbreviations.
- `-v` message text: `timeout: sending signal NAME to command 'CMD'`.

## Consequences

- The differential tests compare against GNU and fail on any difference not in
  this table, so drift in either direction is visible.
- Scripts that depend on one of the listed GNU behaviours see the standard's
  behaviour instead. All six cases are corner cases (`-p` with a signal death,
  a script run with TTIN/TTOU ignored, `-f` with SIGKILL, quote characters,
  `-k` with no time limit).

## Alternatives considered

- **GNU everywhere.** Closest to "Linux scripts run unchanged", but it
  reproduces behaviour the standard names as a defect (the `nohup`-style
  disposition loss). Rejected.
- **POSIX only, ignoring GNU.** Leaves every unspecified point to invention and
  drops the long options scripts use. Rejected.

## References

- POSIX.1-2024 XCU `timeout`: https://pubs.opengroup.org/onlinepubs/9799919799/utilities/timeout.html
- GNU coreutils manual, `timeout` invocation: https://www.gnu.org/software/coreutils/manual/html_node/timeout-invocation.html
- GNU coreutils `src/timeout.c`
- [RFP](../timeout-rfp.md) §3 Order of precedence

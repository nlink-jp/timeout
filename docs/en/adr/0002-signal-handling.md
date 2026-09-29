# ADR-0002: Receive signals through kqueue, in three groups

| Field | Value |
|-------|-------|
| Status | **Accepted** (revision 3 — the Phase 1 experiments passed on 2026-09-29) |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | The standard's signal requirements, two defects the second verification measured in RFP revision 2, and one the Phase 1 experiments found in this ADR's first revision |

## Context

The standard requires timeout to forward every terminating signal it receives
(and the `-s` signal), to ignore SIGTTIN and SIGTTOU, to not signal itself when
signalling its process group, and to hand the child exactly the dispositions
it inherited, except the timeout signal, which must be default in the child.

RFP revision 2 proposed setting every received signal to `SIG_IGN` and reading
them from kqueue, with no handlers at all. The second verification measured
two defects in that:

- **Signals to a just-forked child are lost.** The child inherits `SIG_IGN`
  until it restores its dispositions, and XNU discards a signal generated while
  ignored. A TERM sent right after fork was lost 292 times in 300.
- **Ignoring a synchronous signal makes a real fault spin.** With SEGV ignored,
  a NULL dereference re-executes forever at 100% CPU (reproduced by the author:
  it did not return until a 3-second cut-off). The same holds for the SIGTRAP of
  a `-fbounds-safety` trap. timeout would then neither time out nor exit.

The Phase 1 experiments then showed that **macOS reports the same siginfo for a
SEGV sent with `kill` as for a real NULL dereference**: `si_code=2`,
`si_pid=0`, `si_addr=0x0` for both. The first revision of this ADR told the two
apart by `si_code`; that premise is false, so a handler cannot know which it
received.

Also measured: kqueue `EVFILT_SIGNAL` records delivery attempts for all 22
signals tried even while they are ignored or blocked; a signal timeout sends to
itself with `kill(0, sig)` or `kill(getpid(), sig)` is already recorded when
`kill` returns, and the record's `data` is a count; setting a pending signal to
`SIG_IGN` discards it; SIGCHLD inherited as ignored makes `waitpid` fail with
`ECHILD`; registering `EVFILT_PROC` for a child that has already exited fails
with `ESRCH`.

## Decision

At start-up, record every signal's disposition and the signal mask. Then set
the signals in three groups:

| Group | Setting in the parent |
|---|---|
| A. The forwarding set except the five below, the `-s` signal, and SIGCHLD | default action, **blocked** |
| B. SEGV, BUS, ILL, TRAP, FPE (can arise synchronously) | a minimal counting handler |
| C. SIGTTIN, SIGTTOU | ignored |

The forwarding set is every signal whose default action is terminate or core,
except SIGKILL and SIGSTOP: HUP INT QUIT ILL TRAP ABRT EMT FPE BUS SEGV SYS PIPE
ALRM TERM XCPU XFSZ VTALRM PROF USR1 USR2. A signal inherited as ignored is left
ignored and not forwarded (the child inherits it ignored, which is consistent).
SIGCHLD is set to default in the parent whatever it inherited.

- **Receiving.** One kqueue holds `EVFILT_SIGNAL` for groups A and B,
  `EVFILT_PROC`/`NOTE_EXIT` for the child, and `EVFILT_TIMER` (`EV_ONESHOT`)
  for the deadline. The count timeout sent itself is subtracted from each
  signal record; what remains came from outside and is forwarded.
- **Group B handler.** A real fault re-executes at once, so the same signal
  keeps arriving with no progress of the main loop in between; a signal sent
  from outside lets the main loop run. The handler therefore counts arrivals
  of each signal per main-loop **epoch** and returns; only at the **8th**
  arrival within one epoch does it set the action to default and return, so a
  real fault recurs and kills the process. The main loop starts a new epoch
  each time `kevent` returns and re-arms the handler after forwarding. State:
  one `volatile sig_atomic_t` epoch written by the main loop, and per-signal
  `volatile sig_atomic_t` epoch and count arrays written by the handler,
  indexed after a range check. The handler calls only `sigaction`.
- **Fork.** Groups A and B are blocked around `fork`. The child, still blocked,
  restores every recorded disposition (the timeout signal: default), then
  restores the recorded mask, then calls `execvp`. A signal sent in the window
  stays pending in the child and is delivered once unblocked.
- **Child exit.** `ESRCH` from registering `EVFILT_PROC` means the child has
  already exited; go straight to `waitpid`.
- **Self-termination** with signal *n*: set every other pending signal to
  ignored (which discards it), set *n* to default, set `RLIMIT_CORE` to 0,
  unblock *n*, and send it.
- The program stays single-threaded.

### Experiments (Phase 1)

Pass criteria were fixed before each run. Criterion 3 was strengthened after
the first run showed it could pass while the design was wrong.

1. With group A blocked at default in the parent, 0 of 300 TERMs sent to the
   child right after fork are lost.
2. A blocked, default-action signal sent from outside is recorded by kqueue.
3. Group B: a NULL dereference, a `-fbounds-safety` trap, `__builtin_trap` and
   an illegal instruction inside the process end it instead of spinning; each
   of the five signals sent twice from outside is survived; and 2 SEGV sends at
   0, 10, 100 and 1000 µs apart are survived in 1000 of 1000 trials each.
4. Self-termination ends with signal *n* even when other signals are pending.

Every experiment also runs a control — the rejected alternative — which must
show the failure, or the experiment could not tell pass from fail.

### Results

Source: `experiments/adr-0002/probe.c`, run by `run.sh`; raw logs in
`experiments/adr-0002/logs/` (macOS 27.0 26A428, arm64, Apple clang 21.0.0).

| Run (log) | Handler | Outcome |
|---|---|---|
| `20260929T212410` | revision 1: `si_code` decides | criteria passed, but the log showed sent signals arriving with fault codes: the premise was false and criterion 3 (one send each) too weak to see it |
| `20260929T224733` | revision 2: always reset to default, re-arm in the main loop | **FAIL**: 2 sends 10 µs apart killed timeout 1 time in 200; bursts of 20 sends killed it 121/200 (0 µs), 66/200 (5 µs), 6/200 (20 µs) |
| `20260929T230022` | revision 3: count per epoch, reset at 8 | **PASS**, every criterion |

Revision 3 in detail:

- exp1: 0/300 lost (control, ignored in the parent: 295/300 lost).
- exp2: self-sent recorded at once (`data=1`); 3 external sends `data=3`;
  `SIG_IGN` discarded the pending signal.
- exp3: the four real faults ended the process in 5–6 ms (control, SEGV
  ignored: still spinning at 2 s); each of the five signals sent twice was
  survived; 2 SEGV sends at 0/10/100/1000 µs: 0/1000 died at each gap.
- exp3 bursts of 20 SEGV sends (measured, not a criterion): 0/200 died at every
  gap from 0 to 200 µs; the control with the reset at the 1st arrival (revision
  2) died 145/200 at 0 µs, 10/200 at 5 µs, 2/200 at 20 µs.
- exp4: 40/40 cases ended by signal *n*, no core flag (control, unblocking
  everything without discarding: killed by the pending HUP instead).

## Consequences

- No signal is lost in the fork window, and a bug in timeout cannot turn into
  an unkillable spinning process: a real fault costs 8 re-executions, a few
  microseconds, before the process dies.
- Only five signals have a handler; it calls one async-signal-safe function
  and touches only `volatile sig_atomic_t` counters (ADR-0001 rule 6, amended).
- A signal sent from outside kills timeout without being forwarded only if 8
  arrivals of that same synchronous signal land within one main-loop
  iteration. Accepted as residual risk 4 (ADR-0004); no burst tried reached it.
- The standard's "briefly ignore the signal while sending it to the group" is
  replaced by subtracting self-sent counts.

## Alternatives considered

- **Ignore everything, no handlers (RFP revision 2).** Rejected: the two
  measured defects above.
- **Tell sent signals from faults by `si_code` (this ADR, revision 1).**
  Rejected: macOS gives both the same siginfo.
- **Always reset to default and re-arm in the main loop (revision 2).**
  Rejected: a second send in the window before the re-arm kills timeout
  (measured above, including with only two sends).
- **Keep the five at their default action.** Real faults die correctly, but
  every one of the five sent from outside kills timeout without forwarding.
  Rejected.
- **Handlers for every signal writing to a self-pipe (GNU-like).** Works, but
  every handler is async-signal-safety surface, and GNU still resets TTIN/TTOU
  in the child, which the standard criticizes. Rejected.
- **Delay sending to the child until `NOTE_EXEC` is seen.** Avoids the fork
  window for signals timeout sends, but not for signals others send to the
  child's pid. Rejected.

## References

- [RFP](../timeout-rfp.md) §2 Behaviour, §3 How signals are received
- `kqueue(2)`, `sigaction(2)`, `signal(3)` (macOS 27)
- [ADR-0001](0001-c-and-memory-safety-rules.md) rule 6
- [ADR-0004](0004-residual-risks.md) risk 4
- `experiments/adr-0002/`

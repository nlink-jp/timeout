# ADR-0002: Receive signals through kqueue, in three groups

| Field | Value |
|-------|-------|
| Status | **Accepted** — the Phase 1 experiments below must pass before code relies on it |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | The standard's signal requirements, and two defects the second verification measured in RFP revision 2 |

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

Also measured: kqueue `EVFILT_SIGNAL` records delivery attempts for all 22
signals tried even while they are ignored or blocked; a signal timeout sends to
itself with `kill(0, sig)` or `kill(getpid(), sig)` is already recorded when
`kill` returns, and the record's `data` is a count; SIGCHLD inherited as
ignored makes `waitpid` fail with `ECHILD`; registering `EVFILT_PROC` for a
child that has already exited fails with `ESRCH`.

## Decision

At start-up, record every signal's disposition and the signal mask. Then set
the signals in three groups:

| Group | Setting in the parent |
|---|---|
| A. The forwarding set except the five below, the `-s` signal, and SIGCHLD | default action, **blocked** |
| B. SEGV, BUS, ILL, TRAP, FPE (can arise synchronously) | a minimal `SA_SIGINFO` handler |
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
- **Group B handler.** If `si_code` says the signal was sent (`SI_USER`,
  `SI_QUEUE`), return at once — kqueue has recorded it. Otherwise it is a real
  fault in timeout: set the action to default and return, so the fault recurs
  and kills the process. The handler calls only `sigaction`.
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

### Experiments that must pass first (Phase 1)

Pass criteria are fixed here, before the experiments run:

1. With group A blocked at default in the parent, 0 of 300 TERMs sent to the
   child right after fork are lost (the second verification's reproduction).
2. A blocked, default-action signal sent from outside is recorded by kqueue.
3. Group B: a signal sent from outside is forwarded; a NULL dereference and a
   `-fbounds-safety` trap inside timeout end the process instead of spinning.
4. Self-termination ends with signal *n* even when another signal is pending.

If any fails, this ADR is revised before implementation continues; the
fallback for receiving is the self-pipe technique.

## Consequences

- No signal is lost in the fork window, and a bug in timeout cannot turn into
  an unkillable spinning process.
- Only five signals have a handler, and it calls one async-signal-safe
  function.
- The standard's "briefly ignore the signal while sending it to the group" is
  replaced by subtracting self-sent counts, which kqueue makes exact.
- A signal of the same kind arriving from outside in the same instant as a
  self-sent one is still counted correctly, because `data` is a count.

## Alternatives considered

- **Ignore everything, no handlers (RFP revision 2).** Rejected: the two
  measured defects above.
- **Handlers for every signal writing to a self-pipe (GNU-like).** Works, but
  every handler is async-signal-safety surface, and GNU still resets TTIN/TTOU
  in the child, which the standard criticizes. Kept as the fallback.
- **Delay sending to the child until `NOTE_EXEC` is seen.** Avoids the fork
  window for signals timeout sends, but not for signals others send to the
  child's pid. Rejected.

## References

- [RFP](../timeout-rfp.md) §2 Behaviour, §3 How signals are received
- `kqueue(2)`, `sigaction(2)`, `signal(3)` (macOS 27)
- [ADR-0001](0001-c-and-memory-safety-rules.md) rule 6

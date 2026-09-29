# ADR-0004: Accepted residual risks of the process-group method

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Date | 2026-09-29 |
| Binds | timeout |
| Decision makers | nlink-jp maintainers |
| Triggered by | macOS lacks the facilities other implementations use to reach every descendant and to take the child down with them |

## Context

The standard allows two ways to signal a command's descendants: make timeout a
process group leader and signal the group, or become the "reaper" of orphaned
descendants and signal all of them. macOS has no reaper facility: the SDK's
`usr/include` has no `procctl`, no `PR_SET_CHILD_SUBREAPER` and no
`PR_SET_PDEATHSIG` (checked 2026-09-29). So only the process-group method is
available, and nothing makes the child die when timeout dies.

## Decision

Use the process-group method (ADR-0002) and accept the following, each
documented in the README where a user would meet it.

1. **Descendants that leave the group are not reached.** A descendant that
   calls `setsid` or `setpgid` itself (daemons, some job-control programs) does
   not receive the signal. The standard's RATIONALE acknowledges this.
2. **If timeout is SIGKILLed, the child survives.** There is no parent-death
   signal. The GNU manual gives the same warning.
3. **Without `-f`, timeout and the command are in their own process group.**
   - A kill aimed by an outer party (an agent harness, a CI runner) at the
     group timeout was started in does not reach timeout or the command. With a
     duration of 0 they keep running. Sending the signal to timeout's own pid
     works: timeout forwards it.
   - A command run from a non-interactive script that reads the terminal is
     stopped by SIGTTIN, and the terminal's Ctrl-C does not reach it. `-f`
     avoids both, at the cost of not timing out descendants. Same as GNU.

4. **A flood of one synchronous signal can kill timeout unforwarded.** macOS
   gives a SEGV, BUS, ILL, TRAP or FPE sent with `kill` the same siginfo as a
   real fault, so the handler recognises a fault by 8 arrivals within one
   main-loop iteration (ADR-0002). Eight sends of the same one of these five
   landing within one iteration are taken for a fault: timeout dies by that
   signal without forwarding it, and the child is left as in risk 2. Measured:
   0 of 200 bursts of 20 sends died at any gap from 0 to 200 µs. Sending these
   five signals to timeout at all is unusual. (Added 2026-09-29 with ADR-0002
   revision 3.)

Risk 3's first point is pinned by an e2e test, so a change in behaviour is
visible.

## Consequences

- The README explains when to use `-f` and that timeout's own pid is the one
  to signal.
- No attempt is made to emulate a reaper (for example by polling the process
  table), which would be racy and would add surface for no guarantee.

## Alternatives considered

- **Poll for descendants and signal them by pid.** Racy (pid reuse, processes
  started between polls) and still misses daemons that reparent to launchd.
  Rejected.
- **A helper process that watches timeout with `EVFILT_PROC` and kills the
  child if timeout dies.** Covers risk 2 only while the helper lives, and the
  helper itself can be SIGKILLed. Rejected as complexity without a guarantee.

## References

- POSIX.1-2024 XCU `timeout`, RATIONALE
- GNU coreutils manual, `timeout` invocation (`--foreground`, exit status 137)
- [RFP](../timeout-rfp.md) §3 Reaching descendants, and residual risks

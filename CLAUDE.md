# CLAUDE.md — timeout

Organization rules: https://github.com/nlink-jp/.github/blob/main/CONVENTIONS.md
Read AGENTS.md first. The specification is the RFP in `docs/`; the decisions
behind it are the ADRs in `docs/{en,ja}/adr/`.

## Non-negotiable rules

- **The specification is POSIX.1-2024 XCU `timeout`.** Where it says
  implementation-defined or unspecified, follow GNU coreutils; where GNU
  contradicts it, follow the standard and add the case to the table of known
  differences (ADR-0003). Never infer behaviour from what another
  implementation happens to do without checking the standard first.
- **Memory bounds and automatic variables (ADR-0001)** — the operator's
  explicit condition for writing this in C:
  - initialize every automatic variable where it is declared; never let its
    address leave the function; no VLAs, no `alloca`, only small fixed arrays
    on the stack;
  - never copy user input into a fixed buffer; no `strcpy`/`strcat`/
    `sprintf`/`gets`; check every `snprintf` result; range-check every index;
  - no dynamic allocation;
  - check `isfinite` and the range before any `double` → integer conversion;
  - the forked child only restores signal state, calls `execvp` and `_exit`s;
  - signal handlers exist only for the five synchronous signals, call
    nothing but `sigaction`, and touch only their automatic variables and the
    `volatile sig_atomic_t` arrival counters.
- **Never weaken a safety layer to make code compile.** A warning, an analyzer
  finding, a bounds trap or a sanitizer report is a bug in the code. The only
  sanctioned bounds exception is the `execvp` call (ADR-0001).
- **Signal handling follows ADR-0002** (three groups: blocked default +
  kqueue, a counting handler for the synchronous five, ignored TTIN/TTOU). Do
  not set a forwarded signal to `SIG_IGN` in the parent: signals sent to a
  just-forked child are then lost. Do not tell a sent signal from a fault by
  `siginfo`: macOS gives both the same. A change to signal handling reruns
  `experiments/adr-0002/run.sh` and keeps its log.
- **Tests with every change**, run with `make test` (never `go test` alone —
  it fails without `TIMEOUT_BIN` by design). A new check needs a positive
  control that fails without it.
- **Build with `make build`** (outputs to `dist/`); never call `clang` by hand
  for a binary you will run or ship.
- **Docs in sync**: README.md and README.ja.md, the RFP pair, and the ADR
  pairs, in the same commit as the change they describe.

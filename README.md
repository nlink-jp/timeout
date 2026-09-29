# timeout

Run a command with a time limit, on macOS.

macOS ships no `timeout` command, so scripts written for Linux — and the
`timeout 10 cmd` that coding agents write out of habit — fail with
`command not found`. This `timeout` follows the POSIX.1-2024 specification
of the utility, with the GNU coreutils options, so those scripts run unchanged.

## Building

Requires Xcode (Apple clang) and Go (for the tests only).

```bash
make build   # dist/timeout
make test    # safety-layer controls, static analysis, and the e2e tests
```

## Documentation

- [RFP](docs/en/timeout-rfp.md) — the specification
- [Architecture decision records](docs/en/adr/)

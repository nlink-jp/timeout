// Package e2e drives the built timeout binary from outside. The binary is
// never built here: `make test` builds it and passes its path in
// TIMEOUT_BIN, once for the sanitizer build and once for the shipped one.
package e2e

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"syscall"
	"testing"
)

// bin returns the binary under test. A missing TIMEOUT_BIN fails the run
// rather than skipping it: a gate that skips is a gate that passes.
func bin(t *testing.T) string {
	t.Helper()
	p := os.Getenv("TIMEOUT_BIN")
	if p == "" {
		t.Fatal("TIMEOUT_BIN is not set; run the tests with `make test`")
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("TIMEOUT_BIN=%s: %v", p, err)
	}
	return p
}

type result struct {
	stdout, stderr string
	code           int            // exit status; -1 when killed by a signal
	signal         syscall.Signal // the signal that killed it, when code is -1
}

// run executes timeout with args and returns its output and how it ended.
func run(t *testing.T, args ...string) result {
	t.Helper()
	cmd := exec.Command(bin(t), args...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	err := cmd.Run()
	r := result{stdout: out.String(), stderr: errb.String()}
	var ee *exec.ExitError
	switch {
	case err == nil:
	case errors.As(err, &ee):
		r.code = ee.ExitCode()
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			r.signal = ws.Signal()
		}
	default:
		t.Fatalf("running %s: %v", bin(t), err)
	}
	return r
}

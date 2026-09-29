// Package e2e drives the built timeout binary from outside. Nothing is
// built here: `make test` builds the binaries and passes their paths in
// TIMEOUT_BIN (once for the sanitizer build, once for the shipped one) and
// TIMEOUT_HELPER (the child program in tests/helpers).
package e2e

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"
)

// envPath returns the file named by an environment variable. A missing one
// fails the run rather than skipping it: a gate that skips is a gate that
// passes.
func envPath(t *testing.T, name string) string {
	t.Helper()
	p := os.Getenv(name)
	if p == "" {
		t.Fatalf("%s is not set; run the tests with `make test`", name)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("%s=%s: %v", name, p, err)
	}
	return p
}

func bin(t *testing.T) string    { return envPath(t, "TIMEOUT_BIN") }
func helper(t *testing.T) string { return envPath(t, "TIMEOUT_HELPER") }

type result struct {
	stdout, stderr string
	code           int            // exit status; -1 when killed by a signal
	signal         syscall.Signal // the signal that killed it, when code is -1
	elapsed        time.Duration
}

func (r result) String() string {
	if r.code == -1 {
		return "killed by " + r.signal.String() + " after " + r.elapsed.String()
	}
	return "exit " + itoa(r.code) + " after " + r.elapsed.String()
}

func itoa(n int) string {
	if n < 0 {
		return "-" + itoa(-n)
	}
	if n < 10 {
		return string(rune('0' + n))
	}
	return itoa(n/10) + string(rune('0'+n%10))
}

// finish waits for cmd and fills a result.
func finish(t *testing.T, cmd *exec.Cmd, out, errb *bytes.Buffer, start time.Time) result {
	t.Helper()
	err := cmd.Wait()
	r := result{stdout: out.String(), stderr: errb.String(), elapsed: time.Since(start)}
	var ee *exec.ExitError
	switch {
	case err == nil:
	case errors.As(err, &ee):
		r.code = ee.ExitCode()
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			r.signal = ws.Signal()
		}
	default:
		t.Fatalf("waiting for %v: %v", cmd.Args, err)
	}
	return r
}

// command builds an exec.Cmd for argv, where the word "TIMEOUT" and
// "HELPER" stand for the binaries under test.
func command(t *testing.T, argv ...string) *exec.Cmd {
	t.Helper()
	args := make([]string, len(argv))
	for i, a := range argv {
		switch a {
		case "TIMEOUT":
			args[i] = bin(t)
		case "HELPER":
			args[i] = helper(t)
		default:
			args[i] = a
		}
	}
	cmd := exec.Command(args[0], args[1:]...)
	cmd.Env = os.Environ()
	return cmd
}

// run executes timeout with args and returns how it ended.
func run(t *testing.T, args ...string) result {
	t.Helper()
	return runArgv(t, append([]string{"TIMEOUT"}, args...)...)
}

// runArgv executes argv (see command) and returns how it ended.
func runArgv(t *testing.T, argv ...string) result {
	t.Helper()
	cmd := command(t, argv...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	start := time.Now()
	if err := cmd.Start(); err != nil {
		t.Fatalf("starting %v: %v", argv, err)
	}
	return finish(t, cmd, &out, &errb, start)
}

// started is a timeout process running in the background.
type started struct {
	cmd       *exec.Cmd
	out, errb bytes.Buffer
	start     time.Time
}

// startTimeout starts timeout with args and waits until its command has had
// time to start (delay).
func startTimeout(t *testing.T, delay time.Duration, args ...string) *started {
	t.Helper()
	s := &started{cmd: command(t, append([]string{"TIMEOUT"}, args...)...)}
	s.cmd.Stdout, s.cmd.Stderr = &s.out, &s.errb
	s.start = time.Now()
	if err := s.cmd.Start(); err != nil {
		t.Fatalf("starting timeout: %v", err)
	}
	t.Cleanup(func() { _ = s.cmd.Process.Kill() })
	time.Sleep(delay)
	return s
}

func (s *started) signal(t *testing.T, sig syscall.Signal) {
	t.Helper()
	if err := s.cmd.Process.Signal(sig); err != nil {
		t.Fatalf("sending %v: %v", sig, err)
	}
}

func (s *started) wait(t *testing.T) result {
	t.Helper()
	return finish(t, s.cmd, &s.out, &s.errb, s.start)
}

// alive reports whether pid exists.
func alive(pid int) bool {
	return syscall.Kill(pid, 0) == nil
}

// dispositions parses `helper disp` output into sig -> 'D'/'I'/'H' and the mask.
func dispositions(t *testing.T, out string) (map[int]byte, string) {
	t.Helper()
	d := map[int]byte{}
	mask := ""
	for f := range strings.FieldsSeq(out) {
		k, v, ok := strings.Cut(f, "=")
		if !ok {
			t.Fatalf("unexpected disp output %q", out)
		}
		if k == "mask" {
			mask = v
			continue
		}
		n := 0
		for _, c := range k {
			n = n*10 + int(c-'0')
		}
		d[n] = v[0]
	}
	if len(d) != 31 || mask == "" {
		t.Fatalf("unexpected disp output %q", out)
	}
	return d, mask
}

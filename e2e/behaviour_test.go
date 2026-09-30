package e2e

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// The three defects measured in the perl-alarm substitute (RFP §1).

func TestStopsACommandThatIgnoresALRM(t *testing.T) {
	r := run(t, "1", "sh", "-c", `trap "" ALRM; sleep 5`)
	if r.code != 124 || r.elapsed > 3*time.Second {
		t.Fatalf("%v; want exit 124 after about 1s", r)
	}
}

func TestStopsGrandchildren(t *testing.T) {
	pidFile := filepath.Join(t.TempDir(), "gc.pid")
	r := run(t, "1", "sh", "-c", `sleep 30 & echo $! > `+pidFile+`; wait`)
	if r.code != 124 {
		t.Fatalf("%v; want exit 124", r)
	}
	b, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatal(err)
	}
	pid, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	deadline := time.Now().Add(2 * time.Second)
	for alive(pid) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if alive(pid) {
		_ = syscall.Kill(pid, syscall.SIGKILL)
		t.Fatalf("grandchild %d survived the timeout", pid)
	}
}

func TestTimedOutExitsWith124(t *testing.T) {
	r := run(t, "0.3", "sleep", "5")
	if r.code != 124 {
		t.Fatalf("%v; want exit 124", r)
	}
}

// Exit statuses (RFP §2).

func TestExitStatuses(t *testing.T) {
	dir := t.TempDir()
	noShebang := filepath.Join(dir, "noshebang")
	if err := os.WriteFile(noShebang, []byte("exit 3\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	notExec := filepath.Join(dir, "notexec")
	if err := os.WriteFile(notExec, []byte("exit 0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		args []string
		want int
	}{
		{"command exits 0", []string{"5", "true"}, 0},
		{"command's own status", []string{"5", "sh", "-c", "exit 7"}, 7},
		{"not found", []string{"5", "timeout-e2e-no-such-command"}, 127},
		{"not executable", []string{"5", notExec}, 126},
		{"no shebang runs through sh", []string{"5", noShebang}, 3},
		{"bad duration", []string{"x", "true"}, 125},
		{"negative duration", []string{"-1", "true"}, 125},
		{"NaN duration", []string{"nan", "true"}, 125},
		{"bad signal", []string{"-s", "NOPE", "1", "true"}, 125},
		{"bad kill-after", []string{"-k", "x", "1", "true"}, 125},
		{"missing command", []string{"5"}, 125},
		{"missing operands", []string{}, 125},
		{"unknown option", []string{"--no-such-option", "1", "true"}, 125},
		{"ambiguous abbreviation", []string{"--ver", "1", "true"}, 125},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r := run(t, c.args...)
			if r.code != c.want {
				t.Fatalf("%v; want exit %d; stderr=%q", r, c.want, r.stderr)
			}
		})
	}
}

func TestNotExecutableOnPath(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "e2e-notexec"), []byte("exit 0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	cmd := command(t, "TIMEOUT", "5", "e2e-notexec")
	cmd.Env = append(os.Environ(), "PATH="+dir)
	err := cmd.Run()
	if code := cmd.ProcessState.ExitCode(); code != 126 {
		t.Fatalf("exit %d (%v); want 126", code, err)
	}
}

func TestChildSignalDeathIsMimicked(t *testing.T) {
	r := run(t, "5", "sh", "-c", `kill -USR1 $$`)
	if r.code != -1 || r.signal != syscall.SIGUSR1 {
		t.Fatalf("%v; want killed by SIGUSR1", r)
	}
}

func TestPreserveStatus(t *testing.T) {
	r := run(t, "-p", "0.3", "sleep", "5")
	if r.code != -1 || r.signal != syscall.SIGTERM {
		t.Fatalf("%v; want killed by SIGTERM (the child's status)", r)
	}
	r = run(t, "--preserve-status", "0.3", "sh", "-c", `trap "exit 5" TERM; sleep 5 & wait`)
	if r.code != 5 {
		t.Fatalf("%v; want exit 5", r)
	}
}

// Signals and options.

func TestSignalOption(t *testing.T) {
	for _, args := range [][]string{
		{"-s", "INT", "0.3", "sleep", "5"},
		{"-sINT", "0.3", "sleep", "5"},
		{"--signal=int", "0.3", "sleep", "5"},
		{"--signal", "SIGINT", "0.3", "sleep", "5"},
		{"-s", "2", "0.3", "sleep", "5"},
		// the null signal (GNU accepts 0 and EXIT): nothing is delivered,
		// so the command runs out and the run still counts as timed out
		{"-s", "0", "0.3", "sleep", "0.6"},
		{"-s", "sigexit", "0.3", "sleep", "0.6"},
	} {
		r := run(t, args...)
		if r.code != 124 {
			t.Fatalf("%v: %v; want exit 124", args, r)
		}
	}
}

func TestKillAfter(t *testing.T) {
	for _, args := range [][]string{
		// exec: no grandchild keeps the output pipe open after the KILL
		{"-k", "0.3", "0.3", "sh", "-c", `trap "" TERM; exec sleep 5`},
		{"-k0.3", "0.3", "sh", "-c", `trap "" TERM; exec sleep 5`},
		{"--kill-after", "0.3", "0.3", "sh", "-c", `trap "" TERM; exec sleep 5`},
		{"-f", "-k", "0.3", "0.3", "sh", "-c", `trap "" TERM; exec sleep 5`},
	} {
		r := run(t, args...)
		if r.code != -1 || r.signal != syscall.SIGKILL || r.elapsed > 3*time.Second {
			t.Fatalf("%v: %v; want killed by SIGKILL within a second or so", args, r)
		}
	}
}

func TestSignalKill(t *testing.T) {
	r := run(t, "-s", "KILL", "0.3", "sleep", "5")
	if r.code != -1 || r.signal != syscall.SIGKILL {
		t.Fatalf("%v; want killed by SIGKILL (the group signal reaches timeout)", r)
	}
	// With -f only the child is signalled: the standard says 124 (ADR-0003).
	r = run(t, "-f", "-s", "KILL", "0.3", "sleep", "5")
	if r.code != 124 {
		t.Fatalf("%v; want exit 124", r)
	}
}

func TestVerbose(t *testing.T) {
	r := run(t, "-v", "-s", "INT", "0.3", "sleep", "5")
	want := "timeout: sending signal INT to command 'sleep'\n"
	if r.code != 124 || r.stderr != want {
		t.Fatalf("%v, stderr %q; want exit 124 and %q", r, r.stderr, want)
	}
	r = run(t, "--verbose", "-k", "0.3", "0.3", "sh", "-c", `trap "" TERM; sleep 5`)
	want = "timeout: sending signal TERM to command 'sh'\ntimeout: sending signal KILL to command 'sh'\n"
	if r.stderr != want {
		t.Fatalf("stderr %q; want %q", r.stderr, want)
	}
}

func TestOptionsStopAtTheDuration(t *testing.T) {
	r := run(t, "5", "ls", "-l", "/")
	if r.code != 0 {
		t.Fatalf("%v; want exit 0 (-l belongs to ls); stderr=%q", r, r.stderr)
	}
	r = run(t, "--", "5", "true")
	if r.code != 0 {
		t.Fatalf("%v; want exit 0 after --", r)
	}
	r = run(t, "--pres", "5", "true")
	if r.code != 0 {
		t.Fatalf("%v; want an unambiguous abbreviation accepted", r)
	}
}

func TestStoppedCommandIsContinued(t *testing.T) {
	// macOS kills a stopped process with a default-action TERM by itself
	// (measured), so this case alone cannot show that SIGCONT is sent.
	r := run(t, "0.5", "sh", "-c", `kill -STOP $$; sleep 5`)
	if r.code != 124 || r.elapsed > 3*time.Second {
		t.Fatalf("%v; want exit 124 soon after 0.5s", r)
	}
	// A command that catches TERM keeps it pending while stopped: only the
	// SIGCONT that follows lets its handler run. -k bounds the test.
	r = run(t, "-p", "-k", "4", "0.5", "sh", "-c", `trap "exit 5" TERM; kill -STOP $$; sleep 5`)
	if r.code != 5 || r.elapsed > 3*time.Second {
		t.Fatalf("%v; want exit 5 soon after 0.5s (the handler ran after SIGCONT)", r)
	}
}

func TestChildThatLeavesTheGroupIsStillSignalled(t *testing.T) {
	r := run(t, "0.5", "HELPER", "setpgid-sleep", "5")
	if r.code != 124 || r.elapsed > 3*time.Second {
		t.Fatalf("%v; want exit 124 soon after 0.5s (sent to the child's pid)", r)
	}
}

// Durations (ADR-0005).

func TestDurationEdges(t *testing.T) {
	for _, d := range []string{"1e-9", "1e-400", "0x1p-10"} {
		r := run(t, d, "sleep", "5")
		if r.code != 124 || r.elapsed > 2*time.Second {
			t.Fatalf("duration %s: %v; want an immediate timeout", d, r)
		}
	}
	// "--" first: "-0" would otherwise be read as an option, as by GNU.
	for _, d := range []string{"0", "-0", "inf", "1e400"} {
		r := run(t, "--", d, "sh", "-c", "sleep 0.2")
		if r.code != 0 {
			t.Fatalf("duration %s: %v; want no limit", d, r)
		}
	}
}

// Timing is checked repeatedly with a tolerance, never from one run.
func TestTimingTolerance(t *testing.T) {
	for i := range 5 {
		r := run(t, "0.3", "sleep", "5")
		if r.code != 124 || r.elapsed < 300*time.Millisecond || r.elapsed > 1500*time.Millisecond {
			t.Fatalf("run %d: %v; want exit 124 between 0.3s and 1.5s", i, r)
		}
	}
}

package e2e

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// Inherited dispositions reach the command unchanged, except the timeout
// signal, which is default (ADR-0002).
func TestInheritedDispositions(t *testing.T) {
	ignored := []int{1, 3, 13, 15, 20, 21, 30} // HUP QUIT PIPE TERM CHLD TTIN USR1
	list := make([]string, len(ignored))
	for i, s := range ignored {
		list[i] = strconv.Itoa(s)
	}
	r := runArgv(t, "HELPER", "ignore", strings.Join(list, ","), "--", "TIMEOUT", "5", "HELPER", "disp")
	if r.code != 0 {
		t.Fatalf("%v; stderr=%q", r, r.stderr)
	}
	d, mask := dispositions(t, r.stdout)
	for _, s := range ignored {
		want := byte('I')
		if s == 15 {
			want = 'D' // the timeout signal
		}
		if d[s] != want {
			t.Errorf("signal %d: %c, want %c (%s)", s, d[s], want, r.stdout)
		}
	}
	for _, s := range []int{2, 11, 14, 22, 31} { // not ignored on entry: default in the child
		if d[s] != 'D' {
			t.Errorf("signal %d: %c, want D (%s)", s, d[s], r.stdout)
		}
	}
	if mask != "0x00000000" {
		t.Errorf("mask %s, want 0x00000000", mask)
	}

	// With -s HUP, HUP is the one set to default and TERM stays ignored.
	r = runArgv(t, "HELPER", "ignore", "1,15", "--", "TIMEOUT", "-s", "HUP", "5", "HELPER", "disp")
	d, _ = dispositions(t, r.stdout)
	if d[1] != 'D' || d[15] != 'I' {
		t.Errorf("-s HUP: HUP=%c TERM=%c, want D and I (%s)", d[1], d[15], r.stdout)
	}

	// SIGTTIN/SIGTTOU inherited at default stay default (GNU would too), and
	// a synchronous signal inherited as ignored stays ignored.
	r = runArgv(t, "HELPER", "ignore", "11", "--", "TIMEOUT", "5", "HELPER", "disp")
	d, _ = dispositions(t, r.stdout)
	if d[21] != 'D' || d[22] != 'D' || d[11] != 'I' {
		t.Errorf("TTIN=%c TTOU=%c SEGV=%c, want D D I (%s)", d[21], d[22], d[11], r.stdout)
	}
}

func TestInheritedMask(t *testing.T) {
	r := runArgv(t, "HELPER", "block", "31", "--", "TIMEOUT", "5", "HELPER", "disp")
	_, mask := dispositions(t, r.stdout)
	if mask != "0x40000000" {
		t.Fatalf("mask %s, want 0x40000000 (USR2 blocked, as inherited)", mask)
	}
}

// Every signal in the forwarding set reaches the command, and timeout then
// ends by the same signal, as the command did.
func TestForwardsEverySignal(t *testing.T) {
	sigs := []syscall.Signal{
		syscall.SIGHUP, syscall.SIGINT, syscall.SIGQUIT, syscall.SIGILL, syscall.SIGTRAP,
		syscall.SIGABRT, syscall.SIGEMT, syscall.SIGFPE, syscall.SIGBUS, syscall.SIGSEGV,
		syscall.SIGSYS, syscall.SIGPIPE, syscall.SIGALRM, syscall.SIGTERM, syscall.SIGXCPU,
		syscall.SIGXFSZ, syscall.SIGVTALRM, syscall.SIGPROF, syscall.SIGUSR1, syscall.SIGUSR2,
	}
	for _, sig := range sigs {
		t.Run(sig.String(), func(t *testing.T) {
			pidFile := filepath.Join(t.TempDir(), "child.pid")
			s := startTimeout(t, 300*time.Millisecond, "10", "sh", "-c", `echo $$ > `+pidFile+`; exec sleep 10`)
			s.signal(t, sig)
			r := s.wait(t)
			if r.code != -1 || r.signal != sig || r.elapsed > 5*time.Second {
				t.Fatalf("%v; want killed by %v soon", r, sig)
			}
			if pid := readPid(t, pidFile); alive(pid) {
				_ = syscall.Kill(pid, syscall.SIGKILL)
				t.Fatalf("the command (pid %d) survived; timeout did not forward %v", pid, sig)
			}
		})
	}
}

// The -s signal is forwarded even when its default action does not terminate.
func TestForwardsTheSignalOption(t *testing.T) {
	winch := strconv.Itoa(int(syscall.SIGWINCH))
	s := startTimeout(t, 300*time.Millisecond, "-s", "WINCH", "10", "HELPER", "catch", winch)
	s.signal(t, syscall.SIGWINCH)
	r := s.wait(t)
	if r.code != 40 {
		t.Fatalf("%v; want exit 40 (the command caught WINCH)", r)
	}
}

// A signal forwarded with a duration of 0 still starts -k (POSIX; ADR-0003).
func TestForwardedSignalStartsKillAfter(t *testing.T) {
	s := startTimeout(t, 300*time.Millisecond, "-k", "0.3", "0", "sh", "-c", `trap "" TERM; sleep 10`)
	s.signal(t, syscall.SIGTERM)
	r := s.wait(t)
	if r.code != -1 || r.signal != syscall.SIGKILL || r.elapsed > 5*time.Second {
		t.Fatalf("%v; want killed by SIGKILL about 0.3s after the TERM", r)
	}
}

// Without -f, timeout is in its own process group: a kill aimed at the group
// it was started in does not reach it, and a kill aimed at its pid is
// forwarded (ADR-0004 risk 3).
func TestOuterGroupKillDoesNotReach(t *testing.T) {
	sh := command(t, "sh", "-c", `"$0" 0 sleep 30 & echo $!; wait`, bin(t))
	sh.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	out, err := sh.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := sh.Start(); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 32)
	n, _ := out.Read(buf)
	pid, err := strconv.Atoi(strings.TrimSpace(string(buf[:n])))
	if err != nil {
		t.Fatalf("reading timeout's pid: %v", err)
	}
	t.Cleanup(func() { _ = syscall.Kill(pid, syscall.SIGKILL) })
	time.Sleep(300 * time.Millisecond)

	_ = syscall.Kill(-sh.Process.Pid, syscall.SIGTERM) // the original group
	_ = sh.Wait()
	time.Sleep(300 * time.Millisecond)
	if !alive(pid) {
		t.Fatal("timeout died from a kill aimed at the group it was started in")
	}

	_ = syscall.Kill(pid, syscall.SIGTERM) // timeout's own pid: forwarded
	deadline := time.Now().Add(3 * time.Second)
	for alive(pid) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if alive(pid) && !isZombie(pid) {
		t.Fatal("timeout did not end after SIGTERM to its pid")
	}
}

// isZombie reports whether pid has exited but not been reaped (its parent,
// the shell, is gone, so launchd reaps it eventually).
func isZombie(pid int) bool {
	out, err := exec.Command("ps", "-o", "stat=", "-p", strconv.Itoa(pid)).Output()
	return err == nil && strings.HasPrefix(strings.TrimSpace(string(out)), "Z")
}

func readPid(t *testing.T, file string) int {
	t.Helper()
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil {
		t.Fatalf("pid file %s: %v", file, err)
	}
	return pid
}

// -v reports forwarded signals too, as GNU does.
func TestVerboseReportsForwardedSignals(t *testing.T) {
	s := startTimeout(t, 300*time.Millisecond, "-v", "10", "sleep", "10")
	s.signal(t, syscall.SIGHUP)
	r := s.wait(t)
	want := "timeout: sending signal HUP to command 'sleep'\n"
	if r.signal != syscall.SIGHUP || r.stderr != want {
		t.Fatalf("%v, stderr %q; want killed by SIGHUP and %q", r, r.stderr, want)
	}
}

// What timeout sends its own group is recorded by kqueue too; it must not
// be forwarded back. Without the subtraction timeout would echo TERM to the
// group over and over while the command catches it.
func TestOwnGroupSignalsAreNotForwardedBack(t *testing.T) {
	term := strconv.Itoa(int(syscall.SIGTERM))
	r := run(t, "-p", "0.3", "HELPER", "count", term, "2")
	if r.code < 1 || r.code > 3 {
		t.Fatalf("%v; want the command to see TERM once or twice (exit 1..3), not an echo", r)
	}
}

// Without -f, timeout leads a process group of its own; with -f it stays in
// the group it was started in.
func TestForegroundDoesNotCreateAGroup(t *testing.T) {
	own := syscall.Getpgrp()
	pgidOfTimeout := func(args ...string) int {
		t.Helper()
		r := run(t, append(args, "sh", "-c", `ps -o pgid= -p $PPID`)...)
		n, err := strconv.Atoi(strings.TrimSpace(r.stdout))
		if r.code != 0 || err != nil {
			t.Fatalf("%v, stdout %q: %v", r, r.stdout, err)
		}
		return n
	}
	if g := pgidOfTimeout("-f", "5"); g != own {
		t.Errorf("with -f, timeout's group is %d; want the test's own %d", g, own)
	}
	if g := pgidOfTimeout("5"); g == own {
		t.Errorf("without -f, timeout stayed in the test's group %d", g)
	}
}

// timeout ignores SIGTTIN and SIGTTOU (POSIX), so it cannot be stopped by them.
func TestIgnoresTTINandTTOU(t *testing.T) {
	for _, sig := range []syscall.Signal{syscall.SIGTTIN, syscall.SIGTTOU} {
		s := startTimeout(t, 300*time.Millisecond, "10", "sleep", "10")
		s.signal(t, sig)
		time.Sleep(200 * time.Millisecond)
		out, _ := exec.Command("ps", "-o", "stat=", "-p", strconv.Itoa(s.cmd.Process.Pid)).Output()
		if strings.HasPrefix(strings.TrimSpace(string(out)), "T") {
			_ = s.cmd.Process.Kill()
			t.Fatalf("%v stopped timeout (ps stat %q)", sig, out)
		}
		s.signal(t, syscall.SIGTERM)
		if r := s.wait(t); r.signal != syscall.SIGTERM {
			t.Fatalf("after %v: %v; want killed by SIGTERM", sig, r)
		}
	}
}

package e2e

import (
	"strings"
	"testing"
)

func TestVersion(t *testing.T) {
	r := run(t, "--version")
	if r.code != 0 {
		t.Fatalf("exit %d, want 0; stderr=%q", r.code, r.stderr)
	}
	if !strings.HasPrefix(r.stdout, "timeout version ") || !strings.HasSuffix(r.stdout, "\n") {
		t.Fatalf("stdout %q, want \"timeout version <VERSION>\\n\"", r.stdout)
	}
	if r.stderr != "" {
		t.Fatalf("stderr %q, want empty", r.stderr)
	}
}

// --help goes to standard output with status 0 (GNU; RFP §2).
func TestHelp(t *testing.T) {
	r := run(t, "--help")
	if r.code != 0 {
		t.Fatalf("exit %d, want 0; stderr=%q", r.code, r.stderr)
	}
	if !strings.HasPrefix(r.stdout, "Usage: timeout ") {
		t.Fatalf("stdout %q, want the usage text", r.stdout)
	}
	if r.stderr != "" {
		t.Fatalf("stderr %q, want empty", r.stderr)
	}
}

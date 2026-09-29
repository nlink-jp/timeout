#!/bin/sh
# run.sh — positive controls for the C safety layers (ADR-0001).
#
# Each control is a tiny file with a planted bug. A layer passes only when
# it fails on its control, and for the right reason: the exit status and
# the diagnostic text are both checked, so a control stopped by a different
# layer (ASan catching what UBSan should) does not count.
#
# Environment (set by the Makefile): CC, WARN_FLAGS, BOUNDS_FLAGS,
# TEST_SAN_FLAGS, OUT (a scratch directory under dist/).

set -u

here=$(cd "$(dirname "$0")" && pwd)
: "${CC:?}" "${WARN_FLAGS:?}" "${BOUNDS_FLAGS:?}" "${TEST_SAN_FLAGS:?}" "${OUT:?}"
mkdir -p "$OUT"
failed=0

# expect NAME WANT_RC PATTERN LOGFILE RC
expect() {
	name=$1 want_rc=$2 pattern=$3 log=$4 rc=$5
	if [ "$rc" -ne "$want_rc" ]; then
		echo "controls: FAIL $name — exit status $rc, want $want_rc" >&2
		sed 's/^/    /' "$log" >&2
		failed=1
	elif ! grep -q -- "$pattern" "$log"; then
		echo "controls: FAIL $name — failed, but without \"$pattern\"" >&2
		sed 's/^/    /' "$log" >&2
		failed=1
	else
		echo "controls: ok   $name"
	fi
}

# Layer 1: warnings are fatal.
# shellcheck disable=SC2086
$CC $WARN_FLAGS -c "$here/warn_uninit.c" -o /dev/null >"$OUT/warn.log" 2>&1
expect "warnings (uninitialized)" 1 "Wsometimes-uninitialized" "$OUT/warn.log" $?

# Layer 2: static analysis findings are fatal.
# shellcheck disable=SC2086
$CC --analyze -Xanalyzer -analyzer-werror -w "$here/analyze_stack_escape.c" -o /dev/null >"$OUT/analyze.log" 2>&1
expect "static analysis (stack escape)" 1 "StackAddressEscape" "$OUT/analyze.log" $?

# Layer 3: -fbounds-safety traps (SIGTRAP = 128+5).
# shellcheck disable=SC2086
if $CC $BOUNDS_FLAGS -w "$here/bounds_index.c" -o "$OUT/bounds" >"$OUT/bounds-build.log" 2>&1; then
	{ ( "$OUT/bounds" ) >"$OUT/bounds.log" 2>&1; } 2>/dev/null
	rc=$?
	echo "exit $rc" >>"$OUT/bounds.log"
	expect "bounds safety (index)" 133 "exit 133" "$OUT/bounds.log" "$rc"
else
	echo "controls: FAIL bounds safety — control did not build" >&2
	sed 's/^/    /' "$OUT/bounds-build.log" >&2
	failed=1
fi

# Layer 4: ASan aborts (SIGABRT = 128+6).
# shellcheck disable=SC2086
if $CC $TEST_SAN_FLAGS -w "$here/asan_overflow.c" -o "$OUT/asan" >"$OUT/asan-build.log" 2>&1; then
	{ ( "$OUT/asan" ) >"$OUT/asan.log" 2>&1; } 2>/dev/null
	expect "ASan (overflow)" 134 "AddressSanitizer: stack-buffer-overflow" "$OUT/asan.log" $?
else
	echo "controls: FAIL ASan — control did not build" >&2; failed=1
fi

# Layer 4: UBSan does not recover.
# shellcheck disable=SC2086
if $CC $TEST_SAN_FLAGS -w "$here/ubsan_overflow.c" -o "$OUT/ubsan" >"$OUT/ubsan-build.log" 2>&1; then
	{ ( "$OUT/ubsan" ) >"$OUT/ubsan.log" 2>&1; } 2>/dev/null
	expect "UBSan (signed overflow)" 134 "runtime error: signed integer overflow" "$OUT/ubsan.log" $?
else
	echo "controls: FAIL UBSan — control did not build" >&2; failed=1
fi

# Layer 4: pattern auto-var init fills uninitialized reads.
# shellcheck disable=SC2086
if $CC $TEST_SAN_FLAGS -w "$here/pattern_init.c" -o "$OUT/pattern" >"$OUT/pattern-build.log" 2>&1; then
	"$OUT/pattern" >"$OUT/pattern.log" 2>&1
	expect "pattern init (uninitialized read)" 0 "^aaaaaaaa$" "$OUT/pattern.log" $?
else
	echo "controls: FAIL pattern init — control did not build" >&2; failed=1
fi

exit "$failed"

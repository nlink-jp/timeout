/*
 * unit_test.c — unit tests and input stress for the pure parsers
 * (ADR-0001 layer 5). Built with the test flags (ASan, UBSan, bounds
 * safety, pattern init) by `make unit`.
 */
#include <ptrcheck.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/duration.h"
#include "../../src/signames.h"

static int failures = 0;

#define NS INT64_C(1000000000)

static void check_duration(const char *in, int want_rc, int64_t want_ns)
{
	int64_t ns = -7;
	int rc = parse_duration(in, &ns);
	if (rc != want_rc || ns != want_ns) {
		printf("FAIL parse_duration(\"%s\") = %d, %" PRId64 "; want %d, %" PRId64 "\n",
		       in, rc, ns, want_rc, want_ns);
		failures++;
	}
}

static void check_signal(const char *in, int want)
{
	int got = parse_signal(in);
	if (got != want) {
		printf("FAIL parse_signal(\"%s\") = %d; want %d\n", in, got, want);
		failures++;
	}
}

static void test_duration_table(void)
{
	/* no limit */
	check_duration("0", 0, 0);
	check_duration("-0", 0, 0);
	check_duration("0s", 0, 0);
	check_duration("0.0d", 0, 0);
	/* units */
	check_duration("1", 0, 1 * NS);
	check_duration("1s", 0, 1 * NS);
	check_duration("1m", 0, 60 * NS);
	check_duration("1h", 0, 3600 * NS);
	check_duration("1d", 0, 86400 * NS);
	check_duration("1.5", 0, 1500000000);
	check_duration("0.5m", 0, 30 * NS);
	check_duration(".5", 0, 500000000);
	check_duration("1.", 0, 1 * NS);
	check_duration("+2", 0, 2 * NS);
	check_duration("1e3", 0, 1000 * NS);
	check_duration(" 3", 0, 3 * NS); /* strtod skips leading space, as GNU */
	/* hexadecimal, as strtod reads it */
	check_duration("0x10", 0, 16 * NS);
	check_duration("0x1d", 0, 29 * NS); /* 29 seconds, not one day */
	check_duration("0x1p0", 0, 1 * NS);
	check_duration("0x1p0d", 0, 86400 * NS);
	check_duration("0X1P-1", 0, 500000000);
	/* tiny values round up; never "no limit" */
	check_duration("1e-10", 0, 1);
	check_duration("1e-9", 0, 1);
	check_duration("1.5e-9", 0, 2);
	check_duration("1e-400", 0, 1);
	/* large values clamp */
	check_duration("1e400", 0, DURATION_MAX_NS);
	check_duration("inf", 0, DURATION_MAX_NS);
	check_duration("INFINITY", 0, DURATION_MAX_NS);
	check_duration("2147483647", 0, DURATION_MAX_NS);
	check_duration("2147483646", 0, INT64_C(2147483646) * NS);
	check_duration("3e9", 0, DURATION_MAX_NS);
	check_duration("24856d", 0, DURATION_MAX_NS);
	/* invalid */
	check_duration("", -1, 0);
	check_duration("-1", -1, 0);
	check_duration("-1e-9", -1, 0);
	check_duration("nan", -1, 0);
	check_duration("NaN", -1, 0);
	check_duration("1x", -1, 0);
	check_duration("1ss", -1, 0);
	check_duration("1 ", -1, 0);
	check_duration("1 s", -1, 0);
	check_duration("s", -1, 0);
	check_duration("abc", -1, 0);
	check_duration("0x", -1, 0);
	check_duration("1_000", -1, 0);
	check_duration("1,5", -1, 0);
	check_duration("--1", -1, 0);
	check_duration("1S", -1, 0);
}

static void test_signal_table(void)
{
	check_signal("TERM", 15);
	check_signal("term", 15);
	check_signal("SIGTERM", 15);
	check_signal("sigTerm", 15);
	check_signal("KILL", 9);
	check_signal("HUP", 1);
	check_signal("INFO", 29);
	check_signal("EMT", 7);
	check_signal("STOP", 17);
	check_signal("stop", 17);
	check_signal("SIGSTOP", 17);
	check_signal("USR2", 31);
	check_signal("15", 15);
	check_signal("9", 9);
	check_signal("1", 1);
	check_signal("31", 31);
	check_signal("01", 1);
	check_signal("0", -1);
	check_signal("32", -1);
	check_signal("001", -1);
	check_signal("-9", -1);
	check_signal("9x", -1);
	check_signal("", -1);
	check_signal("SIG", -1);
	check_signal("TERMX", -1);
	check_signal("SIGSIGTERM", -1);
	check_signal("S", -1);
	check_signal("SI", -1);
	check_signal("TER", -1);

	/* every name round-trips */
	for (int n = 1; n <= 31; n++) {
		const char *__null_terminated name = signal_name(n);
		if (name == NULL || parse_signal(name) != n) {
			printf("FAIL round trip for signal %d\n", n);
			failures++;
		}
	}
	if (signal_name(0) != NULL || signal_name(32) != NULL || signal_name(-1) != NULL) {
		printf("FAIL signal_name outside 1..31 is not NULL\n");
		failures++;
	}
}

/* ---- input stress --------------------------------------------------- */

/* xorshift64: deterministic, so a failure reproduces from the seed. */
static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t rng(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

static const char *const pieces[] = {
	"0", "1", "9", ".", "e", "E", "-", "+", "x", "X", "p", "P", "s", "m", "h", "d",
	"inf", "nan", "0x", "1e308", "1e-308", "1e400", "1e-400", "2147483647",
	"SIG", "sig", "TERM", "KILL", "31", "32", " ", "\t", "_", ",", "\xff", "\x80",
};
enum { PIECES_N = sizeof pieces / sizeof pieces[0] };

enum { STRESS_BUF = 4096 };
static char stress_buf[STRESS_BUF]; /* static, not on the stack (ADR-0001 rule 1) */

/* Fill stress_buf with one generated input and return its length. */
static size_t generate(void)
{
	size_t len = 0;
	uint64_t kind = rng() % 4;
	size_t target = (kind == 3) ? (size_t)(rng() % (STRESS_BUF - 1)) : (size_t)(rng() % 24);
	while (len < target) {
		if (kind == 0 || kind == 3) {
			/* raw bytes, never NUL */
			stress_buf[len++] = (char)(1 + rng() % 255);
		} else {
			const char *__null_terminated p = pieces[rng() % PIECES_N];
			size_t n = strlen(p);
			if (len + n > target)
				break;
			memcpy(&stress_buf[len], __null_terminated_to_indexable(p), n);
			len += n;
		}
	}
	stress_buf[len] = '\0';
	return len;
}

static void stress(int rounds)
{
	int bad = 0;
	for (int i = 0; i < rounds; i++) {
		size_t len = generate();
		/* The one unsafe conversion in the tests: generate() wrote the NUL
		 * at stress_buf[len], which the explicit terminator pointer names. */
		const char *__null_terminated in =
			__unsafe_null_terminated_from_indexable(stress_buf, &stress_buf[len]);

		int64_t ns = -7;
		int rc = parse_duration(in, &ns);
		int ok = (rc == 0 && ns >= 0 && ns <= DURATION_MAX_NS) || (rc == -1 && ns == 0);

		int sig = parse_signal(in);
		ok = ok && (sig == -1 || (sig >= 1 && sig <= 31));
		if (sig != -1) {
			const char *__null_terminated name = signal_name(sig);
			ok = ok && name != NULL && parse_signal(name) == sig;
		}
		if (!ok && bad++ < 5)
			printf("FAIL stress round %d: rc=%d ns=%" PRId64 " sig=%d\n", i, rc, ns, sig);
	}
	if (bad > 0)
		failures++;
	printf("stress: %d rounds, %d invariant violations\n", rounds, bad);
}

int main(void)
{
	test_duration_table();
	test_signal_table();
	stress(200000);
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}

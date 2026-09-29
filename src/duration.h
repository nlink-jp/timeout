/*
 * duration.h — parsing DURATION operands (ADR-0005).
 */
#ifndef TIMEOUT_DURATION_H
#define TIMEOUT_DURATION_H

#include <stdint.h>

/* The largest duration: 2^31-1 seconds (about 68 years), in nanoseconds. */
#define DURATION_MAX_SECONDS INT64_C(2147483647)
#define DURATION_MAX_NS (DURATION_MAX_SECONDS * INT64_C(1000000000))

/*
 * Parse a duration by GNU's rules: a number strtod accepts in the C locale,
 * then an optional unit s, m, h or d. On success store the duration in
 * nanoseconds in *ns — 0 means no limit — and return 0; return -1 when the
 * operand is not a valid duration (*ns is then 0).
 *
 *   0, -0                    -> 0 (no limit)
 *   below 1 ns, or underflow -> 1 ns (never "no limit")
 *   overflow, inf, too large -> DURATION_MAX_NS
 *   negative, NaN, garbage   -> -1
 */
int parse_duration(const char *s, int64_t *ns);

#endif

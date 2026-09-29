/*
 * duration.c — parsing DURATION operands (ADR-0005).
 */
#include "duration.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The multiplier for a one-character unit suffix, or 0 if it is not one. */
static double unit_multiplier(char c)
{
	switch (c) {
	case 's':
		return 1.0;
	case 'm':
		return 60.0;
	case 'h':
		return 3600.0;
	case 'd':
		return 86400.0;
	default:
		return 0.0;
	}
}

int parse_duration(const char *s, int64_t *ns)
{
	*ns = 0;
	if (s == NULL || s[0] == '\0')
		return -1;

	char *__null_terminated end = NULL;
	errno = 0;
	double v = strtod(s, &end);
	int range_error = (errno == ERANGE);
	if (end == NULL || end == s)
		return -1;

	/* What follows the number: nothing, or exactly one unit character. */
	size_t rest = strlen(end);
	double mult = 1.0;
	if (rest == 1) {
		mult = unit_multiplier(end[0]);
		if (mult == 0.0)
			return -1;
	} else if (rest != 0) {
		return -1;
	}

	if (isnan(v) || v < 0.0)
		return -1;

	if (v == 0.0) {
		/* strtod gives 0 with ERANGE on underflow: a tiny positive value. */
		*ns = range_error ? 1 : 0;
		return 0;
	}

	double nsd = v * mult * 1e9;
	if (!isfinite(nsd) || nsd >= (double)DURATION_MAX_NS) {
		*ns = DURATION_MAX_NS;
		return 0;
	}
	double up = ceil(nsd);
	if (up < 1.0)
		up = 1.0;
	/* up is finite and within [1, DURATION_MAX_NS), so the cast is defined. */
	*ns = (int64_t)up;
	return 0;
}

/*
 * signames.c — signal names and numbers on macOS.
 */
#include "signames.h"

#include <signal.h>
#include <stddef.h>

struct signame {
	int number;
	const char *name;
};

/* Every signal macOS defines (signal(3)), by number. */
static const struct signame signames[] = {
	{ SIGHUP, "HUP" },       { SIGINT, "INT" },       { SIGQUIT, "QUIT" },
	{ SIGILL, "ILL" },       { SIGTRAP, "TRAP" },     { SIGABRT, "ABRT" },
	{ SIGEMT, "EMT" },       { SIGFPE, "FPE" },       { SIGKILL, "KILL" },
	{ SIGBUS, "BUS" },       { SIGSEGV, "SEGV" },     { SIGSYS, "SYS" },
	{ SIGPIPE, "PIPE" },     { SIGALRM, "ALRM" },     { SIGTERM, "TERM" },
	{ SIGURG, "URG" },       { SIGSTOP, "STOP" },     { SIGTSTP, "TSTP" },
	{ SIGCONT, "CONT" },     { SIGCHLD, "CHLD" },     { SIGTTIN, "TTIN" },
	{ SIGTTOU, "TTOU" },     { SIGIO, "IO" },         { SIGXCPU, "XCPU" },
	{ SIGXFSZ, "XFSZ" },     { SIGVTALRM, "VTALRM" }, { SIGPROF, "PROF" },
	{ SIGWINCH, "WINCH" },   { SIGINFO, "INFO" },     { SIGUSR1, "USR1" },
	{ SIGUSR2, "USR2" },
};
enum { SIGNAMES_N = sizeof signames / sizeof signames[0] };

/* ASCII upper-casing; the C locale is never changed. */
static char upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Compare s, case-insensitively, with the upper-case name. */
static int equal_name(const char *s, const char *name)
{
	for (;;) {
		char a = upper(*s);
		char b = *name;
		if (a != b)
			return 0;
		if (a == '\0')
			return 1;
		s++;
		name++;
	}
}

/* s without a leading "SIG" in any case; s itself if it has none. */
static const char *skip_sig_prefix(const char *s)
{
	if (upper(s[0]) != 'S')
		return s;
	s++;
	if (upper(s[0]) != 'I')
		return NULL;
	s++;
	if (upper(s[0]) != 'G')
		return NULL;
	s++;
	return s;
}

/* A decimal number from 1 to 31, or -1. Digits only; at most two. */
static int parse_number(const char *s)
{
	int value = 0;
	int digits = 0;
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9' || digits == 2)
			return -1;
		value = value * 10 + (*s - '0');
		digits++;
	}
	if (digits == 0 || value < 1 || value > 31)
		return -1;
	return value;
}

int parse_signal(const char *s)
{
	if (s == NULL || s[0] == '\0')
		return -1;
	if (s[0] >= '0' && s[0] <= '9')
		return parse_number(s);

	const char *__null_terminated bare = skip_sig_prefix(s);
	for (size_t i = 0; i < SIGNAMES_N; i++) {
		if (equal_name(s, signames[i].name))
			return signames[i].number;
		if (bare != NULL && bare != s && equal_name(bare, signames[i].name))
			return signames[i].number;
	}
	return -1;
}

const char *signal_name(int sig)
{
	for (size_t i = 0; i < SIGNAMES_N; i++)
		if (signames[i].number == sig)
			return signames[i].name;
	return NULL;
}

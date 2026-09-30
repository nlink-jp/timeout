/*
 * signames.h — signal names and numbers on macOS.
 */
#ifndef TIMEOUT_SIGNAMES_H
#define TIMEOUT_SIGNAMES_H

/*
 * Parse a SIGNAL operand: a name in any case, with or without the SIG
 * prefix (TERM, sigterm), or a decimal number from 0 to 31. 0 and EXIT name
 * the null signal, which GNU accepts (ADR-0003). Return the signal number,
 * or -1 if the operand names no signal.
 */
int parse_signal(const char *s);

/* The name of signal sig without the SIG prefix ("TERM"; "EXIT" for 0),
 * or NULL. */
const char *signal_name(int sig);

#endif

/*
 * timeout — run a command with a time limit (POSIX.1-2024 XCU timeout,
 * GNU coreutils compatible). See docs/en/timeout-rfp.md and docs/en/adr/.
 *
 * Scaffold: only --help and --version are implemented. Everything else
 * exits 125 until Phase 1 lands.
 */

#include <ptrcheck.h>
#include <stdio.h>
#include <string.h>

#ifndef TIMEOUT_VERSION
#define TIMEOUT_VERSION "dev"
#endif

enum {
	EXIT_TIMEOUT_FAILED = 125,
};

static const char usage_text[] =
	"Usage: timeout [OPTION]... DURATION COMMAND [ARG]...\n"
	"Start COMMAND, and kill it if still running after DURATION.\n"
	"\n"
	"  -f, --foreground        time out COMMAND only, not its descendants;\n"
	"                          COMMAND can read the terminal and get its signals\n"
	"  -k, --kill-after=DURATION\n"
	"                          also send KILL if COMMAND is still running\n"
	"                          this long after the initial signal was sent\n"
	"  -p, --preserve-status   exit with the same status as COMMAND,\n"
	"                          even when the command times out\n"
	"  -s, --signal=SIGNAL     the signal to send on time out (default TERM);\n"
	"                          a name like HUP or a number\n"
	"  -v, --verbose           report any signal sent on time out to stderr\n"
	"      --help              display this help and exit\n"
	"      --version           output version information and exit\n"
	"\n"
	"DURATION is a number with an optional suffix: s seconds (default),\n"
	"m minutes, h hours, d days. A duration of 0 disables the time limit.\n"
	"\n"
	"Exit status: 124 if COMMAND times out (without --preserve-status),\n"
	"125 if timeout itself fails, 126 if COMMAND is found but cannot be\n"
	"invoked, 127 if COMMAND cannot be found, otherwise the status of COMMAND.\n";

int main(int argc, char *__null_terminated *__counted_by(argc) argv)
{
	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		fputs(usage_text, stdout);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("timeout version %s\n", TIMEOUT_VERSION);
		return 0;
	}
	fputs("timeout: not implemented yet\n", stderr);
	return EXIT_TIMEOUT_FAILED;
}

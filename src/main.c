/*
 * timeout — run a command with a time limit (POSIX.1-2024 XCU timeout,
 * GNU coreutils compatible). See docs/en/timeout-rfp.md and docs/en/adr/.
 */

#include <ptrcheck.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

#include "duration.h"
#include "signames.h"
#include "sigstate.h"

#ifndef TIMEOUT_VERSION
#define TIMEOUT_VERSION "dev"
#endif

enum {
	EXIT_TIMEDOUT = 124,
	EXIT_CANCELED = 125,
	EXIT_CANNOT_INVOKE = 126,
	EXIT_ENOENT = 127,
};

enum { TIMER_MAIN = 1, TIMER_KILL = 2 };

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

enum { OPT_HELP = 256, OPT_VERSION };

static const struct option longopts[] = {
	{ "foreground", no_argument, NULL, 'f' },
	{ "kill-after", required_argument, NULL, 'k' },
	{ "preserve-status", no_argument, NULL, 'p' },
	{ "signal", required_argument, NULL, 's' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, OPT_HELP },
	{ "version", no_argument, NULL, OPT_VERSION },
	{ NULL, 0, NULL, 0 },
};

/* The supervisor's state. Written only by the main loop. */
static int opt_foreground;
static int opt_preserve;
static int opt_verbose;
static int timeout_sig = SIGTERM;
static int64_t kill_after_ns;
static const char *__null_terminated command_name = "";
static pid_t child_pid;
static int kq = -1;
static int timed_out;
static int first_signal_sent;
static int kill_after_sent;
static int64_t self_sent[SIG_SLOTS];

/* Write a string to standard error with write(2): usable anywhere,
 * including the forked child (ADR-0001 rule 5). */
static void put_err(const char *__null_terminated s)
{
	const char *p = __null_terminated_to_indexable(s);
	size_t n = strlen(s);
	while (n > 0) {
		ssize_t w = write(STDERR_FILENO, p, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return;
		p += w;
		n -= (size_t)w;
	}
}

static void try_help(void)
{
	put_err("Try 'timeout --help' for more information.\n");
}

static void fail(const char *__null_terminated what, const char *__null_terminated detail)
{
	put_err("timeout: ");
	put_err(what);
	if (detail != NULL) {
		put_err(": ");
		put_err(detail);
	}
	put_err("\n");
}

static int arm_timer(uintptr_t ident, int64_t ns)
{
	struct kevent ev = {0};
	EV_SET(&ev, ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, (intptr_t)ns, NULL);
	return kevent(kq, &ev, 1, NULL, 0, NULL);
}

/* Count a signal timeout is about to send itself, so its kqueue record can
 * be told from one that came from outside (ADR-0002). */
static void note_self_sent(int sig)
{
	if (sig > 0 && sig < SIG_SLOTS && sig_received(sig))
		self_sent[sig]++;
}

static void report(int sig)
{
	if (!opt_verbose)
		return;
	const char *__null_terminated name = signal_name(sig);
	put_err("timeout: sending signal ");
	put_err(name != NULL ? name : "?");
	put_err(" to command '");
	put_err(command_name);
	put_err("'\n");
}

/* Send sig as the standard requires: to the child, then (without -f) to the
 * process group, followed by SIGCONT unless sig is KILL or CONT. */
static void send_signal(int sig)
{
	(void)kill(child_pid, sig);
	if (!opt_foreground) {
		note_self_sent(sig);
		(void)kill(0, sig); /* SIGKILL ends timeout here, by design */
		if (sig != SIGKILL && sig != SIGCONT) {
			(void)kill(child_pid, SIGCONT);
			note_self_sent(SIGCONT);
			(void)kill(0, SIGCONT);
		}
	}
	if (!first_signal_sent) {
		first_signal_sent = 1;
		/* POSIX: a forwarded signal also counts as the first signal,
		 * even with a duration of 0 (ADR-0003). */
		if (kill_after_ns > 0 && sig != SIGKILL && arm_timer(TIMER_KILL, kill_after_ns) != 0)
			fail("cannot arm the kill-after timer", strerror(errno));
	}
}

static void on_signal_event(int sig, int64_t count)
{
	int64_t mine = (sig > 0 && sig < SIG_SLOTS) ? self_sent[sig] : 0;
	if (sig > 0 && sig < SIG_SLOTS)
		self_sent[sig] = mine > count ? mine - count : 0;
	sig_rearm(sig);
	if (count > mine)
		send_signal(sig);
}

/* Run the command in the forked child. Never returns. */
_Noreturn static void run_child(char *__null_terminated const *__null_terminated cmd)
{
	sig_restore_child(timeout_sig);
	execvp(command_name, cmd);
	int err = errno;
	/* strerror returns constant text; the process is single-threaded. */
	put_err("timeout: failed to run command '");
	put_err(command_name);
	put_err("': ");
	put_err(strerror(err));
	put_err("\n");
	_exit(err == ENOENT ? EXIT_ENOENT : EXIT_CANNOT_INVOKE);
}

static int wait_child(void)
{
	int status = 0;
	for (;;) {
		pid_t r = waitpid(child_pid, &status, 0);
		if (r == child_pid)
			return status;
		if (r < 0 && errno != EINTR) {
			fail("waitpid failed", strerror(errno));
			return -1;
		}
	}
}

/* The exit status timeout reports, or self-termination (ADR-0003). */
static int finish(int status)
{
	if (status == -1)
		return EXIT_CANCELED;
	if (timed_out && !opt_preserve) {
		if (kill_after_sent)
			sig_self_terminate(SIGKILL);
		return EXIT_TIMEDOUT;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		sig_self_terminate(WTERMSIG(status));
	return EXIT_CANCELED;
}

static int supervise(int64_t duration_ns, char *__null_terminated const *__null_terminated cmd)
{
	if (sig_record_inherited() != 0) {
		fail("cannot read signal dispositions", strerror(errno));
		return EXIT_CANCELED;
	}
	if (!opt_foreground)
		(void)setpgid(0, 0); /* already a session leader: ignored, as GNU */

	kq = kqueue();
	if (kq < 0 || sig_setup_parent(kq, timeout_sig) != 0) {
		fail("cannot set up signal handling", strerror(errno));
		return EXIT_CANCELED;
	}

	sigset_t saved = {0};
	if (sig_block_for_fork(&saved) != 0) {
		fail("cannot block signals", strerror(errno));
		return EXIT_CANCELED;
	}
	child_pid = fork();
	if (child_pid < 0) {
		fail("fork system call failed", strerror(errno));
		return EXIT_CANCELED;
	}
	if (child_pid == 0) {
		close(kq);
		run_child(cmd);
	}
	(void)sig_unblock_after_fork(&saved);

	struct kevent reg = {0};
	EV_SET(&reg, (uintptr_t)child_pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
	if (kevent(kq, &reg, 1, NULL, 0, NULL) != 0) {
		if (errno == ESRCH) /* already exited */
			return finish(wait_child());
		fail("cannot watch the command", strerror(errno));
		(void)kill(child_pid, SIGKILL);
		(void)wait_child();
		return EXIT_CANCELED;
	}
	if (duration_ns > 0 && arm_timer(TIMER_MAIN, duration_ns) != 0) {
		fail("cannot arm the timer", strerror(errno));
		(void)kill(child_pid, SIGKILL);
		(void)wait_child();
		return EXIT_CANCELED;
	}

	for (;;) {
		struct kevent ev = {0};
		int n = kevent(kq, NULL, 0, &ev, 1, NULL);
		sig_new_epoch();
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fail("kevent failed", strerror(errno));
			(void)kill(child_pid, SIGKILL);
			return finish(wait_child());
		}
		if (n == 0)
			continue;
		switch (ev.filter) {
		case EVFILT_PROC:
			return finish(wait_child());
		case EVFILT_TIMER:
			if (ev.ident == TIMER_MAIN) {
				timed_out = 1;
				report(timeout_sig);
				send_signal(timeout_sig);
			} else if (ev.ident == TIMER_KILL) {
				kill_after_sent = 1;
				report(SIGKILL);
				send_signal(SIGKILL);
			}
			break;
		case EVFILT_SIGNAL:
			on_signal_event((int)ev.ident, (int64_t)ev.data);
			break;
		default:
			break;
		}
	}
}

int main(int argc, char *__null_terminated *__counted_by(argc) argv)
{
	int c = 0;
	while ((c = getopt_long(argc, argv, "+fk:ps:v", longopts, NULL)) != -1) {
		switch (c) {
		case 'f':
			opt_foreground = 1;
			break;
		case 'k':
			if (parse_duration(optarg, &kill_after_ns) != 0) {
				put_err("timeout: invalid time interval '");
				put_err(optarg);
				put_err("'\n");
				try_help();
				return EXIT_CANCELED;
			}
			break;
		case 'p':
			opt_preserve = 1;
			break;
		case 's':
			timeout_sig = parse_signal(optarg);
			if (timeout_sig < 0) {
				put_err("timeout: invalid signal '");
				put_err(optarg);
				put_err("'\n");
				try_help();
				return EXIT_CANCELED;
			}
			break;
		case 'v':
			opt_verbose = 1;
			break;
		case OPT_HELP:
			fputs(usage_text, stdout);
			return fflush(stdout) == 0 ? 0 : EXIT_CANCELED;
		case OPT_VERSION:
			printf("timeout version %s\n", TIMEOUT_VERSION);
			return fflush(stdout) == 0 ? 0 : EXIT_CANCELED;
		default:
			try_help();
			return EXIT_CANCELED;
		}
	}

	if (argc - optind < 2) {
		put_err("timeout: missing operand\n");
		try_help();
		return EXIT_CANCELED;
	}

	int64_t duration_ns = 0;
	if (parse_duration(argv[optind], &duration_ns) != 0) {
		put_err("timeout: invalid time interval '");
		put_err(argv[optind]);
		put_err("'\n");
		try_help();
		return EXIT_CANCELED;
	}

	command_name = argv[optind + 1];
	/* The single sanctioned bounds exception (ADR-0001): argv[argc] is the
	 * NULL that terminates the vector, outside the counted range. */
	char *__null_terminated const *__null_terminated cmd =
		__unsafe_forge_null_terminated(char *__null_terminated const *, &argv[optind + 1]);
	return supervise(duration_ns, cmd);
}

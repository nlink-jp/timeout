/*
 * helper.c — a child for the e2e tests to run under timeout.
 *
 *   helper disp                  print dispositions 1..31 and the mask:
 *                                "1=D 2=I ... mask=0x..." (D default,
 *                                I ignored, H handler, - unsettable)
 *   helper ignore N,N -- CMD...  set signals N to ignored, then exec CMD
 *   helper block N,N -- CMD...   block signals N, then exec CMD
 *   helper catch N               exit 40 when signal N arrives; exit 1
 *                                if it does not within 5 seconds
 *   helper setpgid-sleep S       leave the process group, sleep S seconds
 */
#include <ptrcheck.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_s(int s)
{
	struct timespec ts = { s, 0 };
	nanosleep(&ts, NULL);
}

/* Parse "N,N,..." into a set; -1 on anything else. */
static int parse_set(const char *s, sigset_t *set)
{
	sigemptyset(set);
	int value = 0;
	int digits = 0;
	for (;; s++) {
		char c = *s;
		if (c >= '0' && c <= '9' && digits < 2) {
			value = value * 10 + (c - '0');
			digits++;
			continue;
		}
		if ((c == ',' || c == '\0') && digits > 0 && value >= 1 && value <= 31) {
			sigaddset(set, value);
			value = 0;
			digits = 0;
			if (c == '\0')
				return 0;
			continue;
		}
		return -1;
	}
}

static int disp(void)
{
	sigset_t mask = {0};
	sigprocmask(SIG_SETMASK, NULL, &mask);
	for (int sig = 1; sig <= 31; sig++) {
		char c = '-';
		if (sig != SIGKILL && sig != SIGSTOP) {
			struct sigaction sa = {0};
			sigaction(sig, NULL, &sa);
			c = sa.sa_handler == SIG_IGN ? 'I' : sa.sa_handler == SIG_DFL ? 'D' : 'H';
		}
		printf("%d=%c ", sig, c);
	}
	unsigned bits = 0;
	for (int sig = 1; sig <= 31; sig++)
		if (sigismember(&mask, sig))
			bits |= 1u << (sig - 1);
	printf("mask=0x%08x\n", bits);
	return 0;
}

static void on_catch(int sig)
{
	(void)sig;
	_exit(40);
}

int main(int argc, char *__null_terminated *__counted_by(argc) argv)
{
	if (argc == 2 && strcmp(argv[1], "disp") == 0)
		return disp();

	if (argc >= 5 && (strcmp(argv[1], "ignore") == 0 || strcmp(argv[1], "block") == 0) &&
	    strcmp(argv[3], "--") == 0) {
		sigset_t set = {0};
		if (parse_set(argv[2], &set) != 0)
			return 2;
		if (strcmp(argv[1], "block") == 0) {
			sigprocmask(SIG_BLOCK, &set, NULL);
		} else {
			for (int sig = 1; sig <= 31; sig++) {
				if (!sigismember(&set, sig))
					continue;
				struct sigaction sa = {0};
				sa.sa_handler = SIG_IGN;
				sigemptyset(&sa.sa_mask);
				sigaction(sig, &sa, NULL);
			}
		}
		char *__null_terminated const *__null_terminated rest =
			__unsafe_forge_null_terminated(char *__null_terminated const *, &argv[4]);
		execvp(argv[4], rest);
		perror("helper: exec");
		return 127;
	}

	if (argc == 3 && strcmp(argv[1], "catch") == 0) {
		sigset_t set = {0};
		if (parse_set(argv[2], &set) != 0)
			return 2;
		for (int sig = 1; sig <= 31; sig++) {
			if (!sigismember(&set, sig))
				continue;
			struct sigaction sa = {0};
			sa.sa_handler = on_catch;
			sigemptyset(&sa.sa_mask);
			sigaction(sig, &sa, NULL);
		}
		sigset_t unblock = set;
		sigprocmask(SIG_UNBLOCK, &unblock, NULL);
		sleep_s(5);
		return 1;
	}

	if (argc == 3 && strcmp(argv[1], "setpgid-sleep") == 0) {
		setpgid(0, 0);
		sleep_s(argv[2][0] - '0');
		return 0;
	}

	fputs("usage: helper disp | ignore N,.. -- CMD | block N,.. -- CMD | catch N | setpgid-sleep S\n",
	      stderr);
	return 2;
}

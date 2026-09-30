/*
 * sigstate.c — signal dispositions, the three groups, and the synchronous
 * handler (ADR-0002).
 */
#include "sigstate.h"

#include <errno.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* A real fault re-executes at once; this many arrivals of one synchronous
 * signal within one main-loop epoch are taken for a fault (ADR-0002). */
enum { SYNC_STRIKES = 8 };

static struct sigaction inherited[SIG_SLOTS];
static sigset_t inherited_mask;
static int received[SIG_SLOTS];
static sigset_t group_b;

/* Written by the main loop; read by the handler. */
static volatile sig_atomic_t loop_epoch;
/* Written by the handler only. */
static volatile sig_atomic_t hit_epoch[SIG_SLOTS];
static volatile sig_atomic_t hit_count[SIG_SLOTS];

static int in_range(int sig)
{
	return sig > 0 && sig < SIG_SLOTS;
}

static int settable(int sig)
{
	return in_range(sig) && sig != SIGKILL && sig != SIGSTOP;
}

enum action { ACT_DFL, ACT_IGN };

static int set_action(int sig, enum action a)
{
	struct sigaction sa = {0};
	if (a == ACT_IGN)
		sa.sa_handler = SIG_IGN;
	else
		sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	return sigaction(sig, &sa, NULL);
}

/*
 * The handler for the synchronous five. It calls nothing but sigaction and
 * touches only volatile sig_atomic_t counters (ADR-0001 rule 6).
 */
static void sync_handler(int sig)
{
	if (in_range(sig)) {
		sig_atomic_t epoch = loop_epoch;
		if (hit_epoch[sig] != epoch) {
			hit_epoch[sig] = epoch;
			hit_count[sig] = 1;
			return;
		}
		sig_atomic_t n = hit_count[sig] + 1;
		hit_count[sig] = n;
		if (n < SYNC_STRIKES)
			return;
	}
	struct sigaction d = {0};
	d.sa_handler = SIG_DFL;
	sigemptyset(&d.sa_mask);
	(void)sigaction(sig, &d, NULL);
}

static int arm_sync_handler(int sig)
{
	struct sigaction sa = {0};
	sa.sa_handler = sync_handler;
	sigemptyset(&sa.sa_mask);
	return sigaction(sig, &sa, NULL);
}

int sig_record_inherited(void)
{
	for (int sig = 1; sig < SIG_SLOTS; sig++) {
		if (!settable(sig))
			continue;
		if (sigaction(sig, NULL, &inherited[sig]) != 0)
			return -1;
	}
	return sigprocmask(SIG_SETMASK, NULL, &inherited_mask);
}

int sig_inherited_ignored(int sig)
{
	return settable(sig) && inherited[sig].sa_handler == SIG_IGN;
}

int sig_is_forwarded(int sig)
{
	switch (sig) {
	case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL: case SIGTRAP:
	case SIGABRT: case SIGEMT: case SIGFPE: case SIGBUS: case SIGSEGV:
	case SIGSYS: case SIGPIPE: case SIGALRM: case SIGTERM: case SIGXCPU:
	case SIGXFSZ: case SIGVTALRM: case SIGPROF: case SIGUSR1: case SIGUSR2:
		return 1;
	default:
		return 0;
	}
}

int sig_is_synchronous(int sig)
{
	switch (sig) {
	case SIGSEGV: case SIGBUS: case SIGILL: case SIGTRAP: case SIGFPE:
		return 1;
	default:
		return 0;
	}
}

int sig_received(int sig)
{
	return in_range(sig) && received[sig];
}

int sig_setup_parent(int kq, int timeout_sig)
{
	sigset_t group_a = {0};
	sigemptyset(&group_a);
	sigemptyset(&group_b);

	for (int sig = 1; sig < SIG_SLOTS; sig++) {
		if (!settable(sig))
			continue;
		int receive = (sig_is_forwarded(sig) && !sig_inherited_ignored(sig)) ||
			      sig == timeout_sig;

		if (!receive) {
			/* group C, SIGCHLD, and a synchronous signal inherited as
			 * ignored — it still needs the handler, or a real fault in
			 * timeout would spin. Everything else keeps what it inherited;
			 * the child gets the inherited dispositions back either way. */
			if (sig_is_synchronous(sig)) {
				if (arm_sync_handler(sig) != 0)
					return -1;
				sigaddset(&group_b, sig);
			} else if (sig == SIGTTIN || sig == SIGTTOU) {
				if (set_action(sig, ACT_IGN) != 0)
					return -1;
			} else if (sig == SIGCHLD) {
				if (set_action(sig, ACT_DFL) != 0)
					return -1;
				sigaddset(&group_a, sig);
			}
			continue;
		}

		received[sig] = 1;
		if (sig_is_synchronous(sig)) {
			if (arm_sync_handler(sig) != 0)
				return -1;
			sigaddset(&group_b, sig);
		} else {
			if (set_action(sig, ACT_DFL) != 0)
				return -1;
			sigaddset(&group_a, sig);
		}
		struct kevent reg = {0};
		EV_SET(&reg, (uintptr_t)sig, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
		if (kevent(kq, &reg, 1, NULL, 0, NULL) != 0)
			return -1;
	}
	if (sigprocmask(SIG_BLOCK, &group_a, NULL) != 0)
		return -1;
	/* Group B must never be blocked in the parent, even if inherited so:
	 * a real fault in timeout would then spin (ADR-0002). The child gets
	 * the inherited mask back. */
	return sigprocmask(SIG_UNBLOCK, &group_b, NULL);
}

int sig_block_for_fork(sigset_t *saved)
{
	return sigprocmask(SIG_BLOCK, &group_b, saved);
}

int sig_unblock_after_fork(const sigset_t *saved)
{
	return sigprocmask(SIG_SETMASK, saved, NULL);
}

void sig_restore_child(int timeout_sig)
{
	for (int sig = 1; sig < SIG_SLOTS; sig++) {
		if (!settable(sig))
			continue;
		if (sig == timeout_sig)
			(void)set_action(sig, ACT_DFL);
		else
			(void)sigaction(sig, &inherited[sig], NULL);
	}
	(void)sigprocmask(SIG_SETMASK, &inherited_mask, NULL);
}

void sig_new_epoch(void)
{
	/* Wrap by hand: overflowing a signed sig_atomic_t is undefined. */
	loop_epoch = loop_epoch == SIG_ATOMIC_MAX ? 0 : loop_epoch + 1;
}

void sig_rearm(int sig)
{
	if (in_range(sig) && sig_is_synchronous(sig) && received[sig])
		(void)arm_sync_handler(sig);
}

void sig_prepare_self_termination(int sig)
{
	sigset_t pending = {0};
	sigemptyset(&pending);
	(void)sigpending(&pending);
	for (int s = 1; s < SIG_SLOTS; s++)
		if (settable(s) && s != sig && sigismember(&pending, s))
			(void)set_action(s, ACT_IGN); /* discards it */

	if (settable(sig))
		(void)set_action(sig, ACT_DFL);

	struct rlimit none = {0};
	(void)setrlimit(RLIMIT_CORE, &none);

	sigset_t one = {0};
	sigemptyset(&one);
	if (in_range(sig))
		sigaddset(&one, sig);
	(void)sigprocmask(SIG_UNBLOCK, &one, NULL);
}

_Noreturn void sig_self_terminate(int sig)
{
	sig_prepare_self_termination(sig);
	(void)kill(getpid(), sig);
	/* Not reached for a signal whose default action terminates. */
	struct timespec ts = { 0, 200 * 1000 * 1000 };
	(void)nanosleep(&ts, NULL);
	_exit(in_range(sig) ? 128 + sig : 125);
}

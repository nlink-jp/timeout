/*
 * probe.c — the Phase 1 experiments fixed in ADR-0002.
 *
 * Each experiment runs the design under test and a control that runs the
 * rejected alternative. A control that does not show the failure means the
 * experiment cannot tell pass from fail, and the run as a whole fails.
 *
 * Build and run: see run.sh. Output is line-oriented, one result per line.
 */

#include <ptrcheck.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const int forward_set[] = {
	SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE, SIGBUS,
	SIGSEGV, SIGSYS, SIGPIPE, SIGALRM, SIGTERM, SIGXCPU, SIGXFSZ, SIGVTALRM,
	SIGPROF, SIGUSR1, SIGUSR2,
};
enum { FORWARD_N = sizeof forward_set / sizeof forward_set[0] };

static const int sync_set[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE };
enum { SYNC_N = sizeof sync_set / sizeof sync_set[0] };

static int failures = 0;

static void report(const char *name, int ok, const char *__counted_by(len) detail, size_t len)
{
	printf("%s %s: %.*s\n", ok ? "PASS" : "FAIL", name, (int)strnlen(detail, len), detail);
	if (!ok)
		failures++;
}

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static int is_sync(int sig)
{
	for (size_t i = 0; i < SYNC_N; i++)
		if (sync_set[i] == sig)
			return 1;
	return 0;
}

enum action { ACT_DFL, ACT_IGN };

static void set_action(int sig, enum action a)
{
	struct sigaction sa = {0};
	if (a == ACT_IGN)
		sa.sa_handler = SIG_IGN;
	else
		sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, NULL) != 0)
		die("sigaction");
}

/* ---- the group B handler under test ---------------------------------- */

/* Revision 3 of ADR-0002: the handler cannot tell a signal sent with kill
 * from a real fault (macOS reports the same siginfo for both — see
 * observe_siginfo). A real fault re-executes at once, so the same signal
 * keeps arriving with no main-loop progress in between; a sent signal lets
 * the main loop run. So the handler counts arrivals per main-loop epoch and
 * restores the default action only at SYNC_STRIKES arrivals within one
 * epoch. A real fault then recurs and kills the process; sent signals are
 * recorded by kqueue, forwarded by the main loop, which starts a new epoch
 * and re-arms the handler. */
enum { SYNC_STRIKES = 8, SIG_SLOTS = 32 };

/* SYNC_STRIKES in the design; 1 reproduces revision 2 (the control). */
static volatile sig_atomic_t sync_strikes = SYNC_STRIKES;

static volatile sig_atomic_t loop_epoch;          /* written by the main loop */
static volatile sig_atomic_t hit_epoch[SIG_SLOTS]; /* written by the handler */
static volatile sig_atomic_t hit_count[SIG_SLOTS];

static void sync_handler(int sig)
{
	if (sig > 0 && sig < SIG_SLOTS) {
		sig_atomic_t epoch = loop_epoch;
		if (hit_epoch[sig] != epoch) {
			hit_epoch[sig] = epoch;
			hit_count[sig] = 1;
			return;
		}
		sig_atomic_t n = hit_count[sig] + 1;
		hit_count[sig] = n;
		if (n < sync_strikes)
			return;
	}
	struct sigaction d = {0};
	d.sa_handler = SIG_DFL;
	sigemptyset(&d.sa_mask);
	(void)sigaction(sig, &d, NULL);
}

static void arm_sync_handler(int sig)
{
	struct sigaction sa = {0};
	sa.sa_handler = sync_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, NULL) != 0)
		die("sigaction");
}

static void install_sync_handlers(void)
{
	for (size_t i = 0; i < SYNC_N; i++)
		arm_sync_handler(sync_set[i]);
}

/* Observation only (not part of the design): what siginfo says. */
static void observing_handler(int sig, siginfo_t *info, void *uctx)
{
	(void)uctx;
	char buf[128] = {0};
	int n = snprintf(buf, sizeof buf, "  siginfo: sig=%d si_code=%d si_pid=%d si_addr=%p\n",
			 sig, info->si_code, (int)info->si_pid, info->si_addr);
	if (n > 0 && (size_t)n < sizeof buf)
		(void)write(1, buf, (size_t)n);
	sync_handler(sig);
}

/* ---- harness: run a child mode, bounded ------------------------------- */

struct outcome {
	int finished;    /* 0: still running at the deadline (spin) */
	int status;      /* wait status when finished */
	double seconds;
};

static double now(void)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static struct outcome wait_bounded(pid_t pid, double limit)
{
	struct outcome o = {0};
	double start = now();
	for (;;) {
		int st = 0;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid) {
			o.finished = 1;
			o.status = st;
			break;
		}
		if (r < 0)
			die("waitpid");
		if (now() - start > limit) {
			kill(pid, SIGKILL);
			(void)waitpid(pid, &st, 0);
			break;
		}
		struct timespec ts = { 0, 5 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	o.seconds = now() - start;
	return o;
}

static void describe(struct outcome o, char *__counted_by(len) buf, size_t len)
{
	if (!o.finished)
		snprintf(buf, len, "still running after %.2fs (spin)", o.seconds);
	else if (WIFSIGNALED(o.status))
		snprintf(buf, len, "killed by signal %d%s in %.3fs", WTERMSIG(o.status),
			 WCOREDUMP(o.status) ? " (core)" : "", o.seconds);
	else
		snprintf(buf, len, "exited %d in %.3fs", WEXITSTATUS(o.status), o.seconds);
}

/* ---- experiment 1: the fork window ------------------------------------ */

/* One trial: returns 1 if the TERM sent right after fork was lost. */
static int fork_window_trial(int ignore_in_parent)
{
	sigset_t term = {0}, old = {0};
	sigemptyset(&term);
	sigaddset(&term, SIGTERM);
	set_action(SIGTERM, ignore_in_parent ? ACT_IGN : ACT_DFL);
	if (sigprocmask(SIG_BLOCK, &term, &old) != 0)
		die("sigprocmask");

	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		/* the child's restore sequence: dispositions, then the mask */
		struct sigaction d = {0};
		d.sa_handler = SIG_DFL;
		sigemptyset(&d.sa_mask);
		(void)sigaction(SIGTERM, &d, NULL);
		(void)sigprocmask(SIG_SETMASK, &old, NULL);
		execl("/bin/sleep", "sleep", "2", (char *)NULL);
		_exit(127);
	}
	kill(pid, SIGTERM);
	(void)sigprocmask(SIG_SETMASK, &old, NULL);
	set_action(SIGTERM, ACT_DFL);

	int st = 0;
	if (waitpid(pid, &st, 0) != pid)
		die("waitpid");
	return !(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

static void experiment1(int trials)
{
	char buf[128] = {0};
	int lost = 0, lost_control = 0;
	for (int i = 0; i < trials; i++)
		lost += fork_window_trial(0);
	for (int i = 0; i < trials; i++)
		lost_control += fork_window_trial(1);
	snprintf(buf, sizeof buf, "blocked default: %d/%d lost", lost, trials);
	report("exp1 fork window (design)", lost == 0, buf, sizeof buf);
	snprintf(buf, sizeof buf, "ignored: %d/%d lost", lost_control, trials);
	report("exp1 fork window (control shows loss)", lost_control > 0, buf, sizeof buf);
}

/* ---- experiment 2: kqueue records a blocked default signal ------------ */

static int64_t kq_count(int kq, int sig, double wait_s)
{
	struct kevent ev = {0};
	struct timespec ts = { (time_t)wait_s, (long)((wait_s - (double)(time_t)wait_s) * 1e9) };
	int n = kevent(kq, NULL, 0, &ev, 1, &ts);
	if (n < 0)
		die("kevent");
	if (n == 0 || (int)ev.ident != sig)
		return 0;
	return (int64_t)ev.data;
}

static void experiment2(void)
{
	char buf[160] = {0};
	sigset_t set = {0}, pend = {0};
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	set_action(SIGTERM, ACT_DFL);
	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
		die("sigprocmask");

	int kq = kqueue();
	if (kq < 0)
		die("kqueue");
	struct kevent reg = {0};
	EV_SET(&reg, SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	if (kevent(kq, &reg, 1, NULL, 0, NULL) != 0)
		die("kevent register");

	/* self-sent: recorded by the time kill returns */
	kill(getpid(), SIGTERM);
	int64_t self = kq_count(kq, SIGTERM, 0.0);
	snprintf(buf, sizeof buf, "kill(getpid) then kevent(timeout 0): data=%lld", (long long)self);
	report("exp2 self-sent recorded at once", self == 1, buf, sizeof buf);

	/* from outside: three sends from another process */
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		for (int i = 0; i < 3; i++)
			kill(getppid(), SIGTERM);
		_exit(0);
	}
	int st = 0;
	(void)waitpid(pid, &st, 0);
	int64_t ext = kq_count(kq, SIGTERM, 1.0);
	snprintf(buf, sizeof buf, "3 sends from another process: data=%lld", (long long)ext);
	report("exp2 external sends recorded with count", ext == 3, buf, sizeof buf);

	/* the signal stays pending while blocked; ignoring it discards it */
	sigpending(&pend);
	int was_pending = sigismember(&pend, SIGTERM);
	set_action(SIGTERM, ACT_IGN);
	sigpending(&pend);
	int still_pending = sigismember(&pend, SIGTERM);
	snprintf(buf, sizeof buf, "pending before=%d, after SIG_IGN=%d", was_pending, still_pending);
	report("exp2 SIG_IGN discards a pending signal", was_pending == 1 && still_pending == 0, buf, sizeof buf);

	set_action(SIGTERM, ACT_DFL);
	(void)sigprocmask(SIG_UNBLOCK, &set, NULL);
	close(kq);
}

/* ---- experiment 3: the group B handler -------------------------------- */

enum fault { F_NULL, F_BOUNDS, F_TRAP, F_ILL };

static int bounds_at(const int *__counted_by(n) p, size_t n, size_t i) { return p[i]; }

static void raise_fault(enum fault f)
{
	switch (f) {
	case F_NULL: {
		volatile int *__single p = __unsafe_forge_single(volatile int *, (uintptr_t)0);
		(void)*p;
		break;
	}
	case F_BOUNDS: {
		int a[3] = {1, 2, 3};
		volatile size_t i = 3;
		(void)bounds_at(a, 3, i);
		break;
	}
	case F_TRAP:
		__builtin_trap();
	case F_ILL:
		__asm__ volatile(".inst 0x00000000"); /* udf #0 */
		break;
	}
}

static struct outcome run_fault(enum fault f, int use_handler)
{
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		if (use_handler)
			install_sync_handlers();
		else
			for (size_t i = 0; i < SYNC_N; i++)
				set_action(sync_set[i], ACT_IGN);
		raise_fault(f);
		_exit(0);
	}
	return wait_bounded(pid, 2.0);
}

static const long double_gaps_us[] = { 0, 10, 100, 1000 };
static const long burst_gaps_us[] = { 0, 5, 20, 50, 200 };

/* The main loop under test: wait on kqueue, forward (here: count), and
 * re-arm the handler after every event. Exits 0 once `want` sends are
 * counted, 3 if they do not arrive within 2 s. */
static int main_loop(int sig, int want, int ready_fd)
{
	int kq = kqueue();
	if (kq < 0)
		return 20;
	struct kevent reg = {0};
	EV_SET(&reg, (uintptr_t)sig, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	if (kevent(kq, &reg, 1, NULL, 0, NULL) != 0)
		return 21;
	arm_sync_handler(sig);
	(void)write(ready_fd, "r", 1);
	int64_t got = 0;
	while (got < want) {
		struct kevent ev = {0};
		struct timespec ts = { 2, 0 };
		int n = kevent(kq, NULL, 0, &ev, 1, &ts);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 3;
		loop_epoch = loop_epoch + 1;
		got += (int64_t)ev.data;
		arm_sync_handler(sig);
	}
	return 0;
}

/* Send `count` signals to a child running main_loop, `gap_us` apart. */
static struct outcome run_sends(int sig, int count, long gap_us, int strikes)
{
	int ready[2] = {0};
	if (pipe(ready) != 0)
		die("pipe");
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		close(ready[0]);
		sync_strikes = strikes;
		_exit(main_loop(sig, count, ready[1]));
	}
	close(ready[1]);
	char r = 0;
	(void)read(ready[0], &r, 1);
	close(ready[0]);
	for (int k = 0; k < count; k++) {
		kill(pid, sig);
		if (gap_us > 0) {
			struct timespec ts = { 0, gap_us * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	return wait_bounded(pid, 5.0);
}

/* Observation: the siginfo of a sent SEGV and of a real one. */
static void observe_siginfo(void)
{
	for (int real = 0; real <= 1; real++) {
		int ready[2] = {0};
		if (pipe(ready) != 0)
			die("pipe");
		pid_t pid = fork();
		if (pid < 0)
			die("fork");
		if (pid == 0) {
			struct sigaction sa = {0};
			sa.sa_sigaction = observing_handler;
			sa.sa_flags = SA_SIGINFO;
			sigemptyset(&sa.sa_mask);
			(void)sigaction(SIGSEGV, &sa, NULL);
			printf("OBSERVED %s:\n", real ? "real NULL dereference" : "SEGV sent with kill");
			fflush(stdout);
			(void)write(ready[1], "r", 1);
			if (real)
				raise_fault(F_NULL);
			struct timespec ts = { 0, 300 * 1000 * 1000 };
			nanosleep(&ts, NULL);
			_exit(0);
		}
		close(ready[1]);
		char r = 0;
		(void)read(ready[0], &r, 1);
		close(ready[0]);
		if (!real)
			kill(pid, SIGSEGV);
		(void)wait_bounded(pid, 2.0);
	}
}

static void experiment3(void)
{
	observe_siginfo();
	char buf[160] = {0}, d[96] = {0};
	static const struct { enum fault f; int sig; const char *name; } cases[] = {
		{ F_NULL, SIGSEGV, "NULL dereference" },
		{ F_BOUNDS, SIGTRAP, "-fbounds-safety trap" },
		{ F_TRAP, SIGTRAP, "__builtin_trap" },
		{ F_ILL, SIGILL, "udf (illegal instruction)" },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct outcome o = run_fault(cases[i].f, 1);
		int ok = o.finished && WIFSIGNALED(o.status) && WTERMSIG(o.status) == cases[i].sig;
		describe(o, d, sizeof d);
		snprintf(buf, sizeof buf, "%s: %s", cases[i].name, d);
		report("exp3 real fault ends the process (design)", ok, buf, sizeof buf);
	}
	struct outcome c = run_fault(F_NULL, 0);
	describe(c, d, sizeof d);
	snprintf(buf, sizeof buf, "NULL dereference with SEGV ignored: %s", d);
	report("exp3 real fault (control shows spin)", !c.finished, buf, sizeof buf);

	/* the design: sent signals survived, each forwarded, handler re-armed */
	for (size_t g = 0; g < sizeof double_gaps_us / sizeof double_gaps_us[0]; g++) {
		int died = 0, other = 0;
		for (int t = 0; t < 1000; t++) {
			struct outcome o = run_sends(SIGSEGV, 2, double_gaps_us[g], SYNC_STRIKES);
			if (o.finished && WIFSIGNALED(o.status))
				died++;
			else if (!(o.finished && WIFEXITED(o.status) && WEXITSTATUS(o.status) == 0))
				other++;
		}
		snprintf(buf, sizeof buf, "2 SEGV sends %ld us apart: %d/1000 died, %d/1000 other",
			 double_gaps_us[g], died, other);
		report("exp3 repeated sends survived (design)", died == 0 && other == 0, buf, sizeof buf);
	}
	for (size_t i = 0; i < SYNC_N; i++) {
		struct outcome o = run_sends(sync_set[i], 2, 100, SYNC_STRIKES);
		int ok = o.finished && WIFEXITED(o.status) && WEXITSTATUS(o.status) == 0;
		describe(o, d, sizeof d);
		snprintf(buf, sizeof buf, "signal %d sent twice, 100 us apart: %s", sync_set[i], d);
		report("exp3 each synchronous signal survived (design)", ok, buf, sizeof buf);
	}
	/* bursts: the residual risk (ADR-0004 risk 4), design against control */
	int control_died = 0;
	for (int strikes = SYNC_STRIKES; strikes >= 1; strikes -= SYNC_STRIKES - 1) {
		for (size_t g = 0; g < sizeof burst_gaps_us / sizeof burst_gaps_us[0]; g++) {
			int died = 0, other = 0;
			for (int t = 0; t < 200; t++) {
				struct outcome o = run_sends(SIGSEGV, 20, burst_gaps_us[g], strikes);
				if (o.finished && WIFSIGNALED(o.status))
					died++;
				else if (!(o.finished && WIFEXITED(o.status) && WEXITSTATUS(o.status) == 0))
					other++;
			}
			if (strikes == 1)
				control_died += died;
			printf("MEASURED exp3 burst (%s): 20 SEGV sends %ld us apart: %d/200 died, %d/200 other\n",
			       strikes == 1 ? "control, strikes=1" : "design, strikes=8",
			       burst_gaps_us[g], died, other);
		}
	}
	snprintf(buf, sizeof buf, "strikes=1 died %d times over the bursts", control_died);
	report("exp3 bursts (control shows the rev. 2 failure)", control_died > 0, buf, sizeof buf);
}
/* ---- experiment 4: self-termination with other signals pending -------- */

static void block_forward_set(void)
{
	sigset_t set = {0};
	sigemptyset(&set);
	for (size_t i = 0; i < FORWARD_N; i++)
		if (!is_sync(forward_set[i])) {
			set_action(forward_set[i], ACT_DFL);
			sigaddset(&set, forward_set[i]);
		}
	install_sync_handlers();
	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
		die("sigprocmask");
}

/* The procedure under test (ADR-0002). */
static void self_terminate(int n)
{
	sigset_t pend = {0}, one = {0};
	sigpending(&pend);
	for (size_t i = 0; i < FORWARD_N; i++) {
		int s = forward_set[i];
		if (s != n && sigismember(&pend, s))
			set_action(s, ACT_IGN);
	}
	set_action(n, ACT_DFL);
	struct rlimit rl = {0};
	(void)setrlimit(RLIMIT_CORE, &rl);
	sigemptyset(&one);
	sigaddset(&one, n);
	(void)sigprocmask(SIG_UNBLOCK, &one, NULL);
	kill(getpid(), n);
	struct timespec ts = { 0, 200 * 1000 * 1000 };
	nanosleep(&ts, NULL);
	_exit(99);
}

/* Control: unblock everything instead, without discarding. */
static void naive_terminate(int n)
{
	set_action(n, ACT_DFL);
	sigset_t none = {0};
	sigemptyset(&none);
	(void)sigprocmask(SIG_SETMASK, &none, NULL);
	kill(getpid(), n);
	_exit(99);
}

static struct outcome run_terminate(int n, int others_pending, int n_pending, int naive)
{
	pid_t pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		block_forward_set();
		if (others_pending) {
			static const int others[] = { SIGHUP, SIGTERM, SIGUSR2 };
			for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
				if (others[i] != n)
					kill(getpid(), others[i]);
		}
		if (n_pending && !is_sync(n))
			kill(getpid(), n);
		if (naive)
			naive_terminate(n);
		self_terminate(n);
	}
	return wait_bounded(pid, 2.0);
}

static void experiment4(void)
{
	char buf[200] = {0}, d[96] = {0};
	int wrong = 0, total = 0;
	for (size_t i = 0; i < FORWARD_N; i++) {
		for (int np = 0; np <= 1; np++) {
			int n = forward_set[i];
			struct outcome o = run_terminate(n, 1, np, 0);
			total++;
			int ok = o.finished && WIFSIGNALED(o.status) && WTERMSIG(o.status) == n &&
				 !WCOREDUMP(o.status);
			if (!ok) {
				wrong++;
				describe(o, d, sizeof d);
				printf("  exp4 n=%d n_pending=%d: %s\n", n, np, d);
			}
		}
	}
	snprintf(buf, sizeof buf, "%d/%d cases ended by signal n (others pending, no core flag)",
		 total - wrong, total);
	report("exp4 self-termination (design)", wrong == 0, buf, sizeof buf);

	struct outcome c = run_terminate(SIGUSR1, 1, 0, 1);
	int control_wrong = !(c.finished && WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGUSR1);
	describe(c, d, sizeof d);
	snprintf(buf, sizeof buf, "unblock-all without discard, n=USR1 with HUP/TERM/USR2 pending: %s", d);
	report("exp4 self-termination (control shows pre-emption)", control_wrong, buf, sizeof buf);
}

int main(int argc, char *__null_terminated *__counted_by(argc) argv)
{
	int trials = 300;
	if (argc > 1)
		trials = atoi(argv[1]);
	setvbuf(stdout, NULL, _IOLBF, 0);
	experiment1(trials);
	experiment2();
	experiment3();
	experiment4();
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}

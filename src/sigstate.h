/*
 * sigstate.h — signal dispositions, the three groups, and the synchronous
 * handler (ADR-0002).
 */
#ifndef TIMEOUT_SIGSTATE_H
#define TIMEOUT_SIGSTATE_H

#include <signal.h>

/* Signals are 1..31 on macOS; index 0 is unused. */
enum { SIG_SLOTS = 32 };

/* Record every inherited disposition and the signal mask. Call first. */
int sig_record_inherited(void);

/* Whether sig was inherited as ignored. */
int sig_inherited_ignored(int sig);

/* Whether sig is in the forwarding set: default action terminate or core,
 * except SIGKILL and SIGSTOP. */
int sig_is_forwarded(int sig);

/* Whether sig is one of the five that can arise synchronously. */
int sig_is_synchronous(int sig);

/*
 * Set the parent's dispositions in the three groups and register an
 * EVFILT_SIGNAL on kq for every signal timeout receives (the forwarding set
 * minus the inherited-ignored, plus timeout_sig). Group A is blocked from
 * here on. Returns 0, or -1 with errno set.
 */
int sig_setup_parent(int kq, int timeout_sig);

/* Whether timeout receives (and forwards) sig. */
int sig_received(int sig);

/* Block group B as well, around fork; *saved gets the mask to restore. */
int sig_block_for_fork(sigset_t *saved);
int sig_unblock_after_fork(const sigset_t *saved);

/* In the forked child: restore every inherited disposition (timeout_sig to
 * default), then the inherited mask. */
void sig_restore_child(int timeout_sig);

/* The main loop starts a new epoch after every kevent return, and re-arms
 * the synchronous handler for sig after taking its event. */
void sig_new_epoch(void);
void sig_rearm(int sig);

/* Discard other pending signals, set sig to default, disable core files,
 * and unblock sig. Separate from sig_self_terminate so it can be tested. */
void sig_prepare_self_termination(int sig);

/* Terminate this process with sig, without a core file. */
_Noreturn void sig_self_terminate(int sig);

#endif

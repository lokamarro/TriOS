/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_ABI_H
#define UML_DARWIN_HOST_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_open(const char *path, int flags, int mode);
int darwin_uml_close(int fd);
int darwin_uml_fcntl(int fd, int command, long argument);
int darwin_uml_ioctl(int fd, unsigned long command, void *argument);
int darwin_uml_stop_fd(void);
int darwin_uml_stop_requested(void);
void darwin_uml_abort(void) __attribute__((noreturn));
void darwin_uml_exit(int status, void *caller) __attribute__((noreturn));

/* How the guest ended: the first argument of darwin_uml_guest_shutdown(). */
enum darwin_uml_shutdown_kind {
	DARWIN_UML_SHUTDOWN_POWER_OFF = 1,
	DARWIN_UML_SHUTDOWN_HALT = 2,
	DARWIN_UML_SHUTDOWN_RESTART = 3,
};

/*
 * The kernel says how the guest is ending, before it runs its exitcalls, so
 * the intent stays on record if an exitcall never returns. command is the
 * restart command the guest passed, or NULL; the host copies what it keeps.
 * A panic does not call this: it aborts the process.
 */
void darwin_uml_guest_shutdown(int kind, const char *command);

#ifdef __cplusplus
}
#endif

#endif

#ifndef KERNEL_PTRACE_H
#define KERNEL_PTRACE_H

#include "misc.h"

#define PTRACE_TRACEME_ 0
#define PTRACE_PEEKTEXT_ 1
#define PTRACE_PEEKDATA_ 2
#define PTRACE_PEEKUSER_ 3
#define PTRACE_POKETEXT_ 4
#define PTRACE_POKEDATA_ 5
#define PTRACE_CONT_ 7
#define PTRACE_KILL_ 8
#define PTRACE_SINGLESTEP_ 9
#define PTRACE_GETREGS_ 12
#define PTRACE_SETREGS_ 13
#define PTRACE_GETFPREGS_ 14
#define PTRACE_SETFPREGS_ 15
#define PTRACE_ATTACH_ 16
#define PTRACE_DETACH_ 17
#define PTRACE_SYSCALL_ 24
#define PTRACE_SETOPTIONS_ 0x4200
#define PTRACE_GETEVENTMSG_ 0x4201
#define PTRACE_GETSIGINFO_ 0x4202
#define PTRACE_SETSIGINFO_ 0x4203
#define PTRACE_GETREGSET_ 0x4204
#define PTRACE_SETREGSET_ 0x4205
#define PTRACE_SEIZE_ 0x4206
#define PTRACE_INTERRUPT_ 0x4207
#define PTRACE_LISTEN_ 0x4208

#define PTRACE_O_TRACESYSGOOD_ 0x1
#define PTRACE_O_TRACEFORK_ 0x2
#define PTRACE_O_TRACEVFORK_ 0x4
#define PTRACE_O_TRACECLONE_ 0x8
#define PTRACE_O_TRACEEXEC_ 0x10
#define PTRACE_O_TRACEVFORKDONE_ 0x20
#define PTRACE_O_TRACEEXIT_ 0x40
#define PTRACE_O_EXITKILL_ 0x100000
// Options implemented here (PTRACE_O_TRACESECCOMP and SUSPEND_SECCOMP are
// not: there is no seccomp; requesting them fails with EINVAL).
#define PTRACE_O_SUPPORTED_ (PTRACE_O_TRACESYSGOOD_ | PTRACE_O_TRACEFORK_ | PTRACE_O_TRACEVFORK_ | \
    PTRACE_O_TRACECLONE_ | PTRACE_O_TRACEEXEC_ | PTRACE_O_TRACEVFORKDONE_ | PTRACE_O_TRACEEXIT_ | PTRACE_O_EXITKILL_)

#define PTRACE_EVENT_FORK_ 1
#define PTRACE_EVENT_VFORK_ 2
#define PTRACE_EVENT_CLONE_ 3
#define PTRACE_EVENT_EXEC_ 4
#define PTRACE_EVENT_VFORK_DONE_ 5
#define PTRACE_EVENT_EXIT_ 6
#define PTRACE_EVENT_STOP_ 128
// siginfo code of the internal SIGTRAP that carries PTRACE_INTERRUPT to the
// tracee's next signal check; it becomes a PTRACE_EVENT_STOP, never a signal.
#define PTRACE_INTERRUPT_CODE_ 0x7e5cbeef

// ELF note types for PTRACE_GETREGSET/SETREGSET (arm64).
#define NT_PRSTATUS_ 1
#define NT_PRFPREG_ 2
#define NT_ARM_TLS_ 0x401
#define NT_ARM_SYSTEM_CALL_ 0x404

struct user_pt_regs_arm64_ {
    qword_t regs[31];
    qword_t sp;
    qword_t pc;
    qword_t pstate;
};
struct user_fpsimd_state_arm64_ {
    __uint128_t vregs[32];
    dword_t fpsr;
    dword_t fpcr;
    dword_t reserved[2];
};

// Stops the current (traced) task outside signal delivery and waits for the
// tracer (kernel/signal.c). status is reported through wait as status << 8 | 0x7f.
void ptrace_stop_current(int status, uint64_t event_msg);
// Syscall-entry and syscall-exit stops for PTRACE_SYSCALL (kernel/calls.c).
void ptrace_syscall_stop(void);

struct task;
// The task's tracer: its parent for TRACEME/ATTACH of a child, otherwise the
// tracer that auto-attached it (pids_lock).
struct task *ptrace_tracer(struct task *task);
// clone: attach the new task when the tracer asked to follow this kind of
// clone; returns the PTRACE_EVENT_* to report (0: none). pids_lock held.
int ptrace_attach_new_task(struct task *task, unsigned clone_flags);
// Undo ptrace_attach_new_task for a task that never started. pids_lock held.
void ptrace_forget_new_task(struct task *task);
// do_exit (pids_lock held): report our exit to a non-parent tracer, and
// detach or kill (PTRACE_O_EXITKILL) everything we trace.
void ptrace_exit_cleanup(int status);
// do_wait helpers (pids_lock held): a ptrace stop or exit report of a task
// traced by this thread that is not its child.
bool ptrace_wait_traced(struct task *tracer_thread, int idtype, pid_t_ id, struct siginfo_ *info, int options, bool *found_any);

struct user_regs_struct_ {
    dword_t ebx;
    dword_t ecx;
    dword_t edx;
    dword_t esi;
    dword_t edi;
    dword_t ebp;
    dword_t eax;
    dword_t xds;
    dword_t xes;
    dword_t xfs;
    dword_t xgs;
    dword_t orig_eax;
    dword_t eip;
    dword_t xcs;
    dword_t eflags;
    dword_t esp;
    dword_t xss;
};

struct user_fpregs_struct_ {
    dword_t cwd;
    dword_t swd;
    dword_t twd;
    dword_t fip;
    dword_t fcs;
    dword_t foo;
    dword_t fos;
    dword_t st_space[20];
};

struct user_ {
    struct user_regs_struct_ user_regs;
    char padding[286 - sizeof(struct user_regs_struct_)];
};

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, addr_t data);

#endif /* KERNEL_PTRACE_H */

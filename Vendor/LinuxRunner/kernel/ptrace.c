#include "ptrace.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "task.h"
#include <string.h>

#if !defined(GUEST_ARM64)
// Returns stopped child with the given pid, locked with the ptrace lock
static struct task *find_child(pid_t_ pid) {
    struct task *child = NULL;
    list_for_each_entry(&current->children, child, siblings) {
        if (child->pid == pid) {
            lock(&child->ptrace.lock);
            if (child->ptrace.stopped && task_may_inspect(child)) {
                goto found;
            }

            unlock(&child->ptrace.lock);
        }
    }
    child = NULL;
found:
    return child;
}
#endif


// ---- Tracer relationships (shared by all requests) -------------------------
struct task *ptrace_tracer(struct task *task) {
    return task->ptrace.tracer ? task->ptrace.tracer : task->parent;
}

// Children that lead their thread group report through the parent's children
// list; everything else (other processes' tasks, and threads, which the
// children scan skips) goes on the tracer's own list.
static void ptrace_link(struct task *tracee, struct task *tracer) {
    bool via_children = tracer == tracee->parent && task_is_leader(tracee);
    tracee->ptrace.tracer = via_children ? NULL : tracer;
    if (tracee->ptrace.tracer) list_add(&tracer->ptraced, &tracee->ptrace.tracer_link);
}

#define CSIGNAL_MASK_ 0xff
#define CLONE_VFORK_FLAG_ 0x4000
int ptrace_attach_new_task(struct task *task, unsigned clone_flags) {
    lock(&current->ptrace.lock);
    bool traced = current->ptrace.traced;
    unsigned options = current->ptrace.options;
    bool seized = current->ptrace.seized;
    unlock(&current->ptrace.lock);
    if (!traced) return 0;
    int event; unsigned needed;
    if (clone_flags & CLONE_VFORK_FLAG_) { event = PTRACE_EVENT_VFORK_; needed = PTRACE_O_TRACEVFORK_; }
    else if ((clone_flags & CSIGNAL_MASK_) == SIGCHLD_) { event = PTRACE_EVENT_FORK_; needed = PTRACE_O_TRACEFORK_; }
    else { event = PTRACE_EVENT_CLONE_; needed = PTRACE_O_TRACECLONE_; }
    if (!(options & needed)) return 0;
    struct task *tracer = ptrace_tracer(current);
    if (tracer == NULL) return 0;
    task->ptrace.traced = true;
    task->ptrace.options = options;
    task->ptrace.seized = seized;
    ptrace_link(task, tracer);
    // The new tracee stops before running user code: SIGSTOP, or a
    // PTRACE_EVENT_STOP for seized tracees.
    if (seized) send_signal(task, SIGTRAP_, (struct siginfo_) {.sig = SIGTRAP_, .code = PTRACE_INTERRUPT_CODE_});
    else send_signal(task, SIGSTOP_, (struct siginfo_) {.sig = SIGSTOP_, .code = SI_USER_});
    return event;
}

void ptrace_forget_new_task(struct task *task) {
    if (task->ptrace.tracer) list_remove_safe(&task->ptrace.tracer_link);
    task->ptrace.tracer = NULL;
    task->ptrace.traced = false;
}

#if defined(GUEST_ARM64)
// The tracee is stopped and its ptrace lock is held.
static void get_pt_regs(struct cpu_state *cpu, struct user_pt_regs_arm64_ *r) {
    memcpy(r->regs, cpu->regs, sizeof(r->regs));
    r->sp = cpu->sp;
    r->pc = cpu->pc;
    r->pstate = cpu->nzcv;   // EL0t, NZCV in bits 31..28, as in signal frames
}
static void set_pt_regs(struct cpu_state *cpu, const struct user_pt_regs_arm64_ *r) {
    memcpy(cpu->regs, r->regs, sizeof(r->regs));
    cpu->sp = r->sp;
    cpu->pc = r->pc;
    arm64_set_nzcv(cpu, (uint32_t) r->pstate & 0xf0000000u);
}
static void get_fpsimd(struct cpu_state *cpu, struct user_fpsimd_state_arm64_ *f) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 32; i++) f->vregs[i] = cpu->fp[i].q;
    f->fpsr = cpu->fpsr;
    f->fpcr = cpu->fpcr;
}
static void set_fpsimd(struct cpu_state *cpu, const struct user_fpsimd_state_arm64_ *f) {
    for (int i = 0; i < 32; i++) cpu->fp[i].q = f->vregs[i];
    cpu->fpsr = f->fpsr;
    cpu->fpcr = f->fpcr;
}

// PTRACE_GETREGSET/SETREGSET: addr is the note type, data an iovec whose
// length is updated to the bytes transferred.

static int ptrace_regset(struct task *child, bool set, addr_t type, addr_t iov_addr) {
    struct iovec_ iov;
    if (user_get(iov_addr, iov)) return _EFAULT;
    union {
        struct user_pt_regs_arm64_ regs;
        struct user_fpsimd_state_arm64_ fp;
        qword_t tls;
        int_t syscall;
    } u;
    size_t size;
    memset(&u, 0, sizeof(u));
    switch (type) {
        case NT_PRSTATUS_: size = sizeof(u.regs); get_pt_regs(&child->cpu, &u.regs); break;
        case NT_PRFPREG_: size = sizeof(u.fp); get_fpsimd(&child->cpu, &u.fp); break;
        case NT_ARM_TLS_: size = sizeof(u.tls); u.tls = child->cpu.tls_ptr; break;
        case NT_ARM_SYSTEM_CALL_:
            size = sizeof(u.syscall);
            u.syscall = child->ptrace.in_syscall ? (int_t) child->cpu.regs[8] : -1;
            break;
        default: return _EINVAL;   // hardware debug, SVE, PAC, MTE: not provided
    }
    size_t count = iov.len < size ? (size_t) iov.len : size;
    if (!set) {
        if (user_write(iov.base, &u, count)) return _EFAULT;
    } else {
        // A short write replaces only the leading registers (u holds the rest).
        if (user_read(iov.base, &u, count)) return _EFAULT;
        switch (type) {
            case NT_PRSTATUS_: set_pt_regs(&child->cpu, &u.regs); break;
            case NT_PRFPREG_: set_fpsimd(&child->cpu, &u.fp); break;
            case NT_ARM_TLS_: child->cpu.tls_ptr = u.tls; break;
            case NT_ARM_SYSTEM_CALL_: if (child->ptrace.in_syscall) child->cpu.regs[8] = (qword_t)(sqword_t) u.syscall; break;
        }
    }
    iov.len = count;
    if (user_put(iov_addr, iov)) return _EFAULT;
    return 0;
}

// Resumes a stopped tracee (lock held). sig is delivered on resume (0: none).
static int ptrace_resume(struct task *child, addr_t sig, bool syscall_trace, bool single_step) {
    if (sig >= NUM_SIGS) return _EIO;
    child->ptrace.inject = (int) sig;
    child->ptrace.syscall_trace = syscall_trace;
    child->cpu.single_step = single_step;
    child->ptrace.stopped = false;
    notify(&child->ptrace.cond);
    return 0;
}

// The tracer is the parent: PTRACE_TRACEME, or ATTACH/SEIZE of one's own child.
static struct task *find_tracee(pid_t_ pid, bool must_be_stopped) {
    struct task *child = NULL;
    lock(&pids_lock);
    list_for_each_entry(&current->ptraced, child, ptrace.tracer_link) {
        if (child->pid == pid) {
            lock(&child->ptrace.lock);
            unlock(&pids_lock);
            if (child->ptrace.traced && (!must_be_stopped || child->ptrace.stopped)) return child;
            unlock(&child->ptrace.lock);
            return NULL;
        }
    }
    unlock(&pids_lock);
    list_for_each_entry(&current->children, child, siblings) {
        if (child->pid == (pid_t_) pid) {
            lock(&child->ptrace.lock);
            // Permission was checked when tracing began (ATTACH/SEIZE) or
            // granted by the tracee (TRACEME); requests only need the bond.
            // A child traced by another process is not ours to control.
            if (child->ptrace.traced && child->ptrace.tracer == NULL &&
                    (!must_be_stopped || child->ptrace.stopped))
                return child;
            unlock(&child->ptrace.lock);
            return NULL;
        }
    }
    return NULL;
}

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, addr_t data) {
    pid = (dword_t) pid_arg((pid_t_) pid); // caller's PID namespace
    STRACE("ptrace(%#x, %d, %#llx, %#llx)", request, pid, (unsigned long long) addr, (unsigned long long) data);
    if (request == PTRACE_TRACEME_) {
        lock(&current->ptrace.lock);
        int err = current->ptrace.traced ? _EPERM : 0;
        if (!err) { current->ptrace.traced = true; current->ptrace.options = 0; }
        unlock(&current->ptrace.lock);
        return err;
    }
    if (request == PTRACE_ATTACH_ || request == PTRACE_SEIZE_) {
        // Any task the caller may inspect (same credentials and dumpable, or
        // root), as Linux without a Yama restriction: strace -p, gdb -p. The
        // tracer's own children report through its wait as their parent;
        // other tasks and threads go on its ptraced list (ptrace_link).
        lock(&pids_lock);
        struct task *found = pid_get_task(pid);
        int err = 0;
        if (!found) err = _ESRCH;
        else if (found->group == current->group) err = _EPERM;
        else {
            lock(&found->ptrace.lock);
            if (found->ptrace.traced) err = _EPERM;
            else if (!task_may_inspect(found)) err = _EPERM;
            else if (request == PTRACE_SEIZE_ && (data & ~(addr_t) PTRACE_O_SUPPORTED_)) err = _EINVAL;
            else {
                found->ptrace.traced = true;
                found->ptrace.seized = request == PTRACE_SEIZE_;
                found->ptrace.options = request == PTRACE_SEIZE_ ? (unsigned) data : 0;
                ptrace_link(found, current);
            }
            unlock(&found->ptrace.lock);
            if (!err && request == PTRACE_ATTACH_) send_signal(found, SIGSTOP_, SIGINFO_NIL);
        }
        unlock(&pids_lock);
        return err;
    }
    if (request == PTRACE_INTERRUPT_) {
        struct task *child = find_tracee(pid, false);   // takes pids_lock itself
        if (!child) return _ESRCH;
        int err = child->ptrace.seized ? 0 : _EIO;
        unlock(&child->ptrace.lock);
        if (!err) {
            lock(&pids_lock);
            send_signal(child, SIGTRAP_, (struct siginfo_) {.sig = SIGTRAP_, .code = PTRACE_INTERRUPT_CODE_});
            unlock(&pids_lock);
        }
        return err;
    }
    if (request == PTRACE_KILL_) {
        struct task *child = find_tracee(pid, false);
        if (!child) return _ESRCH;
        ptrace_resume(child, 0, false, false);
        unlock(&child->ptrace.lock);
        lock(&pids_lock);
        send_signal(child, SIGKILL_, SIGINFO_NIL);
        unlock(&pids_lock);
        return 0;
    }

    // Everything else needs a stopped tracee.
    struct task *child = find_tracee(pid, true);
    if (!child) return _ESRCH;
    int err = 0;
    bool detached = false;
    switch (request) {
        case PTRACE_PEEKTEXT_:
        case PTRACE_PEEKDATA_: {
            qword_t word;
            if (user_read_task(child, addr, &word, sizeof(word))) err = _EIO;
            else if (user_put(data, word)) err = _EFAULT;
            break;
        }
        case PTRACE_POKETEXT_:
        case PTRACE_POKEDATA_: {
            qword_t word = data;
            if (user_write_task_ptrace(child, addr, &word, sizeof(word))) err = _EIO;
            break;
        }
        case PTRACE_PEEKUSER_:
        case PTRACE_GETREGS_:
        case PTRACE_SETREGS_:
        case PTRACE_GETFPREGS_:
        case PTRACE_SETFPREGS_:
            err = _EIO;   // not defined on arm64; register sets are the interface
            break;
        case PTRACE_GETREGSET_: err = ptrace_regset(child, false, addr, data); break;
        case PTRACE_SETREGSET_: err = ptrace_regset(child, true, addr, data); break;
        case PTRACE_CONT_: err = ptrace_resume(child, data, false, false); break;
        case PTRACE_SYSCALL_: err = ptrace_resume(child, data, true, false); break;
        case PTRACE_SINGLESTEP_: err = ptrace_resume(child, data, false, true); break;
        case PTRACE_DETACH_:
            err = ptrace_resume(child, data, false, false);
            if (!err) { child->ptrace.traced = false; child->ptrace.seized = false; child->ptrace.options = 0; detached = true; }
            break;
        case PTRACE_SETOPTIONS_:
            if (data & ~(addr_t) PTRACE_O_SUPPORTED_) err = _EINVAL;
            else child->ptrace.options = (unsigned) data;
            break;
        case PTRACE_GETEVENTMSG_:
            if (user_put(data, child->ptrace.event_msg)) err = _EFAULT;
            break;
        case PTRACE_GETSIGINFO_:
            if (user_put(data, child->ptrace.info)) err = _EFAULT;
            break;
        case PTRACE_SETSIGINFO_:
            if (user_get(data, child->ptrace.info)) err = _EFAULT;
            break;
        default:
            err = _EIO;
    }
    unlock(&child->ptrace.lock);
    if (detached) {   // lock order: pids_lock before a task's ptrace lock
        lock(&pids_lock);
        ptrace_forget_new_task(child);
        unlock(&pids_lock);
    }
    return err;
}
#else
// Ensure stopped, ptrace locked, etc. before calling this
static void get_user_regs(struct cpu_state *cpu, struct user_regs_struct_ *user_regs_) {
    user_regs_->ebx = cpu->ebx;
    user_regs_->ecx = cpu->ecx;
    user_regs_->edx = cpu->edx;
    user_regs_->esi = cpu->esi;
    user_regs_->edi = cpu->edi;
    user_regs_->ebp = cpu->ebp;
    user_regs_->eax = cpu->eax;
//  user_regs_->xds = cpu->xds;
//  user_regs_->xes = cpu->xes;
//  user_regs_->xfs = cpu->xfs;
//  user_regs_->xgs = cpu->xgs;
    user_regs_->orig_eax = cpu->eax;
    user_regs_->eip = cpu->eip;
//  user_regs_->xcs = cpu->xcs;
    user_regs_->eflags = cpu->eflags;
    user_regs_->esp = cpu->esp;
//  user_regs_->xss = cpu->xss;
}

// Ensure stopped, ptrace locked, etc. before calling this
static void set_user_regs(struct cpu_state *cpu, struct user_regs_struct_ *user_regs_) {
    cpu->ebx = user_regs_->ebx;
    cpu->ecx = user_regs_->ecx;
    cpu->edx = user_regs_->edx;
    cpu->esi = user_regs_->esi;
    cpu->edi = user_regs_->edi;
    cpu->ebp = user_regs_->ebp;
    cpu->eax = user_regs_->eax;
//  cpu->xds = user_regs_->xds;
//  cpu->xes = user_regs_->xes;
//  cpu->xfs = user_regs_->xfs;
//  cpu->xgs = user_regs_->xgs;
//  cpu->eax = user_regs_->orig_eax;
    cpu->eip = user_regs_->eip;
//  cpu->xcs = user_regs_->xcs;
    cpu->eflags = user_regs_->eflags;
    cpu->esp = user_regs_->esp;
//  cpu->xss = user_regs_->xss;
}

dword_t sys_ptrace(dword_t request, dword_t pid, addr_t addr, addr_t data) {
    pid = (dword_t) pid_arg((pid_t_) pid); // caller's PID namespace
    switch (request) {
        case PTRACE_TRACEME_:
            STRACE("ptrace(PTRACE_TRACEME, %d, %#x, %#x)", pid, addr, data);
            current->ptrace.traced = true;
            return 0;

        case PTRACE_PEEKTEXT_:
        case PTRACE_PEEKDATA_: {
            STRACE("ptrace(PTRACE_PEEKDATA, %d, %#x, %#x)", pid, addr, data);
            dword_t peek;
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (user_get_task(child, addr, peek)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            } else if (user_put(data, peek)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_PEEKUSER_: {
            STRACE("ptrace(PTRACE_PEEKUSER, %d, %#x, %#x)", pid, addr, data);
            dword_t peek;
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_ user_ = {};
            get_user_regs(&child->cpu, &user_.user_regs);

            if (addr & (sizeof(peek) - 1) || addr >= sizeof(struct user_))
                return _EIO;

            memcpy(&peek, (char *)&user_ + addr, sizeof(peek));
            if (user_put(data, peek)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_POKETEXT_:
        case PTRACE_POKEDATA_: {
            STRACE("ptrace(PTRACE_POKEDATA, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (user_write_task_ptrace(child, addr, &data, sizeof(data))) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_CONT_: {
            STRACE("ptrace(PTRACE_CONT, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

#if !defined(GUEST_ARM64)
            child->cpu.tf = false;
#endif
            child->ptrace.stopped = false;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_KILL_: {
            STRACE("ptrace(PTRACE_KILL, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            child->ptrace.stopped = false;
            send_signal(child, SIGKILL_, SIGINFO_NIL);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SINGLESTEP_: {
            STRACE("ptrace(PTRACE_SINGLESTEP, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

#if !defined(GUEST_ARM64)
            child->cpu.tf = true;
#endif
            child->ptrace.stopped = false;
            notify(&child->ptrace.cond);
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_GETREGS_: {
            STRACE("ptrace(PTRACE_GETREGS, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_regs_struct_ user_regs_ = {};
            get_user_regs(&child->cpu, &user_regs_);
            if (user_put(data, user_regs_)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETREGS_: {
            STRACE("ptrace(PTRACE_SETREGS, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_regs_struct_ user_regs_;
            if (user_get(data, user_regs_)) {
                return _EFAULT;
            } else {
                set_user_regs(&child->cpu, &user_regs_);
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        // GDB needs the fpregs functions to exist if you want to evaluate things
        case PTRACE_GETFPREGS_: {
            STRACE("ptrace(PTRACE_GETFPREGS, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_fpregs_struct_ user_fpregs_ = {};
            if (user_put(data, user_fpregs_)) {
                unlock(&child->ptrace.lock);
                return _EFAULT;
            }
            // TODO get float point registers
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETFPREGS_: {
            STRACE("ptrace(PTRACE_SETFPREGS, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            struct user_fpregs_struct_ user_fpregs_;
            if (user_get(data, user_fpregs_)) {
                return _EFAULT;
            } else {
                // TODO set floating point registers
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        case PTRACE_SETOPTIONS_:
            STRACE("ptrace(PTRACE_SETOPTIONS, %d, %#x, %#x)", pid, addr, data);
            return _EINVAL;

        case PTRACE_GETSIGINFO_: {
            STRACE("ptrace(PTRACE_GETSIGINFO, %d, %#x, %#x)", pid, addr, data);
            struct task *child = find_child(pid);
            if (!child) return _EPERM;

            if (data && user_put(data, child->ptrace.info)) {
                return _EFAULT;
            }
            unlock(&child->ptrace.lock);

            return 0;
        }

        default:
            STRACE("ptrace(%d, %d, %#x, %#x)", request, pid, addr, data);
            return _EPERM;
    }
}
#endif

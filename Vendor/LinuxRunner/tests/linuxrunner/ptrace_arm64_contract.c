#define _GNU_SOURCE
// arm64 ptrace as gdb and strace use it: TRACEME + exec stop, GETREGSET
// (general, FP/SIMD, TLS, system call), 64-bit PEEK/POKE, a software
// breakpoint (BRK) hit and stepped over, PTRACE_SINGLESTEP, PTRACE_SYSCALL
// stops with TRACESYSGOOD, skipping a syscall, signal suppression and
// injection, unsupported options rejected, /proc/<pid>/mem for the tracer,
// and PTRACE_SEIZE/INTERRUPT of a child. Runs as a normal user when started
// as root.
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static volatile int usr1_seen;
static void on_usr1(int sig) { (void) sig; usr1_seen++; }

// Target code for breakpoints and stepping: three plain instructions.
__attribute__((noinline, used)) int target_function(int x) {
    __asm__ volatile("" ::: "memory");
    return x * 3 + 1;
}

static int get_regs(pid_t pid, struct user_pt_regs *r) {
    struct iovec iov = {r, sizeof(*r)};
    return (int) ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRSTATUS, &iov);
}
static int set_regs(pid_t pid, struct user_pt_regs *r) {
    struct iovec iov = {r, sizeof(*r)};
    return (int) ptrace(PTRACE_SETREGSET, pid, (void *) NT_PRSTATUS, &iov);
}
static int wait_stop(pid_t pid, int *status) {
    pid_t r = waitpid(pid, status, __WALL);
    return r == pid && WIFSTOPPED(*status);
}

static void child_body(void) {
    signal(SIGUSR1, on_usr1);
    raise(SIGSTOP);                         // stop 1: tracer inspects us
    int v = target_function(4);             // breakpoint inside
    syscall(SYS_getppid);                   // syscall stops around this
    errno = 0;
    long skipped = syscall(SYS_getpid) == -1 && errno == ENOSYS ? -ENOSYS : 0;  // tracer skips this one
    raise(SIGUSR2);                         // tracer suppresses it
    raise(SIGUSR1);                         // tracer re-injects SIGUSR1
    _exit(v == 13 && skipped == -ENOSYS && usr1_seen == 1 ? 0 : 20 + (v != 13) + 2 * (skipped != -ENOSYS) + 4 * (usr1_seen != 1));
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() == 0 && (setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000))) { perror("drop"); return 2; }
    // Dropping from root clears dumpability, as on Linux; a user's debugger is dumpable.
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

    pid_t pid = fork();
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(10);
        child_body();
    }
    int status = 0;
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == SIGSTOP, "first stop status=%#x", status);

    // Unsupported options are refused (there is no seccomp).
    errno = 0;
    CHECK(ptrace(PTRACE_SETOPTIONS, pid, 0, (void *) PTRACE_O_TRACESECCOMP) == -1 && errno == EINVAL, "TRACESECCOMP must be EINVAL");
    CHECK(ptrace(PTRACE_SETOPTIONS, pid, 0, (void *) (PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL)) == 0, "SETOPTIONS sysgood|exitkill");

    struct user_pt_regs regs;
    CHECK(get_regs(pid, &regs) == 0 && regs.pc != 0 && regs.sp != 0, "GETREGSET pc=%#llx sp=%#llx", (unsigned long long) regs.pc, (unsigned long long) regs.sp);
    struct user_fpsimd_state fp; struct iovec fiov = {&fp, sizeof(fp)};
    CHECK(ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRFPREG, &fiov) == 0 && fiov.iov_len == sizeof(fp), "NT_PRFPREG len=%zu", fiov.iov_len);
    uint64_t tls = 0; struct iovec tiov = {&tls, sizeof(tls)};
    CHECK(ptrace(PTRACE_GETREGSET, pid, (void *) NT_ARM_TLS, &tiov) == 0 && tls != 0, "NT_ARM_TLS %#llx", (unsigned long long) tls);
    CHECK(ptrace(12 /* PTRACE_GETREGS */, pid, 0, &regs) == -1, "PTRACE_GETREGS is not an arm64 request");

    // 64-bit PEEK/POKE round trip on the target's text, through /proc too.
    uintptr_t bp = (uintptr_t) &target_function;
    errno = 0;
    long original = ptrace(PTRACE_PEEKTEXT, pid, (void *) bp, 0);
    CHECK(errno == 0 && original == *(long *) bp, "PEEKTEXT %#lx vs %#lx", original, *(long *) bp);
    char path[64]; snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int mem = open(path, O_RDWR);
    CHECK(mem >= 0, "open %s as tracer", path);
    long via_mem = 0;
    CHECK(mem < 0 || (pread(mem, &via_mem, 8, (off_t) bp) == 8 && via_mem == original), "/proc/pid/mem read");
    if (mem >= 0) close(mem);

    // Software breakpoint: BRK #0 at target_function's first instruction.
    long patched = (original & ~0xffffffffL) | 0xd4200000L;
    CHECK(ptrace(PTRACE_POKETEXT, pid, (void *) bp, (void *) patched) == 0, "POKETEXT breakpoint");
    CHECK(ptrace(PTRACE_CONT, pid, 0, 0) == 0, "CONT to breakpoint");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == SIGTRAP, "breakpoint stop status=%#x", status);
    CHECK(get_regs(pid, &regs) == 0 && regs.pc == bp, "stopped at breakpoint pc=%#llx want %#lx", (unsigned long long) regs.pc, (unsigned long) bp);
    CHECK(regs.regs[0] == 4, "argument x0=%llu", (unsigned long long) regs.regs[0]);
    // Restore, single-step one instruction, continue.
    CHECK(ptrace(PTRACE_POKETEXT, pid, (void *) bp, (void *) original) == 0, "restore instruction");
    CHECK(ptrace(PTRACE_SINGLESTEP, pid, 0, 0) == 0, "SINGLESTEP");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == SIGTRAP, "step stop status=%#x", status);
    struct user_pt_regs after;
    CHECK(get_regs(pid, &after) == 0 && after.pc == bp + 4, "single step pc=%#llx want %#lx", (unsigned long long) after.pc, (unsigned long) bp + 4);

    // Syscall stops: getppid entry (x8 = 173) and exit (x0 = our pid).
    CHECK(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0, "SYSCALL");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == (SIGTRAP | 0x80), "syscall entry status=%#x", status);
    CHECK(get_regs(pid, &regs) == 0 && regs.regs[8] == SYS_getppid, "entry x8=%llu", (unsigned long long) regs.regs[8]);
    int nr = 0; struct iovec siov = {&nr, sizeof(nr)};
    CHECK(ptrace(PTRACE_GETREGSET, pid, (void *) NT_ARM_SYSTEM_CALL, &siov) == 0 && nr == SYS_getppid, "NT_ARM_SYSTEM_CALL %d", nr);
    CHECK(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0, "SYSCALL to exit");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == (SIGTRAP | 0x80), "syscall exit status=%#x", status);
    CHECK(get_regs(pid, &regs) == 0 && (pid_t) regs.regs[0] == getpid(), "exit x0=%lld want %d", (long long) regs.regs[0], getpid());

    // Skip getpid: set the number to -1 at entry and the result in x0.
    CHECK(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0 && wait_stop(pid, &status), "getpid entry");
    nr = -1;
    CHECK(ptrace(PTRACE_SETREGSET, pid, (void *) NT_ARM_SYSTEM_CALL, &siov) == 0, "set syscall -1");
    CHECK(get_regs(pid, &regs) == 0, "regs at skip");
    regs.regs[0] = (uint64_t) -ENOSYS;
    CHECK(set_regs(pid, &regs) == 0, "SETREGSET result");
    CHECK(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0 && wait_stop(pid, &status), "skipped exit stop");

    // SIGUSR2: suppressed by resuming with 0. SIGUSR1: delivered by resuming with it.
    CHECK(ptrace(PTRACE_CONT, pid, 0, 0) == 0, "CONT");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == SIGUSR2, "SIGUSR2 stop status=%#x", status);
    siginfo_t si;
    CHECK(ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0 && si.si_signo == SIGUSR2, "GETSIGINFO signo=%d", si.si_signo);
    CHECK(ptrace(PTRACE_CONT, pid, 0, 0) == 0, "suppress SIGUSR2");
    CHECK(wait_stop(pid, &status) && WSTOPSIG(status) == SIGUSR1, "SIGUSR1 stop status=%#x", status);
    CHECK(ptrace(PTRACE_CONT, pid, 0, (void *) SIGUSR1) == 0, "inject SIGUSR1");
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "tracee result status=%#x", status);

    // PTRACE_SEIZE + PTRACE_INTERRUPT of an untraced child, then KILL.
    pid_t sleeper = fork();
    if (sleeper == 0) { for (;;) pause(); }
    CHECK(ptrace(PTRACE_SEIZE, sleeper, 0, (void *) PTRACE_O_TRACESYSGOOD) == 0, "SEIZE");
    CHECK(ptrace(PTRACE_INTERRUPT, sleeper, 0, 0) == 0, "INTERRUPT");
    CHECK(wait_stop(sleeper, &status), "interrupt stop status=%#x", status);
    CHECK(ptrace(PTRACE_KILL, sleeper, 0, 0) == 0, "KILL");
    CHECK(waitpid(sleeper, &status, 0) == sleeper && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "killed status=%#x", status);

    // Follow fork/vfork/exec as strace -f does: SEIZE a stopped child with
    // event options, INTERRUPT, SIGCONT, then see every event in order.
    pid_t shell = fork();
    if (shell == 0) { kill(getpid(), SIGSTOP); execl("/bin/sh", "sh", "-c", "/bin/true; exit 3", (char *) NULL); _exit(9); }
    CHECK(waitpid(shell, &status, WSTOPPED) == shell && WIFSTOPPED(status), "shell self-stop status=%#x", status);
    long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
                   PTRACE_O_TRACEEXEC | PTRACE_O_TRACEVFORKDONE | PTRACE_O_TRACEEXIT;
    CHECK(ptrace(PTRACE_SEIZE, shell, 0, (void *) options) == 0, "SEIZE with follow options");
    CHECK(ptrace(PTRACE_INTERRUPT, shell, 0, 0) == 0, "INTERRUPT shell");
    kill(shell, SIGCONT);
    int forks = 0, event_stops = 0, execs = 0, exit_events = 0, grandchild_exited = 0, shell_code = -1;
    unsigned long new_pid = 0;
    for (int i = 0; i < 40; i++) {
        pid_t w = waitpid(-1, &status, __WALL);
        if (w <= 0) break;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (w == shell) { shell_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); break; }
            if ((unsigned long) w == new_pid) grandchild_exited = 1;
            continue;
        }
        int event = status >> 16, sig = WSTOPSIG(status);
        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            forks++; ptrace(PTRACE_GETEVENTMSG, w, 0, &new_pid);
        } else if (event == PTRACE_EVENT_STOP) event_stops++;
        else if (event == PTRACE_EVENT_EXEC) execs++;
        else if (event == PTRACE_EVENT_EXIT) exit_events++;
        int deliver = event || sig == SIGTRAP || sig == (SIGTRAP | 0x80) || sig == SIGSTOP ? 0 : sig;
        ptrace(PTRACE_CONT, w, 0, (void *) (long) deliver);
    }
    CHECK(forks >= 1 && new_pid > 0, "fork events %d new pid %lu", forks, new_pid);
    CHECK(event_stops >= 2, "PTRACE_EVENT_STOP for interrupt and new child: %d", event_stops);
    CHECK(execs >= 2, "exec events %d (shell and /bin/true)", execs);
    CHECK(exit_events >= 2, "exit events %d", exit_events);
    CHECK(grandchild_exited, "auto-attached grandchild exit reported to tracer");
    CHECK(shell_code == 3, "shell exit code %d", shell_code);

    printf("ptrace_arm64_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

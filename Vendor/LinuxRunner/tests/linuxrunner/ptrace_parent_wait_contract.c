// ptrace stops are reported to the tracer only. Shape of `strace -f` on a
// launcher (LibreOffice's oosplash): the tracer T traces the launcher C with
// PTRACE_O_TRACEFORK, C forks G (auto-attached to T), T runs G through
// syscall-stops, and C blocks in waitpid(G) like any real parent. C must see
// only G's exit; a leaked syscall-stop made oosplash give up on its child.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int launcher(void) {
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) { perror("TRACEME"); return 90; }
    raise(SIGSTOP);
    pid_t g = fork();
    if (g == 0) {
        for (int i = 0; i < 300; i++) syscall(SYS_getpid);
        usleep(20000);
        _exit(7);
    }
    if (g < 0) return 91;
    int status = 0, reports = 0;
    for (;;) {
        pid_t r = waitpid(g, &status, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r != g) { printf("launcher: waitpid returned %d errno=%d\n", r, errno); return 92; }
        reports++;
        if (!WIFEXITED(status)) { printf("launcher: saw non-exit status %#x for its traced child\n", status); return 93; }
        break;
    }
    if (WEXITSTATUS(status) != 7) { printf("launcher: child exit %d\n", WEXITSTATUS(status)); return 94; }
    // WNOWAIT on a reaped child: nothing left.
    siginfo_t si = {0};
    if (waitid(P_PID, g, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == g) return 95;
    return reports == 1 ? 0 : 96;
}

int main(void) {
    alarm(60);
    pid_t c = fork();
    if (c == 0) _exit(launcher());
    int status;
    if (waitpid(c, &status, 0) != c || !WIFSTOPPED(status)) { printf("FAIL initial stop %#x\n", status); return 1; }
    if (ptrace(PTRACE_SETOPTIONS, c, 0, PTRACE_O_TRACEFORK | PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) != 0) { perror("SETOPTIONS"); return 1; }
    ptrace(PTRACE_CONT, c, 0, 0);
    pid_t g = 0; int syscall_stops = 0, launcher_code = -1, g_exit = -1, wnowait_checked = 0;
    for (;;) {
        pid_t p = waitpid(-1, &status, __WALL);
        if (p < 0) { if (errno == EINTR) continue; if (errno == ECHILD) break; perror("waitpid"); return 1; }
        if (p == c) {
            if (WIFEXITED(status)) { launcher_code = WEXITSTATUS(status); break; }
            if (WIFSIGNALED(status)) { printf("FAIL launcher killed by %d\n", WTERMSIG(status)); return 1; }
            if (WIFSTOPPED(status) && (status >> 8) == (SIGTRAP | PTRACE_EVENT_FORK << 8)) {
                unsigned long msg = 0; ptrace(PTRACE_GETEVENTMSG, c, 0, &msg); g = (pid_t) msg;
            }
            int sig = WIFSTOPPED(status) && WSTOPSIG(status) != SIGTRAP && WSTOPSIG(status) != SIGSTOP ? WSTOPSIG(status) : 0;
            ptrace(PTRACE_CONT, c, 0, sig);
            continue;
        }
        if (g == 0) g = p;   // auto-attach stop may arrive before the fork event
        if (p != g) { printf("FAIL unexpected pid %d\n", p); return 1; }
        if (WIFEXITED(status)) { g_exit = WEXITSTATUS(status); continue; }
        if (!WIFSTOPPED(status)) continue;
        if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
            syscall_stops++;
            if (!wnowait_checked) {
                // The tracer can peek at the stop with WNOWAIT and still collect it.
                siginfo_t si = {0};
                wnowait_checked = waitid(P_PID, g, &si, WEXITED | WSTOPPED | WNOHANG | WNOWAIT | __WALL) == 0 ? 1 : -1;
            }
        }
        ptrace(PTRACE_SYSCALL, g, 0, 0);
        // Leave a few stops uncollected for longer than the launcher's wait
        // recheck interval, as a slow tracer does under load.
        if (syscall_stops == 10 || syscall_stops == 100 || syscall_stops == 200) usleep(1500000);
    }
    int ok = launcher_code == 0 && syscall_stops >= 300 && g_exit == 7;
    printf("PTRACE_PARENT_WAIT_CONTRACT %s launcher=%d syscall_stops=%d grandchild_exit=%d\n",
           ok ? "PASS" : "FAIL", launcher_code, syscall_stops, g_exit);
    return ok ? 0 : 1;
}

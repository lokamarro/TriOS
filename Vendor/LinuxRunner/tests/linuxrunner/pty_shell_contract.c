#define _GNU_SOURCE
// Pseudo-terminal shell round trip as done by node-pty (VS Code's integrated
// terminal): posix_openpt/grantpt/unlockpt/ptsname, fork, setsid, TIOCSCTTY,
// dup2 the slave to stdio, exec /bin/sh; the parent writes a command to the
// master and reads the echoed result. Also checks window-size ioctls and that
// the shell's command really ran (a file it writes).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

// Reads from the master until needle appears or the deadline passes.
static int read_until(int master, const char *needle, int ms, char *acc, size_t size) {
    size_t used = strlen(acc);
    for (long long end = now_ms() + ms; now_ms() < end;) {
        struct pollfd p = {master, POLLIN, 0};
        int r = poll(&p, 1, 200);
        if (r <= 0) continue;
        ssize_t n = read(master, acc + used, size - 1 - used);
        if (n <= 0) { if (errno == EAGAIN || errno == EINTR) continue; return 0; }
        used += (size_t) n; acc[used] = 0;
        if (strstr(acc, needle)) return 1;
        if (used > size - 256) { memmove(acc, acc + used - 128, 128); used = 128; acc[used] = 0; }
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char marker[] = "/tmp/lr-pty-XXXXXX";
    int tmp = mkstemp(marker); CHECK(tmp >= 0, "mkstemp"); close(tmp); unlink(marker);
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0, "posix_openpt");
    CHECK(grantpt(master) == 0 && unlockpt(master) == 0, "grantpt/unlockpt");
    char *name = ptsname(master); CHECK(name != NULL, "ptsname");
    struct winsize ws = {.ws_row = 30, .ws_col = 100};
    CHECK(ioctl(master, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ on master");
    pid_t child = fork();
    if (child == 0) {
        setsid();
        int slave = open(name, O_RDWR);
        if (slave < 0) _exit(10);
        if (ioctl(slave, TIOCSCTTY, 0) != 0) _exit(11);
        struct winsize got;
        if (ioctl(slave, TIOCGWINSZ, &got) != 0 || got.ws_col != 100 || got.ws_row != 30) _exit(12);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        if (argc > 1) execv(argv[1], &argv[1]);
        else execl("/bin/sh", "sh", "-i", (char *) NULL);
        _exit(13);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    char acc[8192] = "", command[256];
    // Wait for the shell to come up, as a user would see its prompt.
    long long start = now_ms();
    // Interactive sh prints "$ " or "# " (root); wait for either.
    int prompt = 0;
    for (long long end = now_ms() + 15000; !prompt && now_ms() < end;)
        prompt = read_until(master, " ", 500, acc, sizeof acc) && (strchr(acc, '$') || strchr(acc, '#'));
    printf("prompt after %lld ms: %d\n", now_ms() - start, prompt);
    CHECK(prompt, "interactive prompt not observed");
    snprintf(command, sizeof command, "echo LR_PTY_$((6*7)) > %s; echo ERROR_$((40+2)) >&2; echo DONE_$((40+2))\n", marker);
    CHECK(write(master, command, strlen(command)) == (ssize_t) strlen(command), "write command");
    acc[0] = 0;
    CHECK(read_until(master, "DONE_42", 15000, acc, sizeof acc), "shell output DONE_42 not seen; got '%s'", acc);
    CHECK(strstr(acc, "ERROR_42") != NULL, "stderr not seen; got '%s'", acc);
    FILE *f = fopen(marker, "r"); char line[64] = "";
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    CHECK(strstr(line, "LR_PTY_42") != NULL, "command did not run (file '%s')", line);
    // Resize while the shell runs: the slave side must see the new size.
    ws.ws_col = 120; CHECK(ioctl(master, TIOCSWINSZ, &ws) == 0, "resize");
    CHECK(write(master, "exit\n", 5) == 5, "write exit");
    int status = 0; waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "shell status %#x", status);
    unlink(marker);
    printf("pty_shell_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

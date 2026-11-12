// Guest-side VS Code session driver (X11 XTest; the fixture has libXtst.so.6
// but no development header, so the one entry point is declared here).
// Drives the original editor through ordinary keyboard shortcuts and checks
// each step independently of the screen: window title (active editor), files
// on disk, a command run in the integrated terminal, and the process list.
//   vscode_session_driver <workspace-dir> <code-pid> [--reopen]
// Prints "PHASE <name> ok=<0|1> mono_ms=<n> <detail>" lines (CLOCK_MONOTONIC,
// comparable with the runtime's FD_TREND mono_ms) and exits 0 only when every
// phase passed. Never touches sandbox flags or the user's real profile.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int XTestFakeKeyEvent(Display *, unsigned int, Bool, unsigned long);
Bool XTestQueryExtension(Display *, int *, int *, int *, int *);
static int fake_failures;

static Display *dpy;
static const char *workspace;
static pid_t code_pid;
static int failures;

static long long mono_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void phase(const char *name, int ok, const char *detail) {
    if (!ok) failures++;
    printf("PHASE %s ok=%d mono_ms=%lld %s\n", name, ok, mono_ms(), detail ? detail : "");
    fflush(stdout);
}

static int window_title(Window w, char *out, size_t size) {
    Atom name = XInternAtom(dpy, "_NET_WM_NAME", False), utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom type; int format; unsigned long count, after; unsigned char *data = NULL;
    out[0] = 0;
    if (XGetWindowProperty(dpy, w, name, 0, 1024, False, utf8, &type, &format, &count, &after, &data) == Success && data) {
        snprintf(out, size, "%s", (char *) data); XFree(data); return 1;
    }
    char *legacy = NULL;
    if (XFetchName(dpy, w, &legacy) && legacy) { snprintf(out, size, "%s", legacy); XFree(legacy); return 1; }
    return 0;
}
// Finds the top-level window whose title names Visual Studio Code.
static Window find_code(Window w, int depth, char *title, size_t size) {
    if (window_title(w, title, size) && strstr(title, "Visual Studio Code")) return w;
    if (depth >= 3) return 0;
    Window root, parent, *kids = NULL; unsigned n = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &n)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < n && !found; i++) found = find_code(kids[i], depth + 1, title, size);
    if (kids) XFree(kids);
    return found;
}
static Window code_window(char *title, size_t size) { return find_code(DefaultRootWindow(dpy), 0, title, size); }

// Waits until the editor's title contains needle (and not reject, if given).
static int wait_title(const char *needle, int seconds, char *title, size_t size) {
    for (long long end = mono_ms() + seconds * 1000LL; mono_ms() < end; sleep_ms(500)) {
        if (code_window(title, size) && strstr(title, needle)) return 1;
    }
    return 0;
}
int XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
int XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
// Focuses the editor as a user would: pointer into the editor text area and a
// click. Focus itself stays with the session's focus manager (WM_TAKE_FOCUS);
// setting X input focus directly on a Chromium toplevel bypasses Chromium's own
// focus handling and its key events are then dropped.
static void focus(void) {
    char title[512]; Window w = code_window(title, sizeof title), child;
    int x = 0, y = 0;
    if (w) XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 300, 100, &x, &y, &child);
    XTestFakeMotionEvent(dpy, -1, x, y, 0);
    XTestFakeButtonEvent(dpy, 1, True, 0); XTestFakeButtonEvent(dpy, 1, False, 0);
    XSync(dpy, False); sleep_ms(1500);
    Window focused; int revert; XGetInputFocus(dpy, &focused, &revert);
    printf("FOCUS window=%#lx click=%d,%d focused=%#lx title='%s'\n", w, x, y, focused, title);
}

static int needs_shift(KeySym ks, KeyCode kc) {
    int per = 0; KeySym *map = XGetKeyboardMapping(dpy, kc, 1, &per);
    int shift = map && per > 1 && map[0] != ks && map[1] == ks;
    if (map) XFree(map);
    return shift;
}
static void press(KeySym ks, int ctrl) {
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) { fprintf(stderr, "no keycode for keysym %#lx\n", ks); return; }
    int shift = needs_shift(ks, kc);
    KeyCode control = XKeysymToKeycode(dpy, XK_Control_L), shift_key = XKeysymToKeycode(dpy, XK_Shift_L);
    if (ctrl) XTestFakeKeyEvent(dpy, control, True, 0);
    if (shift) XTestFakeKeyEvent(dpy, shift_key, True, 0);
    if (!XTestFakeKeyEvent(dpy, kc, True, 0) || !XTestFakeKeyEvent(dpy, kc, False, 0)) fake_failures++;
    if (shift) XTestFakeKeyEvent(dpy, shift_key, False, 0);
    if (ctrl) XTestFakeKeyEvent(dpy, control, False, 0);
    XSync(dpy, False);
    sleep_ms(40);
}
static void type_text(const char *s) { for (; *s; s++) press(*s == '\n' ? XK_Return : (KeySym)(unsigned char) *s, 0); }
static void chord(KeySym ks) { press(ks, 1); }

static int file_contains(const char *path, const char *needle, int seconds) {
    for (long long end = mono_ms() + seconds * 1000LL; mono_ms() < end; sleep_ms(500)) {
        FILE *f = fopen(path, "r"); if (!f) continue;
        char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        if (strstr(buf, needle)) return 1;
    }
    return 0;
}
// Counts live processes whose command line contains needle.
static int count_processes(const char *needle) {
    DIR *d = opendir("/proc"); if (!d) return -1;
    int count = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char) e->d_name[0])) continue;
        char path[300], buf[4096];
        snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
        FILE *f = fopen(path, "r"); if (!f) continue;
        int tgid = -1; while (fgets(buf, sizeof buf, f)) if (sscanf(buf, "Tgid: %d", &tgid) == 1) break;
        fclose(f);
        if (tgid != atoi(e->d_name)) continue;   // threads
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
        buf[n] = 0;
        if (strstr(buf, needle)) count++;
    }
    closedir(d);
    return count;
}
static int code_alive(void) { return kill(code_pid, 0) == 0; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s workspace code-pid [--reopen]\n", argv[0]); return 2; }
    workspace = argv[1]; code_pid = (pid_t) atoi(argv[2]);
    int reopen = argc > 3 && !strcmp(argv[3], "--reopen");
    char a_path[512], b_path[512], term_path[512], title[512], detail[700];
    snprintf(a_path, sizeof a_path, "%s/a.txt", workspace);
    snprintf(b_path, sizeof b_path, "%s/b.txt", workspace);
    snprintf(term_path, sizeof term_path, "%s/terminal-ok.txt", workspace);
    for (int i = 0; i < 120 && !(dpy = XOpenDisplay(NULL)); i++) sleep_ms(500);
    if (!dpy) { phase("display", 0, "XOpenDisplay failed"); return 1; }
    int ev, er, major, minor;
    if (!XTestQueryExtension(dpy, &ev, &er, &major, &minor)) { phase("xtest", 0, "XTEST extension missing"); return 1; }
    printf("XTEST %d.%d\n", major, minor);
    if (argc > 3 && !strcmp(argv[3], "--type-test")) {
        // Self-test against any focused client (e.g. xterm running cat > file).
        sleep_ms(8000); type_text(argv[4 < argc ? 4 : 3]); type_text("\n");
        printf("TYPE_TEST fake_failures=%d\n", fake_failures); XCloseDisplay(dpy); return fake_failures != 0;
    }

    if (reopen) {
        // Second launch: the saved edits must be on disk and the editor must open them.
        // Hot exit restores the session, so the active editor may be the
        // restored untitled file rather than a.txt.
        int ok = wait_title(" - Visual Studio Code", 300, title, sizeof title) &&
                 (strstr(title, "a.txt") || strstr(title, "Untitled-1"));
        snprintf(detail, sizeof detail, "title='%s'", title); phase("reopen_window", ok, detail);
        sleep_ms(10000);
        phase("reopen_contents", file_contains(a_path, "LR_EDIT_OK", 1) && file_contains(b_path, "LR_TAB2_OK", 1), "");
        focus(); chord(XK_q);
        int exited = 0;
        for (int i = 0; i < 40 && !exited; i++) { exited = !code_alive(); if (!exited) sleep_ms(500); }
        if (!exited) { focus(); chord(XK_q); }
        for (int i = 0; i < 180 && !exited; i++) { exited = !code_alive(); if (!exited) sleep_ms(500); }
        phase("reopen_quit", exited, "");
        return failures ? 1 : 0;
    }

    int ok = wait_title("Visual Studio Code", 300, title, sizeof title);
    snprintf(detail, sizeof detail, "title='%s'", title); phase("window", ok, detail);
    if (!ok) return 1;
    // Let the workbench finish restoring both editors before input.
    sleep_ms(15000);
    focus();
    // Both files were opened on the command line, so there are two tabs. Edit
    // and save whichever is active, switch to the other, edit and save it.
    ok = wait_title(".txt", 60, title, sizeof title);
    int a_first = ok && strstr(title, "a.txt") != NULL;
    snprintf(detail, sizeof detail, "title='%s'", title); phase("two_tabs_open", ok, detail);
    for (int step = 0; step < 2; step++) {
        int on_a = step == 0 ? a_first : !a_first;
        if (step == 1) {
            chord(XK_Next);   // Ctrl+PageDown: next editor tab
            ok = wait_title(on_a ? "a.txt" : "b.txt", 45, title, sizeof title);
            snprintf(detail, sizeof detail, "title='%s'", title); phase("switch_tab", ok, detail);
        }
        // The first input after focusing can be lost while the workbench warms
        // up; retry once and report the attempt count.
        int saved = 0, attempt = 0;
        for (; attempt < 2 && !saved; attempt++) {
            if (attempt) focus();
            chord(XK_Home); type_text(on_a ? "LR_EDIT_OK " : "LR_TAB2_OK "); chord(XK_s);
            saved = file_contains(on_a ? a_path : b_path, on_a ? "LR_EDIT_OK" : "LR_TAB2_OK", 30);
        }
        snprintf(detail, sizeof detail, "attempts=%d", attempt);
        phase(on_a ? "save_a" : "save_b", saved, detail);
    }

    // New untitled file (the phone crash trigger) and typing into it.
    chord(XK_n);
    ok = wait_title("Untitled-1", 60, title, sizeof title);
    snprintf(detail, sizeof detail, "title='%s'", title); phase("new_file", ok, detail);
    type_text("new file typed by the LinuxRunner session driver\n");
    sleep_ms(5000);
    ok = wait_title("Untitled-1", 10, title, sizeof title) && code_alive();
    snprintf(detail, sizeof detail, "title='%s'", title); phase("new_file_typing", ok, detail);

    // Integrated terminal: a shell started by the pty host must run a command.
    // Dismiss any onboarding/sign-in modal first, as a user would.
    press(XK_Escape, 0); sleep_ms(500); focus();
    chord(XK_grave);
    sleep_ms(45000);
    // Keys typed while the terminal is still attaching its shell are dropped
    // (code-ext5 showed "echo " lost); settle with an empty line first.
    type_text("\n"); sleep_ms(3000);
    type_text("echo LR_TERM_$((6*7)) > "); type_text(term_path); type_text("\n");
    int shells = count_processes("bash") + count_processes("/bin/sh");
    // Evidence of what the terminal shows (xwd from the fixture; converted on the host).
    char capture[700];
    snprintf(capture, sizeof capture, "sleep 10; xwd -root -silent -out %s/terminal.xwd", workspace);
    if (system(capture) != 0) printf("CAPTURE terminal.xwd failed\n");
    snprintf(detail, sizeof detail, "shell_processes=%d", shells);
    phase("terminal_command", file_contains(term_path, "LR_TERM_42", 120), detail);

    // Extension host and pty host run as Node utility processes.
    int node = count_processes("node.mojom.NodeService");
    snprintf(detail, sizeof detail, "node_service_processes=%d", node);
    phase("extension_host", node >= 1, detail);

    // Clean exit: untitled content goes to hot-exit backup, no prompt.
    sleep_ms(5000); focus(); chord(XK_q);
    int exited = 0;
    for (int i = 0; i < 180 && !exited; i++) { exited = !code_alive(); if (!exited) sleep_ms(500); }
    phase("quit", exited, "");
    printf("VSCODE_SESSION %s failures=%d fake_failures=%d\n", failures ? "FAIL" : "PASS", failures, fake_failures);
    return failures ? 1 : 0;
}

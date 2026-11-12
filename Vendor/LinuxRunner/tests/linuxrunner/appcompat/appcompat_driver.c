// Guest-side X11 application compatibility driver.
//   appcompat_driver <name> <timeout-seconds> <actions-file|-> <output-dir> -- <command> [args...]
// Launches the command in its own process group, measures the time until a
// new viewable top-level window appears, runs scripted actions through XTest,
// closes the application the ICCCM way (WM_DELETE_WINDOW) and checks for a
// clean exit and leftover processes. Prints ACTION lines and one
// "APPCOMPAT {json}" result line. Needs libX11 and libXtst.so.6 (the fixture
// has no XTest header, so its entry points are declared here).
//
// Actions file, one per line ('#' comments):
//   sleep MS | click X Y | type TEXT | key COMBO (e.g. ctrl+s, Return, alt+F4)
//   expect_title SUBSTRING SECONDS | expect_window SUBSTRING SECONDS
//   expect_file PATH SUBSTRING SECONDS | capture NAME | shell COMMAND
// Coordinates are relative to the application window. Every expectation is
// checked independently of the screen (titles, files, process state).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int XTestFakeKeyEvent(Display *, unsigned int, Bool, unsigned long);
int XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
int XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
Bool XTestQueryExtension(Display *, int *, int *, int *, int *);

static Display *dpy;
static pid_t child;
static int child_status = -1, child_exited;
static Window app_window;
static char app_title[512], app_class[128];
static const char *output_dir;
static int actions_ok, actions_failed;
static long long launched_at, ready_ms = -1;
static int settled;

static long long mono_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
// Application windows can vanish between XQueryTree and the next property
// query (splash screens, transient dialogs). Xlib's default handler would exit
// the driver on that BadWindow; count such races and report other errors.
static int x_vanished_windows, x_other_errors;
static int x_error(Display *d, XErrorEvent *e) {
    (void) d;
    if (e->error_code == BadWindow || e->error_code == BadDrawable) { x_vanished_windows++; return 0; }
    x_other_errors++;
    fprintf(stderr, "APPCOMPAT_X_ERROR code=%d request=%d minor=%d resource=0x%lx\n",
            e->error_code, e->request_code, e->minor_code, e->resourceid);
    return 0;
}
static int reap(void) {
    if (child_exited) return 1;
    int status; pid_t r = waitpid(child, &status, WNOHANG);
    if (r == child) { child_exited = 1; child_status = status; }
    return child_exited;
}
static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int window_title(Window w, char *out, size_t size) {
    Atom name = XInternAtom(dpy, "_NET_WM_NAME", False), utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom type; int format; unsigned long count, after; unsigned char *data = NULL;
    out[0] = 0;
    if (XGetWindowProperty(dpy, w, name, 0, 1024, False, utf8, &type, &format, &count, &after, &data) == Success && data) {
        snprintf(out, size, "%s", (char *) data); XFree(data); return 1;
    }
    // WM_NAME may be STRING, UTF8_STRING or COMPOUND_TEXT (LibreOffice's X11
    // backend under a UTF-8 locale); XFetchName only returns STRING.
    XTextProperty prop;
    if (XGetWMName(dpy, w, &prop) && prop.value) {
        char **list = NULL; int count = 0;
        if (Xutf8TextPropertyToTextList(dpy, &prop, &list, &count) >= Success && count > 0 && list) {
            snprintf(out, size, "%s", list[0]); XFreeStringList(list); XFree(prop.value); return 1;
        }
        if (list) XFreeStringList(list);
        XFree(prop.value);
    }
    return 0;
}
static int window_class(Window w, char *out, size_t size) {
    XClassHint hint = {0};
    out[0] = 0;
    if (!XGetClassHint(dpy, w, &hint)) return 0;
    snprintf(out, size, "%s", hint.res_class ? hint.res_class : "");
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return 1;
}
// Client windows: top-level children of the root (and one level below, in
// case a reparenting window manager runs) that carry WM_STATE or a class.
#define MAX_WINDOWS 512
static int list_clients(Window *out) {
    int n = 0;
    Window root, parent, *kids = NULL; unsigned count = 0;
    if (!XQueryTree(dpy, DefaultRootWindow(dpy), &root, &parent, &kids, &count)) return 0;
    for (unsigned i = 0; i < count && n < MAX_WINDOWS; i++) {
        char cls[128], name[256];
        // Clients normally set WM_CLASS; some (glxgears) only set a name.
        if (window_class(kids[i], cls, sizeof cls) || window_title(kids[i], name, sizeof name)) { out[n++] = kids[i]; continue; }
        Window r2, p2, *grand = NULL; unsigned m = 0;
        if (XQueryTree(dpy, kids[i], &r2, &p2, &grand, &m)) {
            for (unsigned j = 0; j < m && n < MAX_WINDOWS; j++) if (window_class(grand[j], cls, sizeof cls)) out[n++] = grand[j];
            if (grand) XFree(grand);
        }
    }
    if (kids) XFree(kids);
    return n;
}
static int viewable(Window w, int min_w, int min_h) {
    XWindowAttributes a;
    return XGetWindowAttributes(dpy, w, &a) && a.map_state == IsViewable && a.width >= min_w && a.height >= min_h;
}
static Window before[MAX_WINDOWS]; static int before_count;
static int was_present(Window w) { for (int i = 0; i < before_count; i++) if (before[i] == w) return 1; return 0; }
// Finds a new viewable client window whose title contains needle (NULL: any).
static Window find_new_window(const char *needle, char *title, size_t size) {
    Window list[MAX_WINDOWS]; int n = list_clients(list);
    for (int i = 0; i < n; i++) {
        if (was_present(list[i]) || !viewable(list[i], 60, 40)) continue;
        window_title(list[i], title, size);
        if (!needle || strstr(title, needle)) return list[i];
    }
    return 0;
}

static void press_combo(const char *combo) {
    char copy[128]; snprintf(copy, sizeof copy, "%s", combo);
    KeyCode mods[4]; int nmods = 0; KeySym key = NoSymbol;
    for (char *part = strtok(copy, "+"); part; part = strtok(NULL, "+")) {
        KeySym m = NoSymbol;
        if (!strcasecmp(part, "ctrl")) m = XK_Control_L;
        else if (!strcasecmp(part, "shift")) m = XK_Shift_L;
        else if (!strcasecmp(part, "alt")) m = XK_Alt_L;
        else if (!strcasecmp(part, "super")) m = XK_Super_L;
        if (m != NoSymbol && nmods < 4) mods[nmods++] = XKeysymToKeycode(dpy, m);
        else key = strlen(part) == 1 ? (KeySym)(unsigned char) part[0] : XStringToKeysym(part);
    }
    KeyCode kc = key == NoSymbol ? 0 : XKeysymToKeycode(dpy, key);
    if (!kc) { fprintf(stderr, "appcompat: no keycode for '%s'\n", combo); return; }
    for (int i = 0; i < nmods; i++) XTestFakeKeyEvent(dpy, mods[i], True, 0);
    XTestFakeKeyEvent(dpy, kc, True, 0); XTestFakeKeyEvent(dpy, kc, False, 0);
    for (int i = nmods - 1; i >= 0; i--) XTestFakeKeyEvent(dpy, mods[i], False, 0);
    XSync(dpy, False); sleep_ms(60);
}
static void type_char(unsigned char c) {
    KeySym ks = c == '\n' ? XK_Return : c == '\t' ? XK_Tab : (KeySym) c;
    KeyCode kc = XKeysymToKeycode(dpy, ks); if (!kc) return;
    int per = 0; KeySym *map = XGetKeyboardMapping(dpy, kc, 1, &per);
    int shift = map && per > 1 && map[0] != ks && map[1] == ks;
    if (map) XFree(map);
    KeyCode sk = XKeysymToKeycode(dpy, XK_Shift_L);
    if (shift) XTestFakeKeyEvent(dpy, sk, True, 0);
    XTestFakeKeyEvent(dpy, kc, True, 0); XTestFakeKeyEvent(dpy, kc, False, 0);
    if (shift) XTestFakeKeyEvent(dpy, sk, False, 0);
    XSync(dpy, False); sleep_ms(35);
}
static void click(int x, int y) {
    int rx = x, ry = y; Window c;
    if (app_window) XTranslateCoordinates(dpy, app_window, DefaultRootWindow(dpy), x, y, &rx, &ry, &c);
    XTestFakeMotionEvent(dpy, -1, rx, ry, 0); XSync(dpy, False); sleep_ms(80);
    XTestFakeButtonEvent(dpy, 1, True, 0); XTestFakeButtonEvent(dpy, 1, False, 0); XSync(dpy, False); sleep_ms(200);
}
static int file_contains(const char *path, const char *needle, int seconds) {
    for (long long end = mono_ms() + seconds * 1000LL;; sleep_ms(400)) {
        FILE *f = fopen(path, "r");
        if (f) {
            static char buf[1 << 16]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            if (!*needle || strstr(buf, needle)) return 1;
        }
        if (mono_ms() >= end) return 0;
    }
}
static void action_result(const char *verb, const char *arg, int ok, long long started, const char *detail) {
    if (ok) actions_ok++; else actions_failed++;
    printf("ACTION verb=%s ok=%d ms=%lld arg='%s' %s\n", verb, ok, mono_ms() - started, arg, detail ? detail : "");
}
static void run_action(char *line) {
    char *nl = strchr(line, '\n'); if (nl) *nl = 0;
    while (isspace((unsigned char) *line)) line++;
    if (!*line || *line == '#') return;
    char *verb = line, *arg = strchr(line, ' ');
    if (arg) *arg++ = 0; else arg = "";
    long long started = mono_ms(); char detail[600] = "";
    // Interaction actions first let the first frame settle and give the window
    // focus the way a user does (click into it); readiness checks do not.
    if (!settled && strncmp(verb, "ready_", 6) && strcmp(verb, "sleep") && app_window) {
        settled = 1;
        sleep_ms(1500);
        XWindowAttributes a; XGetWindowAttributes(dpy, app_window, &a);
        click(a.width / 2, a.height / 2);
        started = mono_ms();
    }
    if (!strcmp(verb, "sleep")) { sleep_ms(atoi(arg)); return; }
    if (!strcmp(verb, "click")) { int x = 0, y = 0; sscanf(arg, "%d %d", &x, &y); click(x, y); return; }
    if (!strcmp(verb, "type")) { for (const char *p = arg; *p; p++) { if (p[0] == '\\' && p[1] == 'n') { type_char('\n'); p++; } else type_char((unsigned char) *p); } return; }
    if (!strcmp(verb, "key")) { press_combo(arg); return; }
    if (!strcmp(verb, "expect_title") || !strcmp(verb, "expect_window")) {
        char needle[256] = ""; int seconds = 30; char *last = strrchr(arg, ' ');
        if (last) { seconds = atoi(last + 1); snprintf(needle, sizeof needle, "%.*s", (int)(last - arg), arg); }
        int ok = 0; char title[512] = "";
        for (long long end = mono_ms() + seconds * 1000LL; !ok && mono_ms() < end; sleep_ms(300)) {
            if (!strcmp(verb, "expect_title")) { window_title(app_window, title, sizeof title); ok = strstr(title, needle) != NULL; }
            else ok = find_new_window(needle, title, sizeof title) != 0;
        }
        snprintf(detail, sizeof detail, "title='%.400s'", title);
        action_result(verb, needle, ok, started, detail); return;
    }
    // Readiness: the first point where the application demonstrably works
    // (shows the opened document/page in its title, or executed typed input).
    // Times count from the launch request; the first ready action wins.
    if (!strcmp(verb, "ready_title")) {
        char needle[256] = ""; int seconds = 60; char *last = strrchr(arg, ' ');
        if (last) { seconds = atoi(last + 1); snprintf(needle, sizeof needle, "%.*s", (int)(last - arg), arg); }
        int ok = 0; char title[512] = "";
        for (long long end = mono_ms() + seconds * 1000LL; !ok && mono_ms() < end; sleep_ms(100)) {
            Window w = find_new_window(needle, title, sizeof title);
            ok = w != 0;
            if (ok && app_window != w) app_window = w;
        }
        if (ok && ready_ms < 0) ready_ms = mono_ms() - launched_at;
        snprintf(detail, sizeof detail, "ready_ms=%lld title='%.300s'", ready_ms, title);
        action_result(verb, needle, ok, started, detail); return;
    }
    if (!strcmp(verb, "ready_file")) {
        char path[256] = "", needle[256] = ""; int seconds = 60;
        char *a = strtok(arg, " "), *b = strtok(NULL, " "), *c = strtok(NULL, " ");
        if (a) snprintf(path, sizeof path, "%s", a);
        if (b && c) { snprintf(needle, sizeof needle, "%s", b); seconds = atoi(c); } else if (b) seconds = atoi(b);
        int ok = file_contains(path, needle, seconds);
        if (ok && ready_ms < 0) ready_ms = mono_ms() - launched_at;
        snprintf(detail, sizeof detail, "ready_ms=%lld", ready_ms);
        action_result(verb, path, ok, started, detail); return;
    }
    if (!strcmp(verb, "expect_file")) {
        char path[256] = "", needle[256] = ""; int seconds = 20;
        char *a = strtok(arg, " "), *b = strtok(NULL, " "), *c = strtok(NULL, " ");
        if (a) snprintf(path, sizeof path, "%s", a);
        if (b && c) { snprintf(needle, sizeof needle, "%s", b); seconds = atoi(c); }
        else if (b) seconds = atoi(b);
        action_result(verb, path, file_contains(path, needle, seconds), started, ""); return;
    }
    if (!strcmp(verb, "capture")) {
        char command[700]; snprintf(command, sizeof command, "xwd -root -silent -out %s/%s.xwd", output_dir, arg);
        action_result(verb, arg, system(command) == 0, started, ""); return;
    }
    if (!strcmp(verb, "shell")) { int r = system(arg); snprintf(detail, sizeof detail, "status=%d", r); action_result(verb, arg, r == 0, started, detail); return; }
    action_result(verb, arg, 0, started, "unknown action");
}
static int group_members(pid_t pgid) {
    DIR *d = opendir("/proc"); if (!d) return -1;
    int count = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char) e->d_name[0])) continue;
        pid_t pid = atoi(e->d_name);
        if (pid != child && getpgid(pid) == pgid) {
            char path[300], buf[512]; snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
            FILE *f = fopen(path, "r"); int tgid = -1;
            if (f) { while (fgets(buf, sizeof buf, f)) if (sscanf(buf, "Tgid: %d", &tgid) == 1) break; fclose(f); }
            if (tgid == pid) count++;
        } else if (pid == child && !reap()) count++;
    }
    closedir(d);
    return count;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int sep = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--")) { sep = i; break; }
    if (sep != 5 || sep + 1 >= argc) { fprintf(stderr, "usage: %s name timeout actions output-dir -- command...\n", argv[0]); return 2; }
    const char *name = argv[1]; int timeout = atoi(argv[2]); const char *actions = argv[3]; output_dir = argv[4];
    setlocale(LC_ALL, "");   // COMPOUND_TEXT titles convert through the locale
    XSetErrorHandler(x_error);
    for (int i = 0; i < 120 && !(dpy = XOpenDisplay(NULL)); i++) sleep_ms(500);
    if (!dpy) { printf("APPCOMPAT {\"name\":\"%s\",\"error\":\"no display\"}\n", name); return 1; }
    int ev, er, major, minor;
    if (!XTestQueryExtension(dpy, &ev, &er, &major, &minor)) { printf("APPCOMPAT {\"name\":\"%s\",\"error\":\"no XTEST\"}\n", name); return 1; }
    before_count = list_clients(before);

    long long launched = mono_ms();
    launched_at = launched;
    child = fork();
    if (child == 0) {
        setpgid(0, 0);
        char log[600]; snprintf(log, sizeof log, "%s/%s.app.log", output_dir, name);
        FILE *f = freopen(log, "w", stdout); (void) f; dup2(1, 2);
        execvp(argv[sep + 1], argv + sep + 1);
        _exit(127);
    }
    setpgid(child, child);
    long long window_ms = -1;
    for (long long end = launched + timeout * 1000LL; mono_ms() < end && !reap(); sleep_ms(150)) {
        app_window = find_new_window(NULL, app_title, sizeof app_title);
        if (app_window) { window_ms = mono_ms() - launched; break; }
    }
    if (app_window) window_class(app_window, app_class, sizeof app_class);
    printf("WINDOW name=%s ms=%lld title='%s' class='%s'\n", name, window_ms, app_title, app_class);

    if (app_window) {
        // Let the first frame settle, then give the window keyboard focus the
        // way a user does (click into it; the session's focus manager handles
        // WM_TAKE_FOCUS).
        if (strcmp(actions, "-")) {
            FILE *f = fopen(actions, "r"); char line[1024];
            if (f) { while (fgets(line, sizeof line, f) && !reap()) run_action(line); fclose(f); }
            else action_result("actions", actions, 0, mono_ms(), "unreadable");
        }
        window_title(app_window, app_title, sizeof app_title);
    }
    // Launchers may hand the window to another process and exit (single
    // instance apps), so a remaining window counts as alive too.
    char scratch[512];
    int alive_after_actions = !reap() || find_new_window(NULL, scratch, sizeof scratch) != 0;

    // ICCCM close; then SIGTERM, then SIGKILL.
    const char *close_result = "already_exited";
    long long close_started = mono_ms();
    int window_gone = -1, exited_after_close = -1, supports_delete = 0;
    if (alive_after_actions) {
        close_result = "clean";
        Window list[MAX_WINDOWS]; int n = list_clients(list);
        Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False), del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        for (int i = 0; i < n; i++) {
            if (was_present(list[i])) continue;
            Atom *proto = NULL; int count = 0;
            if (XGetWMProtocols(dpy, list[i], &proto, &count)) {
                for (int k = 0; k < count; k++) if (proto[k] == del) supports_delete = 1;
                XFree(proto);
            }
            XEvent e = {0}; e.xclient.type = ClientMessage; e.xclient.window = list[i];
            e.xclient.message_type = protocols; e.xclient.format = 32;
            e.xclient.data.l[0] = (long) del; e.xclient.data.l[1] = CurrentTime;
            XSendEvent(dpy, list[i], False, NoEventMask, &e);
        }
        XSync(dpy, False);
        if (!supports_delete && app_window) {
            // ICCCM: a window manager kills clients that do not take part in
            // WM_DELETE_WINDOW (glxgears); that is a normal close.
            XKillClient(dpy, app_window); XSync(dpy, False);
            close_result = "xkillclient";
        }
        int gone = 0;
        for (long long end = mono_ms() + 45000; mono_ms() < end && !gone; sleep_ms(200))
            gone = reap() && !find_new_window(NULL, scratch, sizeof scratch);
        window_gone = find_new_window(NULL, scratch, sizeof scratch) == 0;
        exited_after_close = reap();
        if (!gone && !window_gone) {
            close_result = "window_stayed";
            // Which windows remain (a confirmation dialog is legitimate)?
            Window rest[MAX_WINDOWS]; int m = list_clients(rest);
            for (int i = 0; i < m; i++) {
                if (was_present(rest[i]) || !viewable(rest[i], 20, 20)) continue;
                char t[256]; window_title(rest[i], t, sizeof t);
                printf("REMAINING name=%s window=%#lx title='%s'\n", name, rest[i], t);
            }
            char command[700]; snprintf(command, sizeof command, "xwd -root -silent -out %s/%s-after-close.xwd", output_dir, name);
            if (system(command) != 0) printf("CAPTURE after-close failed\n");
        }
        if (!reap()) {
            close_result = "sigterm"; kill(-child, SIGTERM);
            for (long long end = mono_ms() + 5000; mono_ms() < end && !reap(); sleep_ms(200)) {}
        }
        if (!reap()) { close_result = "sigkill"; kill(-child, SIGKILL); waitpid(child, &child_status, 0); child_exited = 1; }
    }
    long long close_ms = mono_ms() - close_started;
    sleep_ms(1500);
    int leftover = group_members(child);
    if (leftover > 0) kill(-child, SIGKILL);

    printf("APPCOMPAT {\"name\":"); json_string(stdout, name);
    printf(",\"window_ms\":%lld,\"ready_ms\":%lld,\"title\":", window_ms, ready_ms); json_string(stdout, app_title);
    printf(",\"class\":"); json_string(stdout, app_class);
    printf(",\"actions_ok\":%d,\"actions_failed\":%d,\"alive_after_actions\":%d,\"close\":\"%s\",\"close_ms\":%lld",
           actions_ok, actions_failed, alive_after_actions, close_result, close_ms);
    if (WIFEXITED(child_status)) printf(",\"exit_code\":%d", WEXITSTATUS(child_status));
    else if (WIFSIGNALED(child_status)) printf(",\"exit_signal\":%d", WTERMSIG(child_status));
    printf(",\"wm_delete_supported\":%d,\"window_gone_after_close\":%d,\"exited_after_close\":%d", supports_delete, window_gone, exited_after_close);
    printf(",\"x_vanished_windows\":%d,\"x_other_errors\":%d", x_vanished_windows, x_other_errors);
    printf(",\"leftover_processes\":%d}\n", leftover);
    return 0;
}

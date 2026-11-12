/* Read-only X11 startup observer: visibility is not proof of application paint. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
static int ignore_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
int main(int argc, char **argv) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { puts("X11_STARTUP: observer_connection=FAIL"); return 1; }
    XSetErrorHandler(ignore_error);
    int seconds = argc > 1 ? atoi(argv[1]) : 240;
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    puts("X11_STARTUP: observer_connection=PASS first_paint=UNVERIFIED");
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        Window root, parent, *children = NULL; unsigned count = 0, mapped = 0;
        if (XQueryTree(d, DefaultRootWindow(d), &root, &parent, &children, &count)) {
            printf("X11_STARTUP: elapsed=%ld root_children=%u\n", now.tv_sec-start.tv_sec, count);
            for (unsigned i=0; i<count; i++) {
                XWindowAttributes a; char *name=NULL; XClassHint hint={0};
                if (!XGetWindowAttributes(d, children[i], &a)) continue;
                if (a.map_state == IsViewable && !a.override_redirect && a.width >= 64 && a.height >= 64) mapped++;
                XFetchName(d, children[i], &name); XGetClassHint(d, children[i], &hint);
                printf("X11_STARTUP: window=%lx map_state=%d viewable=%s geometry=%dx%d%+d%+d override=%d class=%s title=%s\n",
                    children[i], a.map_state, a.map_state==IsViewable?"yes":"no", a.width,a.height,a.x,a.y,
                    a.override_redirect, hint.res_class?hint.res_class:"", name?name:"");
                if (name) XFree(name);
                if (hint.res_name) XFree(hint.res_name);
                if (hint.res_class) XFree(hint.res_class);
            }
            XFree(children);
        }
        if (argc > 2) {
            char path[4096];
            if (snprintf(path, sizeof path, "%s.new", argv[2]) < (int)sizeof path) {
                FILE *out = fopen(path, "w");
                if (out) {
                    fprintf(out, "mapped=%u\ncreated=%u\nelapsed=%ld\nfirst_paint=UNVERIFIED\n", mapped, count, now.tv_sec-start.tv_sec);
                    if (fclose(out) == 0) rename(path, argv[2]);
                }
            }
        }
        fflush(stdout); sleep(5);
    } while (now.tv_sec-start.tv_sec < seconds && (argc < 4 || access(argv[3], F_OK) != 0));
    XCloseDisplay(d); return 0;
}

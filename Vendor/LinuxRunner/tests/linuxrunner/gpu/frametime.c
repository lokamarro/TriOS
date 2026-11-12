// Frame timing for any GLX program: LD_PRELOAD=libframetime.so <program>
// Wraps glXSwapBuffers and SDL_GL_SwapWindow; prints FRAMETIME lines every 5 s and a summary at exit
// (frames, average FPS, frame-time percentiles, 1% low FPS). The first
// LR_FRAMETIME_SKIP seconds (default 10) are excluded from the summary (startup).
//   gcc -O2 -fPIC -shared frametime.c -ldl -o libframetime.so
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MAX_FRAMES 200000
static double frames_ms[MAX_FRAMES], last, origin, window_start;
static unsigned count, window_frames;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int compare(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double skip_ms(void) { const char *s = getenv("LR_FRAMETIME_SKIP"); return (s ? atof(s) : 10) * 1000; }
static void frame(void) {
    double t = now_ms();
    if (!origin) origin = window_start = t;
    if (last && t - origin > skip_ms() && count < MAX_FRAMES) frames_ms[count++] = t - last;
    last = t; window_frames++;
    if (t - window_start >= 5000) {
        fprintf(stderr, "FRAMETIME: t=%.0fs fps=%.2f\n", (t - origin) / 1000, window_frames * 1000.0 / (t - window_start));
        window_frames = 0; window_start = t;
    }
}
// Direct GLX callers, and SDL2 programs (SDL resolves glXSwapBuffers from libGL
// itself, so its own SDL_GL_SwapWindow is the interposable point).
static __thread int in_sdl;
// Build with -DLR_FRAMETIME_GLX for programs that call glXSwapBuffers directly.
// SDL programs use only the SDL wrapper: a second glXSwapBuffers provider would
// make the Mesa/Metal bridge keep the original and skip its swap hook.
#ifdef LR_FRAMETIME_GLX
void glXSwapBuffers(void *display, unsigned long drawable) {
    static void (*real)(void *, unsigned long);
    if (!real) real = (void (*)(void *, unsigned long))dlsym(RTLD_NEXT, "glXSwapBuffers");
    real(display, drawable);
    if (!in_sdl) frame();
}
#endif
int SDL_GL_SwapWindow(void *window) {
    static int (*real)(void *);
    if (!real) real = (int (*)(void *))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    in_sdl = 1; int result = real ? real(window) : -1; in_sdl = 0;
    frame();
    return result;
}
__attribute__((destructor)) static void summary(void) {
    if (count < 2) { fprintf(stderr, "FRAMETIME_SUMMARY frames=%u (too few after skip)\n", count); return; }
    double total = 0; for (unsigned i = 0; i < count; i++) total += frames_ms[i];
    qsort(frames_ms, count, sizeof *frames_ms, compare);
    double p50 = frames_ms[count / 2], p95 = frames_ms[count * 95 / 100], p99 = frames_ms[count * 99 / 100], worst = frames_ms[count - 1];
    unsigned low_n = count / 100 ? count / 100 : 1; double low = 0; for (unsigned i = count - low_n; i < count; i++) low += frames_ms[i];
    fprintf(stderr, "FRAMETIME_SUMMARY frames=%u seconds=%.1f avg_fps=%.2f p50_ms=%.1f p95_ms=%.1f p99_ms=%.1f max_ms=%.1f low1pct_fps=%.2f\n",
            count, total / 1000, count * 1000.0 / total, p50, p95, p99, worst, 1000.0 / (low / low_n));
}

// Indirect-call microbenchmark: function-pointer calls, virtual-style calls
// through a vtable and a pixel-getter loop shaped like LibreOffice's
// Bitmap::Scale (one indirect call per pixel). Prints nanoseconds per call.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static long f0(long a) { return a + 1; }
static long f1(long a) { return a ^ 3; }
static long (*volatile table[2])(long) = {f0, f1};

struct obj { const struct vt *vt; long v; };
struct vt { long (*get)(const struct obj *); };
static long get_a(const struct obj *o) { return o->v; }
static long get_b(const struct obj *o) { return o->v * 2; }
static const struct vt vt_a = {get_a}, vt_b = {get_b};

typedef uint32_t (*pixel_fn)(const uint8_t *, long);
static uint32_t get_bgra(const uint8_t *row, long x) { const uint8_t *p = row + 4 * x; return p[0] | p[1] << 8 | p[2] << 16; }

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 20000000;
    double t = now(); long acc = 0;
    for (long i = 0; i < n; i++) acc = table[i & 1](acc);
    double fp = (now() - t) * 1e9 / n;

    struct obj objs[2] = {{&vt_a, 3}, {&vt_b, 5}};
    t = now(); long sum = 0;
    for (long i = 0; i < n; i++) sum += objs[i & 1].vt->get(&objs[i & 1]);
    double virt = (now() - t) * 1e9 / n;

    long width = 2048, rows = n / width;
    uint8_t *row = calloc(4, width); pixel_fn volatile get = get_bgra; uint32_t *out = malloc(4 * width);
    t = now();
    for (long y = 0; y < rows; y++) for (long x = 0; x < width; x++) out[x] = get(row, x);
    double pix = (now() - t) * 1e9 / (rows * width);

    printf("INDIRECT_CALL_BENCH n=%ld fnptr_ns=%.2f virtual_ns=%.2f pixel_ns=%.2f check=%ld/%ld/%u\n",
           n, fp, virt, pix, acc, sum, out[width - 1]);
    return 0;
}

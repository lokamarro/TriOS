// Guest PC sampling profiler (diagnostics only; off unless ISH_GUEST_PROFILE
// names an output file). A host thread samples, ISH_GUEST_PROFILE_HZ times a
// second (default 1000), the guest PC each running guest thread last recorded
// (block entry or branch target) and whether it is executing guest code or a
// system call. Every two seconds it rewrites the output: aggregated
// "SAMPLE tgid state pc count" lines plus each sampled process's
// /proc/<pid>/maps, so samples can be attributed to libraries offline
// (tests/linuxrunner/tools/guest_profile_report.py).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kernel/task.h"
#include "kernel/guest_profile.h"
#include "asbestos/frame.h"
#include "emu/tlb.h"
#include "fs/proc.h"

void proc_maps_dump(struct task *task, struct proc_data *buf);

#define PROFILE_SLOTS 1024
#define PROFILE_TABLE (1u << 18)
#define PROFILE_PIDS 256

bool guest_profile_enabled;
static const char *profile_path;
static unsigned profile_interval_us = 1000;
static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { struct tlb *tlb; pid_t_ tgid; _Atomic int state; } slots[PROFILE_SLOTS];
static __thread int my_slot = -1;
static struct entry { uint64_t pc; pid_t_ tgid; int state; uint64_t count; } *table;
static uint64_t total_samples, dropped_samples;
static pid_t_ seen_pids[PROFILE_PIDS];
static unsigned seen_count;
static char *maps_cache[PROFILE_PIDS]; // last maps text per seen process (kept after exit)
static size_t maps_size[PROFILE_PIDS];

void guest_profile_register(struct tlb *tlb, pid_t_ tgid) {
    if (!guest_profile_enabled) return;
    pthread_mutex_lock(&profile_lock);
    for (int i = 0; i < PROFILE_SLOTS; i++) {
        if (slots[i].tlb == NULL) { slots[i].tlb = tlb; slots[i].tgid = tgid; slots[i].state = 0; my_slot = i; break; }
    }
    pthread_mutex_unlock(&profile_lock);
}

void guest_profile_unregister(struct tlb *tlb) {
    if (!guest_profile_enabled) return;
    pthread_mutex_lock(&profile_lock);
    for (int i = 0; i < PROFILE_SLOTS; i++)
        if (slots[i].tlb == tlb) slots[i].tlb = NULL;
    pthread_mutex_unlock(&profile_lock);
    my_slot = -1;
}

void guest_profile_state(int state) {
    if (guest_profile_enabled && my_slot >= 0)
        atomic_store_explicit(&slots[my_slot].state, state, memory_order_relaxed);
}

// exec keeps the thread but may change the process id seen by later samples.
void guest_profile_set_tgid(pid_t_ tgid) {
    if (guest_profile_enabled && my_slot >= 0) slots[my_slot].tgid = tgid;
}

static void record(pid_t_ tgid, int state, uint64_t pc) {
    uint64_t h = (pc * 0x9e3779b97f4a7c15ull) ^ ((uint64_t) tgid << 32) ^ (uint64_t) state;
    for (unsigned probe = 0; probe < 64; probe++) {
        struct entry *e = &table[(h + probe) & (PROFILE_TABLE - 1)];
        if (e->count == 0) { e->pc = pc; e->tgid = tgid; e->state = state; e->count = 1; return; }
        if (e->pc == pc && e->tgid == tgid && e->state == state) { e->count++; return; }
    }
    dropped_samples++;
}

static void note_pid(pid_t_ tgid) {
    for (unsigned i = 0; i < seen_count; i++) if (seen_pids[i] == tgid) return;
    if (seen_count < PROFILE_PIDS) seen_pids[seen_count++] = tgid;
}

static void dump(void) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", profile_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    pthread_mutex_lock(&profile_lock);
    fprintf(f, "PROFILE interval_us=%u samples=%llu dropped=%llu\n", profile_interval_us,
            (unsigned long long) total_samples, (unsigned long long) dropped_samples);
    for (unsigned i = 0; i < PROFILE_TABLE; i++)
        if (table[i].count)
            fprintf(f, "SAMPLE %d %d %#llx %llu\n", table[i].tgid, table[i].state,
                    (unsigned long long) table[i].pc, (unsigned long long) table[i].count);
    pid_t_ pids[PROFILE_PIDS]; unsigned n = seen_count; memcpy(pids, seen_pids, n * sizeof pids[0]);
    pthread_mutex_unlock(&profile_lock);
    // Maps outside the profiler lock: pids_lock, then the task's own locks, as /proc does.
    for (unsigned i = 0; i < n; i++) {
        struct proc_data buf = {0};
        lock(&pids_lock);
        struct task *task = pid_get_task(pids[i]);
        if (task && !task->exiting && task->sighand && task->group) {
            lock(&task->general_lock);
            proc_maps_dump(task, &buf);
            unlock(&task->general_lock);
        }
        unlock(&pids_lock);
        if (buf.data) { free(maps_cache[i]); maps_cache[i] = buf.data; maps_size[i] = buf.size; }
        if (maps_cache[i]) {
            fprintf(f, "MAPS %d %zu\n", pids[i], maps_size[i]);
            fwrite(maps_cache[i], 1, maps_size[i], f);
        }
    }
    fclose(f);
    rename(tmp, profile_path);
}

static void *sampler(void *arg) {
    (void) arg;
    struct timespec interval = {0, (long) profile_interval_us * 1000};
    unsigned since_dump = 0, dump_every = 2000000 / profile_interval_us;
    for (;;) {
        nanosleep(&interval, NULL);
        pthread_mutex_lock(&profile_lock);
        for (int i = 0; i < PROFILE_SLOTS; i++) {
            struct tlb *tlb = slots[i].tlb;
            if (!tlb || !tlb->frame) continue;
            int state = atomic_load_explicit(&slots[i].state, memory_order_relaxed);
            if (state == 0) continue;
            uint64_t pc = __atomic_load_n(&tlb->frame->cpu.pc, __ATOMIC_RELAXED);
            record(slots[i].tgid, state, pc);
            total_samples++;
            note_pid(slots[i].tgid);
        }
        pthread_mutex_unlock(&profile_lock);
        if (++since_dump >= dump_every) { since_dump = 0; dump(); }
    }
    return NULL;
}

void guest_profile_init(void) {
    const char *path = getenv("ISH_GUEST_PROFILE");
    if (!path || !*path) return;
    const char *hz = getenv("ISH_GUEST_PROFILE_HZ");
    if (hz) { unsigned v = (unsigned) atoi(hz); if (v >= 10 && v <= 10000) profile_interval_us = 1000000 / v; }
    table = calloc(PROFILE_TABLE, sizeof *table);
    if (!table) return;
    profile_path = path;
    guest_profile_enabled = true;
    pthread_t thread;
    if (pthread_create(&thread, NULL, sampler, NULL) != 0) { guest_profile_enabled = false; return; }
    pthread_detach(thread);
}

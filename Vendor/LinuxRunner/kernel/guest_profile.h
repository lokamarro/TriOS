#ifndef KERNEL_GUEST_PROFILE_H
#define KERNEL_GUEST_PROFILE_H
#include <stdbool.h>
#include "misc.h"
struct tlb;
// Guest PC sampling profiler; see guest_profile.c. States: 1 guest code, 2 system call/interrupt.
extern bool guest_profile_enabled;
void guest_profile_init(void);
void guest_profile_register(struct tlb *tlb, pid_t_ tgid);
void guest_profile_unregister(struct tlb *tlb);
void guest_profile_state(int state);
void guest_profile_set_tgid(pid_t_ tgid);
#endif

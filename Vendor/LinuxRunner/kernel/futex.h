#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H
#include <stdatomic.h>
struct proc_data;
int futex_pi_show(struct proc_data *buf);

extern atomic_bool runtime_futex_pi_trace_enabled;

void futex_pi_exit(void);
void futex_robust_exit(void);

int futex_wake(addr_t uaddr, dword_t val);

#endif

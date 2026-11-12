#ifndef LR_BOOT_LOG_H
#define LR_BOOT_LOG_H
#include <stdint.h>
// Normal-thread diagnostics only; no fatal-signal handler is installed.
void LRStartupInitialize(void);
void LRBootEvent(const char *message);
void LRBootSnapshot(int pid, uint64_t pc, uint64_t sp, uint64_t syscall, uint64_t opcode);
#endif

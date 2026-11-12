// SPDX-License-Identifier: GPL-2.0-or-later
//
// Freestanding arm64/Linux guest /init that asks the kernel to restart at
// once. A guest that does this on every boot never boots past it, which is
// what the launcher's loop guard is for.

namespace {

constexpr long nr_reboot = 142;
constexpr long magic1 = 0xfee1dead;
constexpr long magic2 = 672274793;
constexpr long command_restart = 0x01234567;

inline long raw_syscall(long number, long a0, long a1, long a2, long a3)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
			 : "memory", "cc");
	return x0;
}

} // namespace

extern "C" void _start()
{
	raw_syscall(nr_reboot, magic1, magic2, command_restart, 0);
	for (;;)
		__asm__ volatile("wfi");
}

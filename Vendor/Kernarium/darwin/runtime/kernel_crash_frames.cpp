// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_crash_frames.hpp"

#include <mach/mach.h>
#include <mach/vm_map.h>

namespace uml::runtime {
namespace {

/* Deep enough for a host stack and a kernel stack chained into one. */
constexpr std::size_t frame_depth_limit = 1024;

/* A frame record is the pair the AArch64 prologue pushes: the caller's frame
 * pointer and the return address. */
[[nodiscard]] bool read_frame_record(std::uintptr_t frame,
	std::uintptr_t (&record)[2]) noexcept
{
	vm_size_t length = 0;
	return vm_read_overwrite(mach_task_self(), frame, sizeof(record),
		       reinterpret_cast<vm_address_t>(record), &length) ==
		KERN_SUCCESS &&
		length == sizeof(record);
}

[[nodiscard]] bool write_word(std::uintptr_t address,
	std::uintptr_t value) noexcept
{
	return vm_write(mach_task_self(), address,
		       reinterpret_cast<vm_offset_t>(&value),
		       sizeof(value)) == KERN_SUCCESS;
}

/* A suspended thread's registers and stack, rewritten where they name the
 * alias. The thread's own state is only changed when thread_set_state() lets
 * it; the frame chain is rewritten either way. */
void translate_thread(const KernelTextAlias *aliases, std::size_t count,
	thread_act_t thread) noexcept
{
	arm_thread_state64_t state;
	mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state(thread, ARM_THREAD_STATE64,
		    reinterpret_cast<thread_state_t>(&state), &words) !=
	    KERN_SUCCESS)
		return;
	const std::uintptr_t pc = arm_thread_state64_get_pc(state);
	const std::uintptr_t lr = arm_thread_state64_get_lr(state);
	const std::uintptr_t frame = arm_thread_state64_get_fp(state);
	const std::uintptr_t new_pc =
		kernel_text_carrier_address(aliases, count, pc);
	const std::uintptr_t new_lr =
		kernel_text_carrier_address(aliases, count, lr);
	if (new_pc != pc || new_lr != lr) {
		arm_thread_state64_set_pc_fptr(state,
			reinterpret_cast<void *>(new_pc));
		arm_thread_state64_set_lr_fptr(state,
			reinterpret_cast<void *>(new_lr));
		(void)thread_set_state(thread, ARM_THREAD_STATE64,
			reinterpret_cast<thread_state_t>(&state), words);
	}
	(void)translate_frame_chain(aliases, count, frame);
}

} // namespace

std::uintptr_t kernel_text_carrier_address(const KernelTextAlias *aliases,
	std::size_t count, std::uintptr_t address) noexcept
{
	for (std::size_t index = 0; index < count; ++index)
		if (address >= aliases[index].begin && address < aliases[index].end)
			return aliases[index].carrier + (address - aliases[index].begin);
	return address;
}

std::size_t translate_frame_chain(const KernelTextAlias *aliases,
	std::size_t count, std::uintptr_t frame) noexcept
{
	std::size_t rewritten = 0;
	std::uintptr_t previous = 0;
	for (std::size_t depth = 0; depth < frame_depth_limit; ++depth) {
		if (frame == 0 || (frame & 15U) != 0 || frame == previous)
			break;
		std::uintptr_t record[2];
		if (!read_frame_record(frame, record))
			break;
		const std::uintptr_t carrier =
			kernel_text_carrier_address(aliases, count, record[1]);
		if (carrier != record[1] && write_word(frame + sizeof(record[0]),
						carrier))
			++rewritten;
		previous = frame;
		frame = record[0];
	}
	return rewritten;
}

void translate_kernel_frames(const KernelTextAlias *aliases,
	std::size_t count) noexcept
{
	if (count == 0)
		return;
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t thread_count = 0;
	if (task_threads(mach_task_self(), &threads, &thread_count) ==
	    KERN_SUCCESS) {
		const thread_act_t self = mach_thread_self();
		/* Stop the others before touching any, and never let them run again:
		 * the process is about to end, and a stopped thread's registers and
		 * stack are what the crash report reads. */
		for (mach_msg_type_number_t index = 0; index < thread_count;
		     ++index)
			if (threads[index] != self)
				(void)thread_suspend(threads[index]);
		for (mach_msg_type_number_t index = 0; index < thread_count;
		     ++index)
			if (threads[index] != self)
				translate_thread(aliases, count, threads[index]);
	}
	(void)translate_frame_chain(aliases, count,
		reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
}

void translate_kernel_frames_for_crash_report() noexcept
{
	KernelTextAlias aliases[kernel_text_alias_limit];
	const std::size_t count =
		recorded_kernel_text_aliases(aliases, kernel_text_alias_limit);
	translate_kernel_frames(aliases,
		count < kernel_text_alias_limit ? count : kernel_text_alias_limit);
}

} // namespace uml::runtime

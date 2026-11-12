// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Statistical profile of translated guest code, taken from the host.
 *
 * Guest profilers need the guest's cooperation, which a production Android
 * build withholds from applications. This sampler needs none: a host thread
 * periodically suspends each vCPU carrier and reads its registers. When the
 * carrier's PC lies in the gadget bank, x28 is the translated block's stream,
 * and the carrier's own executor names the guest block it belongs to and the
 * guest instruction words the block was translated from. Those words identify
 * the code independently of where the guest loaded it, so a host tool can
 * symbolize the profile against the guest's ELF files afterwards. Other host
 * PCs (the guest kernel, the executor, MMU and translation paths) are counted
 * by host symbol.
 *
 * Set DARWIN_UML_GUEST_SAMPLES to PATH or PATH:HZ (default 1000 passes per
 * second) to have the profile rewritten there periodically, when the last
 * executor is retired at the end of a run, and at exit. The hot path is
 * untouched; each guest entry and exit stores one flag.
 */
namespace uml::cpu::arm64::gadgets::diagnostics {

inline constexpr std::size_t sampled_block_words = 16;

/* The guest block a sample landed in. For an AArch32 block guest_address is
 * the block's key (aarch32/guest_key.hpp, bit 63 set) and the words are the
 * guest bytes it was translated from in little-endian words, so a T32
 * block's halfwords come in pairs. */
struct SampledBlock final {
	std::uint64_t guest_address{};
	std::uint32_t instruction_count{};
	std::uint32_t word_count{};
	std::uint32_t words[sampled_block_words]{};
};

class SampledExecutor {
public:
	/* Describe the translation holding a stream address. Called on the
	 * sampler thread while the executor's own thread is suspended inside
	 * gadget code, so it must neither lock nor allocate. */
	[[nodiscard]] virtual bool sample_block(
		const void *stream, SampledBlock &block) const noexcept = 0;

protected:
	~SampledExecutor() = default;
};

struct GuestSamplingSlot;

/* Null when sampling is disabled, which is the only cost it then has. */
[[nodiscard]] GuestSamplingSlot *guest_sampling_register(
	const SampledExecutor &executor) noexcept;
/* Bracket guest execution on the calling thread. */
void guest_sampling_enter(GuestSamplingSlot &slot) noexcept;
void guest_sampling_leave(GuestSamplingSlot &slot) noexcept;
/* Attribute this execution's samples to the guest thread whose TPIDR_EL0
 * lives at thread. The CPU state must outlive the execution; leaving clears
 * it. */
void guest_sampling_thread(
	GuestSamplingSlot &slot, const std::uint64_t *thread) noexcept;
/* Returns once no sampling pass can still use the executor. */
void guest_sampling_unregister(GuestSamplingSlot &slot) noexcept;

} // namespace uml::cpu::arm64::gadgets::diagnostics

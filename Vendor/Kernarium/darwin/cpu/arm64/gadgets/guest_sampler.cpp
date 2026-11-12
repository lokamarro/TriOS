// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_sampler.hpp"

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

/* Zero-size labels around each section the gadget bank (engine.S) emits
 * code into: the bank itself, the sections its gadgets push forwarding
 * variants, cold paths and FEAT_AFP twins into, the SME part and the AArch32
 * part's bank and cold paths. */
extern "C" const char uml_arm64_gadget_bank_begin[];
extern "C" const char uml_arm64_gadget_bank_end[];
extern "C" const char uml_arm64_gadget_forward_section_begin[];
extern "C" const char uml_arm64_gadget_forward_section_end[];
extern "C" const char uml_arm64_gadget_vector_forward_section_begin[];
extern "C" const char uml_arm64_gadget_vector_forward_section_end[];
extern "C" const char uml_arm64_gadget_cold_section_begin[];
extern "C" const char uml_arm64_gadget_cold_section_end[];
extern "C" const char uml_arm64_gadget_afp_section_begin[];
extern "C" const char uml_arm64_gadget_afp_section_end[];
extern "C" const char uml_arm64_gadget_sme_section_begin[];
extern "C" const char uml_arm64_gadget_sme_section_end[];
extern "C" const char uml_arm64_gadget_a32_bank_section_begin[];
extern "C" const char uml_arm64_gadget_a32_bank_section_end[];
extern "C" const char uml_arm64_gadget_a32_cold_section_begin[];
extern "C" const char uml_arm64_gadget_a32_cold_section_end[];

namespace uml::cpu::arm64::gadgets::diagnostics {

struct GuestSamplingSlot final {
	const SampledExecutor *executor{};
	/* A send right held for as long as the slot names the thread, so the
	 * port name cannot be reused by another thread meanwhile. Written by the
	 * executing thread under the sampler mutex; read by the sampler under it
	 * and by the executing thread itself. */
	thread_act_t thread{MACH_PORT_NULL};
	/* Set while the executor runs guest code on that thread. Only then may a
	 * PC in the bank be resolved through this executor's cache. */
	std::atomic_bool active{};
	/* The guest thread pointer (TPIDR_EL0) of the CPU state the executor
	 * runs, published once per execution and cleared on leaving it. A
	 * carrier suspended in gadget code is inside that execution, so the
	 * state it points at is live while the sampler reads it. */
	std::atomic<const std::uint64_t *> guest_thread{};
};

namespace {

[[nodiscard]] bool in_gadget_code(std::uintptr_t pc) noexcept
{
	static constexpr std::pair<const char *, const char *> sections[] = {
		{uml_arm64_gadget_bank_begin, uml_arm64_gadget_bank_end},
		{uml_arm64_gadget_forward_section_begin,
			uml_arm64_gadget_forward_section_end},
		{uml_arm64_gadget_vector_forward_section_begin,
			uml_arm64_gadget_vector_forward_section_end},
		{uml_arm64_gadget_cold_section_begin,
			uml_arm64_gadget_cold_section_end},
		{uml_arm64_gadget_afp_section_begin,
			uml_arm64_gadget_afp_section_end},
		{uml_arm64_gadget_sme_section_begin,
			uml_arm64_gadget_sme_section_end},
		{uml_arm64_gadget_a32_bank_section_begin,
			uml_arm64_gadget_a32_bank_section_end},
		{uml_arm64_gadget_a32_cold_section_begin,
			uml_arm64_gadget_a32_cold_section_end},
	};
	return std::ranges::any_of(sections, [pc](const auto &section) {
		return pc >= reinterpret_cast<std::uintptr_t>(section.first) &&
			pc < reinterpret_cast<std::uintptr_t>(section.second);
	});
}

/* Samples of one block are kept apart per guest thread, so a profile of a
 * phase can be narrowed to the thread doing it. */
struct BlockKey final {
	std::uint64_t address{};
	std::uint64_t words{};
	std::uint64_t thread{};

	bool operator==(const BlockKey &) const noexcept = default;
};

struct BlockKeyHash final {
	[[nodiscard]] std::size_t operator()(const BlockKey &key) const noexcept
	{
		return static_cast<std::size_t>(
			key.address * 0x9e37'79b9'7f4a'7c15ULL ^ key.words ^
			key.thread * 0xc2b2'ae3d'27d4'eb4fULL);
	}
};

struct BlockEntry final {
	SampledBlock block;
	std::uint64_t thread{};
	std::uint64_t count{};
};

[[nodiscard]] std::uint64_t hash_words(const SampledBlock &block) noexcept
{
	std::uint64_t value = 0xcbf2'9ce4'8422'2325ULL;
	for (std::uint32_t i = 0; i < block.word_count; ++i) {
		value ^= block.words[i];
		value *= 0x100'0000'01b3ULL;
	}
	return value;
}

[[nodiscard]] std::string host_symbol(std::uintptr_t pc)
{
	Dl_info info{};
	if (dladdr(reinterpret_cast<const void *>(pc), &info) == 0)
		return "??";
	if (info.dli_sname != nullptr) {
		std::string name = info.dli_sname;
		if (!name.empty() && name.front() == '_')
			name.erase(name.begin());
		return name;
	}
	std::string image = info.dli_fname != nullptr ? info.dli_fname : "??";
	if (const auto slash = image.rfind('/'); slash != std::string::npos)
		image.erase(0, slash + 1);
	return image + " (no symbol)";
}

class Sampler final {
public:
	Sampler(std::string path, unsigned hz) : path_{std::move(path)},
		period_{std::chrono::nanoseconds{1'000'000'000 / std::max(hz, 1U)}}
	{
		targets_.reserve(256);
		/* Host signals are the guest's interrupts and must never be
		 * delivered to this thread; it inherits the blocked mask. */
		sigset_t all;
		sigset_t previous;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &previous);
		worker_ = std::thread{[this] { run(); }};
		pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	}

	GuestSamplingSlot *add(const SampledExecutor &executor)
	{
		auto slot = std::make_unique<GuestSamplingSlot>();
		slot->executor = &executor;
		const std::scoped_lock lock{mutex_};
		slots_.push_back(std::move(slot));
		return slots_.back().get();
	}

	void bind(GuestSamplingSlot &slot) noexcept
	{
		const auto self = pthread_mach_thread_np(pthread_self());
		if (slot.thread != self) {
			const std::scoped_lock lock{mutex_};
			if (slot.thread != MACH_PORT_NULL)
				mach_port_deallocate(mach_task_self(), slot.thread);
			slot.thread = mach_thread_self();
		}
		slot.active.store(true, std::memory_order_release);
	}

	void remove(GuestSamplingSlot &slot) noexcept
	{
		bool last = false;
		{
			const std::scoped_lock lock{mutex_};
			if (slot.thread != MACH_PORT_NULL)
				mach_port_deallocate(mach_task_self(), slot.thread);
			std::erase_if(slots_, [&](const auto &held) {
				return held.get() == &slot;
			});
			last = slots_.empty();
		}
		/* The emulator leaves through _Exit(), which skips atexit(), but
		 * the end of a run retires every executor first. Writing the
		 * profile when the last one goes keeps the samples taken since
		 * the previous periodic dump, all of a run shorter than one. */
		if (last)
			dump();
	}

	void finish() noexcept
	{
		stop_.store(true, std::memory_order_relaxed);
		if (worker_.joinable())
			worker_.join();
		dump();
	}

private:
	struct Target final {
		thread_act_t thread{MACH_PORT_NULL};
		const GuestSamplingSlot *active{};
	};

	void run()
	{
		auto next = std::chrono::steady_clock::now();
		auto next_dump = next + std::chrono::seconds{2};
		while (!stop_.load(std::memory_order_relaxed)) {
			pass();
			next += period_;
			const auto now = std::chrono::steady_clock::now();
			if (now >= next_dump) {
				dump();
				next_dump = now + std::chrono::seconds{2};
			}
			if (next < now)
				next = now;
			std::this_thread::sleep_until(next);
		}
	}

	void pass()
	{
		const std::scoped_lock lock{mutex_};
		++passes_;
		/* One sample per carrier thread, attributed to the executor running
		 * guest code on it if there is one. */
		targets_.clear();
		for (const auto &slot : slots_) {
			if (slot->thread == MACH_PORT_NULL)
				continue;
			auto target = std::find_if(targets_.begin(), targets_.end(),
				[&](const Target &held) {
					return held.thread == slot->thread;
				});
			if (target == targets_.end()) {
				if (targets_.size() == targets_.capacity())
					continue;
				targets_.push_back({slot->thread, nullptr});
				target = targets_.end() - 1;
			}
			if (slot->active.load(std::memory_order_acquire))
				target->active = slot.get();
		}
		for (const auto &target : targets_)
			sample(target);
	}

	void sample(const Target &target)
	{
		/* Between suspend and resume nothing may lock or allocate: the
		 * suspended thread can hold any lock, the allocator's included. */
		if (thread_suspend(target.thread) != KERN_SUCCESS)
			return;
		arm_thread_state64_t state{};
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		const bool read = thread_get_state(target.thread,
			ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
			&count) == KERN_SUCCESS;
		const auto pc = read ?
			static_cast<std::uintptr_t>(arm_thread_state64_get_pc(state)) : 0;
		const bool in_bank = in_gadget_code(pc);
		SampledBlock block;
		/* x28 has already advanced past the dispatched gadget's word. */
		const bool resolved = in_bank && target.active != nullptr &&
			target.active->executor->sample_block(
				reinterpret_cast<const void *>(state.__x[28] - 8), block);
		std::uint64_t guest_thread = 0;
		if (resolved) {
			const auto *thread_pointer = target.active->guest_thread.load(
				std::memory_order_relaxed);
			if (thread_pointer != nullptr)
				guest_thread = *thread_pointer;
		}
		thread_resume(target.thread);

		if (!read)
			return;
		++samples_;
		try {
			if (resolved) {
				auto &entry = blocks_[{block.guest_address,
					hash_words(block), guest_thread}];
				if (entry.count++ == 0) {
					entry.block = block;
					entry.thread = guest_thread;
				}
				++gadgets_[pc];
			} else if (in_bank) {
				++unresolved_;
			} else {
				++host_[pc];
			}
		} catch (...) {
			++dropped_;
		}
	}

	/* A host PC's symbol never changes, and dladdr walks the image's symbol
	 * table on every call: resolving each PC once keeps a periodic dump from
	 * costing the host a core. Only dump() uses it, under dump_mutex_. */
	const std::string &cached_host_symbol(std::uintptr_t pc)
	{
		auto [entry, inserted] = host_symbols_.try_emplace(pc);
		if (inserted)
			entry->second = host_symbol(pc);
		return entry->second;
	}

	void dump() noexcept
	{
		try {
			const std::scoped_lock dump_lock{dump_mutex_};
			std::vector<BlockEntry> blocks;
			std::vector<std::pair<std::uintptr_t, std::uint64_t>> host;
			std::vector<std::pair<std::uintptr_t, std::uint64_t>> gadgets;
			std::uint64_t passes = 0;
			std::uint64_t samples = 0;
			std::uint64_t unresolved = 0;
			std::uint64_t dropped = 0;
			{
				const std::scoped_lock lock{mutex_};
				blocks.reserve(blocks_.size());
				for (const auto &[key, entry] : blocks_)
					blocks.push_back(entry);
				host.assign(host_.begin(), host_.end());
				gadgets.assign(gadgets_.begin(), gadgets_.end());
				passes = passes_;
				samples = samples_;
				unresolved = unresolved_;
				dropped = dropped_;
			}
			std::unordered_map<std::string, std::uint64_t> symbols;
			std::uint64_t host_total = 0;
			for (const auto &[pc, count] : host) {
				symbols[cached_host_symbol(pc)] += count;
				host_total += count;
			}
			std::vector<std::pair<std::uint64_t, std::string>> ranked;
			for (auto &[name, count] : symbols)
				ranked.emplace_back(count, name);
			std::ranges::sort(ranked, std::greater{});
			/* The same samples by the gadget they stopped in, which says
			 * what the translated code spent its time on where the block
			 * lines say which guest code it was. */
			std::unordered_map<std::string, std::uint64_t> gadget_symbols;
			for (const auto &[pc, count] : gadgets)
				gadget_symbols[cached_host_symbol(pc)] += count;
			std::vector<std::pair<std::uint64_t, std::string>> gadget_ranked;
			for (auto &[name, count] : gadget_symbols)
				gadget_ranked.emplace_back(count, name);
			std::ranges::sort(gadget_ranked, std::greater{});
			std::ranges::sort(blocks, [](const auto &a, const auto &b) {
				return a.count > b.count;
			});
			std::uint64_t guest_total = 0;
			for (const auto &entry : blocks)
				guest_total += entry.count;

			const auto temporary = path_ + ".tmp";
			std::FILE *out = std::fopen(temporary.c_str(), "we");
			if (out == nullptr)
				return;
			std::fprintf(out,
				"# guest code samples\tpasses %llu\tsamples %llu\t"
				"guest %llu\tunresolved %llu\thost %llu\tdropped %llu\n",
				static_cast<unsigned long long>(passes),
				static_cast<unsigned long long>(samples),
				static_cast<unsigned long long>(guest_total),
				static_cast<unsigned long long>(unresolved),
				static_cast<unsigned long long>(host_total),
				static_cast<unsigned long long>(dropped));
			std::fprintf(out, "# guest\tcount\taddress\tinstructions\twords\tthread\n");
			for (const auto &entry : blocks) {
				const auto &block = entry.block;
				std::fprintf(out, "guest\t%llu\t%#llx\t%u\t",
					static_cast<unsigned long long>(entry.count),
					static_cast<unsigned long long>(block.guest_address),
					block.instruction_count);
				for (std::uint32_t i = 0; i < block.word_count; ++i)
					std::fprintf(out, "%s%08x", i == 0 ? "" : ",",
						block.words[i]);
				std::fprintf(out, "\t%#llx\n",
					static_cast<unsigned long long>(entry.thread));
			}
			std::fprintf(out, "# host\tcount\tsymbol\n");
			for (const auto &[count, name] : ranked)
				std::fprintf(out, "host\t%llu\t%s\n",
					static_cast<unsigned long long>(count),
					name.c_str());
			std::fprintf(out, "# gadget\tcount\tsymbol\n");
			for (const auto &[count, name] : gadget_ranked)
				std::fprintf(out, "gadget\t%llu\t%s\n",
					static_cast<unsigned long long>(count),
					name.c_str());
			std::fclose(out);
			std::rename(temporary.c_str(), path_.c_str());
		} catch (...) {
		}
	}

	std::string path_;
	std::chrono::nanoseconds period_;
	std::mutex dump_mutex_;
	std::unordered_map<std::uintptr_t, std::string> host_symbols_;
	std::mutex mutex_;
	std::vector<std::unique_ptr<GuestSamplingSlot>> slots_;
	std::vector<Target> targets_;
	std::unordered_map<BlockKey, BlockEntry, BlockKeyHash> blocks_;
	std::unordered_map<std::uintptr_t, std::uint64_t> host_;
	std::unordered_map<std::uintptr_t, std::uint64_t> gadgets_;
	std::uint64_t passes_{};
	std::uint64_t samples_{};
	std::uint64_t unresolved_{};
	std::uint64_t dropped_{};
	std::atomic_bool stop_{};
	std::thread worker_;
};

/* Never destroyed: carriers may still enter and leave while the process
 * exits, and their slots live in the sampler. */
Sampler *sampler() noexcept
{
	static Sampler *const instance = []() -> Sampler * {
#ifdef UML_APP_STORE_RUNTIME
		const char *setting = nullptr;
#else
		const char *setting = std::getenv("DARWIN_UML_GUEST_SAMPLES");
#endif
		if (setting == nullptr || *setting == '\0')
			return nullptr;
		std::string path = setting;
		unsigned hz = 1000;
		if (const auto colon = path.rfind(':'); colon != std::string::npos) {
			hz = static_cast<unsigned>(
				std::strtoul(path.c_str() + colon + 1, nullptr, 10));
			path.erase(colon);
		}
		try {
			auto *created = new Sampler{std::move(path), hz};
			std::atexit([] { sampler()->finish(); });
			return created;
		} catch (...) {
			return nullptr;
		}
	}();
	return instance;
}

} // namespace

GuestSamplingSlot *guest_sampling_register(
	const SampledExecutor &executor) noexcept
{
	auto *instance = sampler();
	if (instance == nullptr)
		return nullptr;
	try {
		return instance->add(executor);
	} catch (...) {
		return nullptr;
	}
}

void guest_sampling_enter(GuestSamplingSlot &slot) noexcept
{
	sampler()->bind(slot);
}

void guest_sampling_leave(GuestSamplingSlot &slot) noexcept
{
	slot.guest_thread.store(nullptr, std::memory_order_relaxed);
	slot.active.store(false, std::memory_order_release);
}

void guest_sampling_thread(
	GuestSamplingSlot &slot, const std::uint64_t *thread) noexcept
{
	slot.guest_thread.store(thread, std::memory_order_relaxed);
}

void guest_sampling_unregister(GuestSamplingSlot &slot) noexcept
{
	sampler()->remove(slot);
}

} // namespace uml::cpu::arm64::gadgets::diagnostics

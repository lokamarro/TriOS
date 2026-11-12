// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/engine.hpp"
#include "execution_engine.hpp"
#include "memory/mmu/word_shared_mutex.hpp"
#include "platform/host_capabilities.hpp"
#include "platform/run_routing.hpp"
#include "x86_tier.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <utility>

namespace uml::runtime {

struct ThreadExecutionState;
struct ThreadStateStore;

using GuestEntry = int (*)(void *, void *, unsigned long, unsigned long,
	unsigned long *);

class Context;

/* How the guest kernel said its run is ending (darwin_uml_guest_shutdown).
 * The values are the bridge's DARWIN_UML_SHUTDOWN_* kinds, which bridge.cpp
 * checks; none means the kernel said nothing. */
enum class GuestShutdownKind : int {
	none = 0,
	power_off = 1,
	halt = 2,
	restart = 3,
};

/* The most of a restart command a record keeps. */
inline constexpr std::size_t guest_shutdown_command_limit = 255;

struct GuestShutdown final {
	GuestShutdownKind kind{GuestShutdownKind::none};
	/* The restart command cut to guest_shutdown_command_limit bytes and
	 * NUL-terminated; empty when the guest gave none. */
	char command[guest_shutdown_command_limit + 1]{};
};

/* Pins one host thread's execution object and the Context memory domain that
 * backs its translated caches. Run transitions and Context destruction wait
 * for every outstanding access; request_stop() remains independent so a guest
 * entry holding this access can still be interrupted. */
class ThreadExecutionStateAccess final {
public:
	ThreadExecutionStateAccess(ThreadExecutionStateAccess &&) noexcept = default;
	ThreadExecutionStateAccess &operator=(
		ThreadExecutionStateAccess &&) noexcept = delete;

	ThreadExecutionStateAccess(const ThreadExecutionStateAccess &) = delete;
	ThreadExecutionStateAccess &operator=(
		const ThreadExecutionStateAccess &) = delete;

	[[nodiscard]] ThreadExecutionState *operator->() const noexcept
	{
		return execution_.get();
	}

	[[nodiscard]] ThreadExecutionState &operator*() const noexcept
	{
		return *execution_;
	}

private:
	friend class Context;

	ThreadExecutionStateAccess(
		std::shared_lock<memory::WordSharedMutex> lifecycle,
		std::shared_ptr<ThreadExecutionState> execution) noexcept
		: lifecycle_{std::move(lifecycle)}, execution_{std::move(execution)}
	{
	}

	std::shared_lock<memory::WordSharedMutex> lifecycle_;
	std::shared_ptr<ThreadExecutionState> execution_;
};

class Context final {
public:
	Context() noexcept;
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	[[nodiscard]] bool begin_run() noexcept;
	void end_run() noexcept;
	void request_stop() noexcept;
	[[nodiscard]] bool stop_requested() const noexcept;
	[[nodiscard]] const std::atomic_bool *stop_token() const noexcept;
	[[nodiscard]] int stop_notification_fd() const noexcept;

	[[nodiscard]] int set_execution_engine(ExecutionEngine engine) noexcept;
	[[nodiscard]] ExecutionEngine execution_engine() const noexcept;
	void set_top_byte_ignore(bool enabled) noexcept;
	[[nodiscard]] bool top_byte_ignore() const noexcept;
	/* Instructions an x86-64 address space runs on the gadget engine
	 * before the JIT takes it over; zero runs the JIT from the first. It
	 * applies to a run that selected the x86-64 JIT. */
	void set_x86_jit_tier(std::uint64_t instructions) noexcept;
	[[nodiscard]] std::uint64_t x86_jit_tier() const noexcept;
	/* The tier state of each address space of the run, for a nonzero
	 * threshold. */
	[[nodiscard]] X86TierTable *x86_tier_table(
		X86TierTable::Live live) noexcept;
	[[nodiscard]] GuestEntry guest_entry() const noexcept;
	[[nodiscard]] std::uint64_t memory_domain() const noexcept;

	/* The guest kernel's report of how its run ends, recorded before the
	 * kernel shuts its drivers down or runs its exitcalls, so that it stays
	 * on record if one never returns. The kernel reports twice, from a reboot
	 * notifier and again from machine_restart(), machine_halt() or
	 * machine_power_off(); a later report replaces an earlier one, as the
	 * later shutdown call is the one that ends the run. begin_run() clears
	 * the record and end_run() keeps it, so the owner reads it once the run
	 * has returned and until the next run begins. Returns whether the report
	 * changed the record, which a repeat of the same report does not. */
	bool record_guest_shutdown(GuestShutdownKind kind,
		std::string_view command) noexcept;
	[[nodiscard]] GuestShutdown guest_shutdown() const noexcept;

	[[nodiscard]] ThreadExecutionStateAccess thread_execution_state();

private:
	void retire_thread_execution_states() noexcept;

	std::atomic<ExecutionEngine> selected_engine_;
	std::atomic<GuestEntry> selected_entry_;
	std::atomic_bool top_byte_ignore_{false};
	std::atomic_uint64_t x86_jit_tier_{0};
	std::once_flag x86_tier_once_;
	std::unique_ptr<X86TierTable> x86_tier_table_;
	std::atomic_uint64_t memory_domain_;
	std::atomic_bool stop_requested_{false};
	int stop_notification_read_{-1};
	int stop_notification_write_{-1};
	mutable std::mutex run_mutex_;
	bool running_{};
	mutable memory::WordSharedMutex execution_lifecycle_mutex_;
	mutable std::mutex states_mutex_;
	std::shared_ptr<ThreadStateStore> state_store_;
	mutable std::mutex shutdown_mutex_;
	GuestShutdown guest_shutdown_;
};

/* Composition-facing run route. The provider endpoint is opaque here so the
 * runtime and device modules continue to depend only on the neutral routing
 * foundation, never on each other. */
class ActiveRunRouteScope final {
public:
	struct DeferredPublication final {};

	ActiveRunRouteScope(Context &context, void *network,
		DeferredPublication) noexcept;
	~ActiveRunRouteScope();

	ActiveRunRouteScope(const ActiveRunRouteScope &) = delete;
	ActiveRunRouteScope &operator=(const ActiveRunRouteScope &) = delete;

	void commit() noexcept;

private:
	::uml::routing::ActiveRouteScope route_;
};

class RunScope final {
public:
	explicit RunScope(Context &context) noexcept;
	~RunScope();

	RunScope(const RunScope &) = delete;
	RunScope &operator=(const RunScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	Context &context_;
	bool entered_;
};

/* A kernel-created vCPU thread has no public handle parameter. It may use a
 * genuinely sole active context as a fallback. While runs overlap, unbound
 * callers receive nullptr and fail closed rather than borrowing either
 * handle's state. Ambiguity remains sticky for the surviving route until the
 * complete overlapping cohort retires. With no active route, every caller
 * receives nullptr. */
[[nodiscard]] Context *current_context() noexcept;

/* The host the virtual CPU presents through `engine`: the prober's
 * capabilities less the parts of the SME profile that engine does not
 * implement (cpu::engine_host_capabilities). The engine translates for it,
 * so its identification registers agree with the guest's HWCAPs. An
 * unresolved (automatic) engine gets the profile both engines implement. */
[[nodiscard]] const ::uml::platform::HostCapabilities &guest_host_capabilities(
	ExecutionEngine engine) noexcept;

/* That of the current context's engine, from which the bridge derives the
 * guest's HWCAPs; with no context, the profile both engines implement. */
[[nodiscard]] const ::uml::platform::HostCapabilities &
current_guest_host_capabilities() noexcept;

/* A process runs one guest. Its guest is spent once a run's kernel has claimed
 * guest RAM, which the host hands out once per process and never takes back. */
[[nodiscard]] bool process_guest_spent() noexcept;

} // namespace uml::runtime

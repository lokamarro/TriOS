// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "device.hpp"
#include "options.hpp"
#include "context.hpp"
#include "guest_sync.hpp"
#include "guest_console.hpp"
#include "guest_hand_back.hpp"
#include "guest_hibernation.hpp"
#include "guest_statistics.hpp"
#include "kernel_signal_scope.hpp"
#include "native_kernel_layout.hpp"
#include "session_record.hpp"
#include "thread_fence.hpp"
#include "virtual_jetsam.hpp"
#include "testing.hpp"
#include "uml_darwin_abi.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if UML_EMULATOR_HAS_KERNEL_RUNNER
extern "C" {
#include "elf_loader.h"
}
#endif

#include <cerrno>
#include <cstdio>
#include <unistd.h>

#if UML_EMULATOR_TESTING
#include <cstring>
#endif

namespace uml::emulator {

namespace {

/* The guest reports that its writeback started, not that it finished. This is
 * the bound the host waits before flushing what the guest handed it: long
 * enough for an ordinary writeback to reach the block layer, short enough to
 * fit inside the few seconds an application has when it is being put away. */
constexpr unsigned int guest_writeback_settle_ms = 1500U;

/* A guest about to be suspended hands its cold page cache and free memory to
 * the host first, since it keeps whatever it holds until it runs again. The
 * guest reclaims for at most a second and compacts for at most another, so
 * this also covers the release and the reply. A pass still running when it
 * expires finishes before the guest takes the suspend request queued behind
 * it. */
constexpr unsigned int guest_trim_wait_ms = 3000U;

/* The most a request for the guest's memory figures waits for its reply. The
 * guest answers from its console interrupt, so a reply that has not come by
 * now is not coming soon, and the caller is not held longer for it. */
constexpr std::chrono::milliseconds guest_memory_wait {500};

/* A line written with write(2) alone: it takes no lock and allocates
 * nothing, so it can be written while the guest's threads are held. */
void write_line(const char *before, unsigned number,
	const char *after) noexcept
{
	char line[256];
	std::size_t size = 0;
	const auto append = [&](const char *text) {
		while (*text != '\0' && size < sizeof line - 1)
			line[size++] = *text++;
	};
	append(before);
	char digits[12];
	std::size_t count = 0;
	do {
		digits[count++] = static_cast<char>('0' + number % 10U);
		number /= 10U;
	} while (number != 0U);
	while (count != 0 && size < sizeof line - 1)
		line[size++] = digits[--count];
	append(after);
	line[size++] = '\n';
	(void)!::write(STDERR_FILENO, line, size);
}

/* The fence's condition for the plain hold, with every thread stopped: none
 * of them owns the guest file registry, so the release and the restore of the
 * disk locks cannot wait on a stopped owner. */
[[nodiscard]] bool registry_idle(void *) noexcept
{
	return ::uml::runtime::guest_file_registry_idle();
}

[[nodiscard]] int guest_request_result(
	::uml::device::GuestRequest request) noexcept
{
	switch (request) {
	case ::uml::device::GuestRequest::done:
		return UML_EMULATOR_OK;
	case ::uml::device::GuestRequest::unanswered:
		return -UML_EMULATOR_RUNTIME_ERROR;
	case ::uml::device::GuestRequest::unreachable:
	case ::uml::device::GuestRequest::refused:
		break;
	}
	return -UML_EMULATOR_UNSUPPORTED;
}

/* An embedded kernel maps fixed host addresses, so a process admits one run
 * at a time. A run queued behind it stays cancellable by its handle's stop
 * request. */
std::mutex process_address_space_mutex;
std::condition_variable process_address_space_changed;
bool process_address_space_admitted;

class TranslationCacheRunScope final {
public:
	TranslationCacheRunScope() noexcept = default;

	[[nodiscard]] bool enter(bool required) noexcept
	{
		required_ = required;
		entered_ = !required ||
			::uml::runtime::acquire_translation_cache_run_session();
		return entered_;
	}

	~TranslationCacheRunScope()
	{
		if (required_ && entered_)
			::uml::runtime::release_translation_cache_run_session();
	}

	TranslationCacheRunScope(const TranslationCacheRunScope &) = delete;
	TranslationCacheRunScope &operator=(
		const TranslationCacheRunScope &) = delete;

	[[nodiscard]] bool entered() const noexcept { return entered_; }

private:
	bool required_{};
	bool entered_{};
};

#if UML_EMULATOR_TESTING
/* The region a test declares to be a linked kernel (src/testing.hpp). */
std::atomic<const std::byte *> test_linked_kernel_begin;
std::atomic<std::size_t> test_linked_kernel_size;
#endif

/* Whether `image` is a kernel image the process links into itself
 * (runtime/kernel_signal_scope.hpp): the one kernel image a handle keeps a
 * pointer to instead of a copy, since no caller can free it and a copy would
 * be dirty memory for the handle's life. Any other buffer is the caller's. */
[[nodiscard]] bool is_linked_kernel(std::span<const std::byte> image) noexcept
{
#if UML_EMULATOR_TESTING
	if (const auto begin =
		    test_linked_kernel_begin.load(std::memory_order_acquire);
	    begin != nullptr && !image.empty() && image.data() == begin &&
	    image.size() == test_linked_kernel_size.load(std::memory_order_relaxed))
		return true;
#endif
	return ::uml::runtime::is_embedded_kernel_image(
		image.data(), image.size());
}

#if UML_EMULATOR_TESTING
constexpr char test_kernel_magic[] = "uml-test-kernel";

[[nodiscard]] testing::KernelEntry test_kernel_entry(
	std::span<const std::byte> image) noexcept
{
	testing::KernelEntry entry{};
	if (image.size() != sizeof(test_kernel_magic) + sizeof(entry) ||
	    std::memcmp(image.data(), test_kernel_magic,
		    sizeof(test_kernel_magic)) != 0)
		return nullptr;
	std::memcpy(&entry, image.data() + sizeof(test_kernel_magic),
		sizeof(entry));
	return entry;
}

std::atomic<testing::AddressSpaceAdmissionHook> after_handle_admission_hook;
std::atomic<testing::AddressSpaceAdmissionHook> admission_wait_boundary_hook;
std::atomic<testing::AddressSpaceAdmissionHook> before_gate_release_hook;
std::atomic<void *> address_space_admission_hook_context;
std::atomic<testing::GuestHandBackHook> guest_hand_back_hook;
std::atomic<void *> guest_hand_back_hook_context;

void invoke_admission_hook(
	const std::atomic<testing::AddressSpaceAdmissionHook> &hook) noexcept
{
	if (const auto callback = hook.load(std::memory_order_acquire);
	    callback != nullptr)
			callback(address_space_admission_hook_context.load(
				std::memory_order_acquire));
}

/* A Darwin sanitizer holds the product's macOS window itself (AddressSanitizer
 * keeps its heap at 0x600000000000, ThreadSanitizer reserves everything
 * outside its application ranges), and the process reserves its window for
 * good. A sanitized test variant therefore places runs at the start of
 * ThreadSanitizer's middle application range, which AddressSanitizer leaves to
 * the application too, unless a test places them elsewhere. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UML_EMULATOR_TEST_SANITIZED 1
#endif
#endif
#ifdef UML_EMULATOR_TEST_SANITIZED
constexpr std::uintptr_t default_test_native_window_start = 0x550000000000ULL;
#else
constexpr std::uintptr_t default_test_native_window_start = 0;
#endif
std::atomic<std::uintptr_t> test_native_kernel_window_start{
	default_test_native_window_start};
#endif

/* Where a run's native kernel window goes: the product's placement, or in the
 * test variant the same geometry wherever a test placed the window. The
 * process holds its window for as long as it lives, so a run takes the window
 * it holds or extends it. */
[[nodiscard]] ::uml::runtime::NativeKernelLayout run_native_kernel_layout(
	std::size_t guest_memory) noexcept
{
#if UML_EMULATOR_TESTING
	const auto start = test_native_kernel_window_start.load(
		std::memory_order_acquire);
	auto product = ::uml::runtime::native_kernel_placement(guest_memory);
	if (start != 0 && product.geometry.valid &&
	    product.placement == ::uml::runtime::WindowPlacement::exact) {
		auto &geometry = product.geometry;
		const auto length = geometry.window_end - geometry.window_start;
		geometry.load_bias += start - geometry.window_start;
		geometry.window_start = start;
		geometry.window_end = start + length;
		const auto layout = ::uml::runtime::reserve_process_native_window(
			geometry, product.placement);
		return layout.valid ? layout :
			::uml::runtime::NativeKernelLayout{.reservation_failed = true};
	}
#endif
	return ::uml::runtime::prepare_native_kernel_layout(guest_memory);
}

class ProcessAddressSpaceAdmission final {
public:
	ProcessAddressSpaceAdmission() noexcept = default;

	[[nodiscard]] bool enter(::uml::runtime::Context &context)
	{
		std::unique_lock lock{process_address_space_mutex};
		while (process_address_space_admitted &&
		       !context.stop_requested()) {
#if UML_EMULATOR_TESTING
			invoke_admission_hook(admission_wait_boundary_hook);
#endif
			process_address_space_changed.wait(lock);
		}
		if (context.stop_requested())
			return false;
		process_address_space_admitted = true;
		entered_ = true;
		return true;
	}

	~ProcessAddressSpaceAdmission()
	{
		if (!entered_)
			return;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(before_gate_release_hook);
#endif
		{
			std::lock_guard lock{process_address_space_mutex};
			process_address_space_admitted = false;
		}
		process_address_space_changed.notify_all();
	}

	ProcessAddressSpaceAdmission(const ProcessAddressSpaceAdmission &) = delete;
	ProcessAddressSpaceAdmission &operator=(
		const ProcessAddressSpaceAdmission &) = delete;

private:
	bool entered_{};
};
}

#if UML_EMULATOR_TESTING
namespace testing {

std::vector<std::byte> kernel_image(KernelEntry entry)
{
	std::vector<std::byte> image(sizeof(test_kernel_magic) + sizeof(entry));
	std::memcpy(image.data(), test_kernel_magic, sizeof(test_kernel_magic));
	std::memcpy(image.data() + sizeof(test_kernel_magic), &entry,
		sizeof(entry));
	return image;
}

void set_address_space_admission_hooks(
		AddressSpaceAdmissionHook after_handle_admission,
		AddressSpaceAdmissionHook at_wait_boundary,
		AddressSpaceAdmissionHook before_gate_release, void *context) noexcept
{
	address_space_admission_hook_context.store(context, std::memory_order_release);
	after_handle_admission_hook.store(
		after_handle_admission, std::memory_order_release);
	admission_wait_boundary_hook.store(
		at_wait_boundary, std::memory_order_release);
	before_gate_release_hook.store(
		before_gate_release, std::memory_order_release);
}

void set_linked_kernel(const void *start, std::size_t size) noexcept
{
	test_linked_kernel_size.store(size, std::memory_order_relaxed);
	test_linked_kernel_begin.store(static_cast<const std::byte *>(start),
		std::memory_order_release);
}

void place_native_kernel_window(std::uintptr_t start) noexcept
{
	test_native_kernel_window_start.store(start, std::memory_order_release);
}

void set_guest_hand_back_hook(GuestHandBackHook hook, void *context) noexcept
{
	guest_hand_back_hook_context.store(context, std::memory_order_release);
	guest_hand_back_hook.store(hook, std::memory_order_release);
}

void report_guest_shutdown(::uml::runtime::GuestShutdownKind kind,
	const char *command) noexcept
{
	if (auto *const context = ::uml::runtime::current_context();
	    context != nullptr)
		context->record_guest_shutdown(kind,
			command == nullptr ? std::string_view{} :
				std::string_view{command});
}

NativeKernelWindow native_kernel_window(std::size_t guest_memory) noexcept
{
	const auto layout = run_native_kernel_layout(guest_memory);
	if (!layout.valid)
		return {};
	return {.start = layout.window_start, .end = layout.window_end};
}

} // namespace testing
#endif

class Emulator final {
public:
	explicit Emulator(const uml_emulator_config &config)
		: rootfs_(config.rootfs_path == nullptr ? "" : config.rootfs_path),
		  runtime_{},
		  network_{network_provider(config)},
		  network_context_{network_}
	{
		if (config.kernel_image != nullptr && config.kernel_image_size != 0) {
			const auto image = std::span{
				static_cast<const std::byte *>(config.kernel_image),
				config.kernel_image_size};
			if (is_linked_kernel(image)) {
				/* The process's own kernel section is immutable and outlives
				 * every handle, and a copy of it would be dirty memory for
				 * the handle's life. Nothing a caller can free is borrowed. */
				kernel_view_ = image;
			} else {
				kernel_.assign(image.begin(), image.end());
				kernel_view_ = kernel_;
			}
		}
	}

	Emulator(const Emulator &) = delete;
	Emulator &operator=(const Emulator &) = delete;

	[[nodiscard]] int release_guest()
	{
		ProcessAddressSpaceAdmission process_admission;
		::uml::runtime::RunScope run_scope{runtime_};
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		if (!power_off_pending_)
			return -UML_EMULATOR_UNSUPPORTED;
		if (!process_admission.enter(runtime_))
			return -UML_EMULATOR_RUNTIME_ERROR;
		const int result = hand_back_guest();
		if (result == 0)
			power_off_pending_ = false;
		return result;
	}

	[[nodiscard]] int run(int argc, const char *const argv[],
			      const char *const envp[])
	{
		if (argc < 0 || (argc != 0 && argv == nullptr))
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (kernel_view_.empty())
			return -UML_EMULATOR_UNSUPPORTED;
		ProcessAddressSpaceAdmission process_admission;
		::uml::runtime::RunScope run_scope{runtime_};
		/* Declared after RunScope so the session-owned executor pool drains
		 * first on scope exit. Thread states retain only lightweight JIT
		 * wrappers; Context::end_run() then retires those wrappers and their
		 * memory domain after no pooled cache can still refer to it.
		 */
		TranslationCacheRunScope translation_cache_run;
		if (!run_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		clear_run_end();
		/* A process runs one guest, and that guest may restart itself: this
		 * handle's guest, after a run that ended with a restart, goes back to
		 * the process once the run is admitted. Refuse any other later run
		 * before it names the console, maps a kernel window or reaches a
		 * kernel that would find no guest RAM left to claim. */
		if (!restartable_ && ::uml::runtime::process_guest_spent())
			return -UML_EMULATOR_UNSUPPORTED;
#if UML_EMULATOR_TESTING
		invoke_admission_hook(after_handle_admission_hook);
#endif
		auto prepared = prepare_emulator_options(argc, argv, rootfs_);
		if (!prepared.valid || prepared.arguments.empty())
			return -UML_EMULATOR_INVALID_ARGUMENT;
		if (runtime_.set_execution_engine(prepared.engine) != 0)
			return -UML_EMULATOR_UNSUPPORTED;
		runtime_.set_top_byte_ignore(prepared.top_byte_ignore);
		if (!prepared.informational)
			prepare_guest_hibernation(prepared.guest_hibernation);
		runtime_.set_x86_jit_tier(prepared.jit_tier_instructions);
		const bool jit_run = !prepared.informational &&
			runtime_.execution_engine() ==
				::uml::runtime::ExecutionEngine::jit;
		/* Before any JIT engine exists, so every executor derives its cache
		 * geometry from the same budget. Data-only engines neither allocate
		 * these resources nor claim the process-global configuration.
		 */
		if (jit_run &&
		    !::uml::runtime::set_translation_cache_budget(
				prepared.jit_cache_budget, prepared.guest_cpus,
				prepared.jit_executor_cache_budget))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Before any gadget executor exists, for the same reason; an
		 * x86-64 address space starts on the gadget engine in a JIT run
		 * too. The limit the launch states for a host that sets none comes
		 * first, since the budget derives from it. */
		std::size_t gadget_cache_budget{};
		if (!prepared.informational) {
			::uml::runtime::set_process_memory_limit(
				prepared.memory_limit_bytes);
			gadget_cache_budget = ::uml::runtime::set_gadget_cache_budget(
				prepared.guest_memory_bytes,
				prepared.gadget_cache_budget, prepared.guest_cpus);
		}
		/* A JIT run activates the managed multi-vCPU executor pool. */
		if (!translation_cache_run.enter(jit_run))
			return -UML_EMULATOR_RUNTIME_ERROR;
		auto &arguments = prepared.arguments;
		std::vector<char *> argument_pointers;
		argument_pointers.reserve(arguments.size() + 1);
		for (auto &argument : arguments)
			argument_pointers.push_back(argument.data());
		argument_pointers.push_back(nullptr);

		std::vector<std::string> environment_strings;
		if (envp != nullptr) {
			for (auto entry = envp; *entry != nullptr; ++entry)
				environment_strings.emplace_back(*entry);
		}
		std::vector<char *> environment_pointers;
		environment_pointers.reserve(environment_strings.size() + 1);
		for (auto &entry : environment_strings)
			environment_pointers.push_back(entry.data());
		environment_pointers.push_back(nullptr);
		if (!process_admission.enter(runtime_))
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Another run may have claimed the guest while this one waited for
		 * admission. */
		if (restartable_) {
			if (const int refused = hand_back_guest(); refused != 0)
				return refused;
		} else if (::uml::runtime::process_guest_spent()) {
			return -UML_EMULATOR_UNSUPPORTED;
		}
		/* Readies the guest console's location before the guest binds it,
		 * and removes it when the run returns, however the guest ended. */
		const ::uml::device::GuestConsoleRun console_run{
			!prepared.informational && prepared.host_console_directory};
		const auto native_layout = run_native_kernel_layout(
			prepared.guest_memory_bytes);
		if (!native_layout.valid)
			return native_layout.reservation_failed ?
				-UML_EMULATOR_RUNTIME_ERROR :
				-UML_EMULATOR_INVALID_ARGUMENT;
		const ::uml::runtime::NativeKernelLayoutScope layout_scope{
			native_layout};
		if (!layout_scope.entered())
			return -UML_EMULATOR_RUNTIME_ERROR;
		/* Where the host sets no limit and the launch stated one, a Mac run
		 * says when its footprint goes past it: the line iOS would have
		 * ended the process at. It ends with the run. */
		std::unique_ptr<::uml::runtime::VirtualJetsamWatch> virtual_jetsam;
		if (!prepared.informational)
			virtual_jetsam = ::uml::runtime::start_virtual_jetsam_watch(
				gadget_cache_budget);
		/* One opaque route publishes CPU and network endpoints under one lock.
		 * Its intrusive storage and commit are allocation-free. */
		::uml::runtime::ActiveRunRouteScope route_scope{
			runtime_, &network_context_,
			::uml::runtime::ActiveRunRouteScope::DeferredPublication{}};
		route_scope.commit();

		/* An informational run prints and returns: it ran no guest, so it
		 * has no end to record. */
		std::optional<RunEndRecorder> run_end_recorder;
#if UML_EMULATOR_TESTING
		if (const auto entry = test_kernel_entry(kernel_view_);
		    entry != nullptr) {
			if (!prepared.informational) {
				::uml::runtime::set_memory_terms_reader(
					&::uml::runtime::memory_terms);
				(void)::uml::runtime::begin_session_record(
					prepared.session_record_path, arguments);
				run_end_recorder.emplace(*this);
			}
			/* The kernel's own process state is the run's to restore, as
			 * the loader restores it around a real kernel. */
			const ::uml::runtime::KernelSignalScope kernel_signals{0, 0};
			return entry(static_cast<int>(arguments.size()),
				argument_pointers.data(), environment_pointers.data());
		}
#endif
#if UML_EMULATOR_HAS_KERNEL_RUNNER
		if (!prepared.informational) {
			::uml::runtime::set_memory_terms_reader(
				&::uml::runtime::memory_terms);
			(void)::uml::runtime::begin_session_record(
				prepared.session_record_path, arguments);
			run_end_recorder.emplace(*this);
		}
		const auto result = uml_elf_run(kernel_view_.data(),
			kernel_view_.size(), native_layout.load_bias,
			native_layout.reserved ? native_layout.window_start : 0U,
			native_layout.reserved ? native_layout.window_end : 0U,
			static_cast<int>(arguments.size()), argument_pointers.data(),
			environment_pointers.data());
		return result;
#else
		return -UML_EMULATOR_UNSUPPORTED;
#endif
	}

	[[nodiscard]] bool has_embedded_kernel() const noexcept
	{
		return !kernel_view_.empty();
	}

	void request_stop() noexcept
	{
		/* A stop reaches threads that run. */
		bring_back_guest_ram();
		/* Predicate mutation and wait enrollment share this mutex, so a
		 * notification cannot land between a false check and wait. */
		{
			std::lock_guard lock{process_address_space_mutex};
			runtime_.request_stop();
		}
		process_address_space_changed.notify_all();
	}

	/* `resident` keeps the guest's RAM in the process: nothing is hibernated,
	 * and the threads are held by the handle's own fence. */
	[[nodiscard]] int quiesce(bool resident = false) noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		if (guest_suspension_ == ::uml::device::GuestSuspension::none) {
			(void)::uml::device::trim_guest_page_cache(
				guest_trim_wait_ms);
			guest_suspension_ = ::uml::device::suspend_guest(
				guest_writeback_settle_ms);
		}
		const auto result = ::uml::runtime::sync_guest_files();
		/* Last, with the guest asleep and its files on disk. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::system)
			settle_suspended_guest(resident);
		/* The files the host could not flush, as the API reports them. A
		 * lock it could not release is no file left unflushed, and is
		 * logged where it happens. */
		return static_cast<int>(result.failed);
	}

	void resume() noexcept
	{
		/* A hibernation still writing leaves what it has not reached in the
		 * process. */
		request_hibernation_abort();
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* The disks, the RAM and the threads are back before the guest is
		 * woken. A restore that fails ends the process and does not
		 * return; where the end is replaced, nothing is woken either. */
		if (!bring_back_guest_locked())
			return;
		const auto suspension = std::exchange(guest_suspension_,
			::uml::device::GuestSuspension::none);
		::uml::device::resume_guest(suspension);
		if (const auto enabled = std::exchange(pending_page_cache_trim_,
			    std::nullopt))
			(void)::uml::device::set_guest_page_cache_trim(*enabled);
	}

	[[nodiscard]] int set_page_cache_trimming(bool enabled) noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* Any console request ends a suspend, so a quiesced guest
		 * receives the change when it is resumed. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none) {
			pending_page_cache_trim_ = enabled;
			return UML_EMULATOR_OK;
		}
		pending_page_cache_trim_.reset();
		return guest_request_result(
			::uml::device::set_guest_page_cache_trim(enabled));
	}

	[[nodiscard]] int trim_page_cache() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* Quiescing already trimmed it, and a request would wake it. */
		if (guest_suspension_ != ::uml::device::GuestSuspension::none)
			return UML_EMULATOR_OK;
		return guest_request_result(
			::uml::device::trim_guest_page_cache(guest_trim_wait_ms));
	}

	/* The figures of this handle's running guest: its route is the one
	 * process-wide active run, which is committed once the run is admitted
	 * and withdrawn before it returns. Nothing here waits for the guest. */
	[[nodiscard]] int read_statistics(uml_emulator_statistics &statistics)
		const noexcept
	{
		if (::uml::runtime::current_context() != &runtime_)
			return -UML_EMULATOR_UNSUPPORTED;
		const auto cpu = ::uml::runtime::guest_cpu_time();
		if (!cpu.listed)
			return -UML_EMULATOR_RUNTIME_ERROR;
		const auto ram = ::uml::runtime::guest_ram_usage();
		statistics = {
			.struct_size = sizeof(statistics),
			.guest_cpu_count = cpu.carriers,
			.guest_cpu_time_ns = cpu.nanoseconds,
			.guest_ram_bytes = ram ? ram->ram_bytes : 0U,
			.guest_ram_held_bytes = ram ? ram->held_bytes : 0U,
		};
		return UML_EMULATOR_OK;
	}

	/* A console request wakes a suspended guest, so it is sent only under
	 * the lifecycle mutex, which says whether the host has quiesced the
	 * guest, and never while another lifecycle call holds it: that call
	 * would otherwise be waited for, or wait for this one. The wait for the
	 * reply is outside the mutex, so a quiesce that starts after the send
	 * does not wait for it. */
	[[nodiscard]] int read_guest_memory(
		uml_emulator_guest_memory &memory) noexcept
	{
		std::unique_lock lock{guest_lifecycle_mutex_, std::try_to_lock};
		if (!lock.owns_lock() ||
			guest_suspension_ != ::uml::device::GuestSuspension::none)
			return -UML_EMULATOR_UNSUPPORTED;
		auto query = ::uml::device::GuestMemoryQuery::start();
		lock.unlock();
		if (!query)
			return -UML_EMULATOR_UNSUPPORTED;
		::uml::device::GuestMemoryFigures figures;
		switch (query->await(guest_memory_wait, figures)) {
		case ::uml::device::GuestMemoryResult::done:
			memory = {
				.struct_size = sizeof(memory),
				.reserved = 0U,
				.total_bytes = figures.total_bytes,
				.available_bytes = figures.available_bytes,
			};
			return UML_EMULATOR_OK;
		case ::uml::device::GuestMemoryResult::unreachable:
		case ::uml::device::GuestMemoryResult::refused:
			return -UML_EMULATOR_UNSUPPORTED;
		case ::uml::device::GuestMemoryResult::unanswered:
		case ::uml::device::GuestMemoryResult::malformed:
			break;
		}
		return -UML_EMULATOR_RUNTIME_ERROR;
	}

	void wake() noexcept
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* A quiesced guest stays put until the host resumes it. */
		if (guest_suspension_ == ::uml::device::GuestSuspension::none)
			(void)::uml::device::wake_guest();
	}

	[[nodiscard]] ::uml::runtime::ExecutionEngine
	execution_engine() const noexcept
	{
		return runtime_.execution_engine();
	}

	[[nodiscard]] uml_emulator_run_end run_end() const noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		return run_end_;
	}

#if UML_EMULATOR_TESTING
	/* Whether the handle runs the linked kernel section and holds no copy. */
	[[nodiscard]] bool kernel_image_borrowed() const noexcept
	{
		return kernel_.empty() && !kernel_view_.empty();
	}

	void use_hibernation(
		std::unique_ptr<::uml::runtime::HibernationSteps> steps,
		::uml::runtime::GuestHibernation::FailureHandler on_failure)
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		publish_hibernation(nullptr);
		hibernation_ = std::make_unique<::uml::runtime::GuestHibernation>(
			std::move(steps), on_failure);
		publish_hibernation(hibernation_.get());
	}
#endif

private:
	/* Hands the process's guest back, for the run that boots it again after a
	 * restart and for an explicit release after a power-off. A refusal
	 * changes nothing the caller cannot repeat: the guest stays to be handed
	 * back by a later run or release, once the caller has stopped what holds
	 * it. */
	[[nodiscard]] int hand_back_guest() noexcept
	{
		switch (::uml::runtime::guest_hand_back_block()) {
		case ::uml::runtime::GuestHandBackBlock::none:
			break;
		case ::uml::runtime::GuestHandBackBlock::borrowed_ram:
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest cannot be handed back: a host "
				"service still borrows its RAM; stop the device "
				"services first\n");
			return -UML_EMULATOR_RUNTIME_ERROR;
		case ::uml::runtime::GuestHandBackBlock::kernel_threads:
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest cannot be handed back: a kernel "
				"thread is still running\n");
			return -UML_EMULATOR_RUNTIME_ERROR;
		}
		/* The handle's state describes this kernel and its RAM; the
		 * hibernation holds the commitment the hand-back retires. */
		reset_guest_state();
		::uml::runtime::GuestHandBack hand_back;
		bool handed_back;
		{
			/* The commitment of the RAM is deleted in here, and a reader of
			 * the statistics must not meet it half gone. */
			const ::uml::runtime::GuestRamRetirementScope retirement;
			handed_back = ::uml::runtime::hand_back_guest(hand_back);
		}
		if (!handed_back) {
			(void)::dprintf(STDERR_FILENO,
				"Darwin: the guest could not be handed back: "
				"errno=%d\n", errno);
			return -UML_EMULATOR_RUNTIME_ERROR;
		}
		const auto &released = hand_back.released;
		(void)::dprintf(STDERR_FILENO,
			"Darwin: guest handed back: descriptors=%zu keys=%zu "
			"allocations=%zu (%zu bytes) mappings=%zu (%zu bytes) "
			"sbrk=%zu guest_files=%zu heaps=%zu (%zu bytes) "
			"leases=%zu\n",
			released.descriptors, released.keys, released.allocations,
			released.allocation_bytes, released.mappings,
			released.mapping_bytes, released.sbrk_calls,
			hand_back.guest_files, hand_back.heaps,
			hand_back.heap_bytes, hand_back.heap_leases);
		restartable_ = false;
#if UML_EMULATOR_TESTING
		if (const auto hook = guest_hand_back_hook.load(
			    std::memory_order_acquire); hook != nullptr)
			hook(guest_hand_back_hook_context.load(
				std::memory_order_acquire),
				{.descriptors = released.descriptors,
				 .keys = released.keys,
				 .allocations = released.allocations,
				 .allocation_bytes = released.allocation_bytes,
				 .mappings = released.mappings,
				 .mapping_bytes = released.mapping_bytes,
				 .sbrk_calls = released.sbrk_calls,
				 .guest_files = hand_back.guest_files,
				 .heaps = hand_back.heaps,
				 .heap_bytes = hand_back.heap_bytes,
				 .heap_leases = hand_back.heap_leases});
#endif
		return 0;
	}

	/* What the handle remembers of its kernel: a suspension, a hibernation
	 * of its RAM, a page-cache request waiting for the resume. The next
	 * kernel starts without any of them. The hibernation image of the RAM is
	 * dropped with the hibernation, and the next boot makes its own. */
	void reset_guest_state() noexcept
	{
		request_hibernation_abort();
		std::lock_guard lock{guest_lifecycle_mutex_};
		/* A stop or a resume on another thread may be inside the abort
		 * request: withdrawing the pointer waits for it to return, and
		 * none starts after, so the object is destroyed unobserved. */
		publish_hibernation(nullptr);
		hibernation_.reset();
		if (guest_hold_ == GuestHold::fence)
			guest_fence_->release();
		guest_hold_ = GuestHold::none;
		guest_hold_failure_logged_ = false;
		guest_suspension_ = ::uml::device::GuestSuspension::none;
		guest_file_locks_released_ = false;
		pending_page_cache_trim_.reset();
	}

	/* A run that begins on the handle starts from no end. */
	void clear_run_end() noexcept
	{
		std::lock_guard lock{run_end_mutex_};
		run_end_ = {
			.struct_size = sizeof(run_end_),
			.kind = UML_EMULATOR_RUN_END_NONE,
			.restart_command = {},
		};
	}

	/* Says how the run's guest ended, once its kernel has returned. The host's
	 * own stop request wins over what the guest said on its way out, and a
	 * kernel that said nothing failed. */
	void record_run_end() noexcept
	{
		uml_emulator_run_end end{
			.struct_size = sizeof(end),
			.kind = UML_EMULATOR_RUN_END_FAILED,
			.restart_command = {},
		};
		const auto shutdown = runtime_.guest_shutdown();
		if (runtime_.stop_requested()) {
			end.kind = UML_EMULATOR_RUN_END_STOPPED;
		} else if (shutdown.kind ==
			   ::uml::runtime::GuestShutdownKind::restart) {
			end.kind = UML_EMULATOR_RUN_END_RESTART;
			static_assert(sizeof(end.restart_command) >
				      ::uml::runtime::guest_shutdown_command_limit);
			std::copy_n(shutdown.command,
				    ::uml::runtime::guest_shutdown_command_limit,
				    end.restart_command);
		} else if (shutdown.kind !=
			   ::uml::runtime::GuestShutdownKind::none) {
			end.kind = UML_EMULATOR_RUN_END_POWER_OFF;
		}
		restartable_ = end.kind == UML_EMULATOR_RUN_END_RESTART;
		power_off_pending_ = end.kind == UML_EMULATOR_RUN_END_POWER_OFF &&
			::uml::runtime::process_guest_spent();
		using ::uml::runtime::SessionEnd;
		::uml::runtime::end_session_record(
			end.kind == UML_EMULATOR_RUN_END_STOPPED ? SessionEnd::stopped :
			end.kind == UML_EMULATOR_RUN_END_RESTART ?
				SessionEnd::restarted :
			end.kind == UML_EMULATOR_RUN_END_POWER_OFF ?
				SessionEnd::powered_off : SessionEnd::failed, 0);
		std::lock_guard lock{run_end_mutex_};
		run_end_ = end;
	}

	/* Records the end of the run that launched a kernel, on every way out of
	 * the run, an exception included. */
	class RunEndRecorder final {
	public:
		explicit RunEndRecorder(Emulator &emulator) noexcept
			: emulator_{emulator}
		{
		}
		~RunEndRecorder()
		{
			{
				std::lock_guard lock{emulator_.guest_lifecycle_mutex_};
				(void)::uml::runtime::release_guest_file_locks(true);
				emulator_.guest_file_locks_released_ = false;
			}
			emulator_.record_run_end();
		}
		RunEndRecorder(const RunEndRecorder &) = delete;
		RunEndRecorder &operator=(const RunEndRecorder &) = delete;

	private:
		Emulator &emulator_;
	};

	/* Before the guest boots: what the options say about hibernating, and
	 * the images a process that ended in a hibernation left behind. */
	void prepare_guest_hibernation(::uml::runtime::HibernationMode mode)
	{
		std::lock_guard lock{guest_lifecycle_mutex_};
		hibernation_mode_ = mode;
		if (mode != ::uml::runtime::HibernationMode::off)
			(void)::uml::runtime::remove_stale_hibernation_images(
				::uml::runtime::hibernation_directory());
	}

	/* What a confirmed system suspension has the host do, last, with the guest
	 * asleep and its files on disk. The guest's disks are the host's to keep:
	 * the kernel's own wake sources (the RTC alarm, VNC input) end a suspend
	 * the host asked for, so once the host's locks on the disks are gone,
	 * only a hold on every thread that can run the guest keeps it from
	 * writing them before the resume has locked them again and found them
	 * unchanged. The locks therefore leave only with the threads held, and
	 * stay where they cannot be. */
	void settle_suspended_guest(bool resident) noexcept
	{
		if (!hold_guest_threads(resident)) {
			if (!std::exchange(guest_hold_failure_logged_, true))
				write_line("Darwin: the guest's disk locks stay held: "
					"its threads could not be held (reason ",
					static_cast<unsigned>(guest_hold_failure_), ")");
			return;
		}
		/* The locks leave first: a process holding one when the system
		 * suspends it is ended, and the write can take a while. */
		const unsigned failed = ::uml::runtime::release_guest_file_locks();
		guest_file_locks_released_ = true;
		if (failed != 0U)
			write_line("Darwin: ", failed, " guest disk lock(s) could not "
				"be released; the system may end the process while it "
				"is suspended");
		if (guest_hold_ == GuestHold::hibernation)
			hibernation_->write();
	}

	/* Holds every thread that can touch the guest's RAM or disks until
	 * `release_guest_hold`. The hibernation does it where the RAM is wired
	 * (or a mode forces it), and goes on to write the RAM out; elsewhere, or
	 * for a `resident` quiesce, a fence of the handle's own does, and nothing
	 * is written. False, with everything running, when the threads could not
	 * be held. */
	[[nodiscard]] bool hold_guest_threads(bool resident) noexcept
	{
		if (guest_hold_ != GuestHold::none)
			return true;
		if (!resident && hibernation_ == nullptr &&
		    hibernation_mode_ != ::uml::runtime::HibernationMode::off) {
			try {
				hibernation_ = ::uml::runtime::GuestHibernation::create(
					hibernation_mode_,
					::uml::runtime::hibernation_directory());
			} catch (...) {
				return false;
			}
			publish_hibernation(hibernation_.get());
		}
		if (!resident && hibernation_ != nullptr) {
			switch (hibernation_->hold(
				&::uml::runtime::guest_file_registry_idle)) {
			case ::uml::runtime::HibernationHold::held:
				guest_hold_ = GuestHold::hibernation;
				return true;
			case ::uml::runtime::HibernationHold::failed:
				guest_hold_failure_ = hibernation_->report().hold_failure;
				return false;
			case ::uml::runtime::HibernationHold::unavailable:
				break;
			}
		}
		try {
			if (!guest_fence_)
				guest_fence_ = std::make_unique<::uml::runtime::ThreadFence>();
		} catch (...) {
			return false;
		}
		if (!guest_fence_->hold(::uml::runtime::guest_hold_budget_ms,
			    &registry_idle)) {
			guest_hold_failure_ = guest_fence_->report().failure;
			return false;
		}
		guest_hold_ = GuestHold::fence;
		return true;
	}

	/* Lets the held threads run, after the hibernation has brought the RAM
	 * back. False when it could not: nothing runs then. */
	[[nodiscard]] bool release_guest_hold() noexcept
	{
		if (hibernation_ != nullptr && !hibernation_->restore())
			return false;
		if (guest_hold_ == GuestHold::fence)
			guest_fence_->release();
		guest_hold_ = GuestHold::none;
		guest_hold_failure_logged_ = false;
		return true;
	}

	/* Names the hibernation the abort request may reach, or none. The
	 * request runs without the lifecycle mutex, which a hibernation in
	 * progress holds, so the pointer has a mutex of its own: held across the
	 * request, which only sets a flag, and across the withdrawal that
	 * precedes the object's destruction. */
	void publish_hibernation(
		::uml::runtime::GuestHibernation *hibernation) noexcept
	{
		std::lock_guard lock{hibernation_abort_mutex_};
		hibernation_pointer_ = hibernation;
	}

	void request_hibernation_abort() noexcept
	{
		std::lock_guard lock{hibernation_abort_mutex_};
		if (hibernation_pointer_ != nullptr)
			hibernation_pointer_->request_abort();
	}

	void bring_back_guest_ram() noexcept
	{
		request_hibernation_abort();
		std::lock_guard lock{guest_lifecycle_mutex_};
		(void)bring_back_guest_locked();
	}

	/* The disks locked and verified again, then the RAM, then the threads:
	 * the guest runs on nothing the host has not checked. False when the RAM
	 * could not be brought back. */
	[[nodiscard]] bool bring_back_guest_locked() noexcept
	{
		if (guest_file_locks_released_) {
			::uml::runtime::restore_guest_file_locks();
			guest_file_locks_released_ = false;
		}
		return release_guest_hold();
	}

	[[nodiscard]] static uml::network::Provider network_provider(
		const uml_emulator_config &config) noexcept
	{
		if (config.network != nullptr)
			return {
				.context = config.network->context,
				.open = config.network->open,
				.irq_fd = config.network->irq_fd,
				.read = config.network->read,
				.write = config.network->write,
			};
		return {};
	}

	std::string rootfs_;
	/* The handle's own copy of the kernel image; empty when it borrows. */
	std::vector<std::byte> kernel_;
	/* The image the handle runs: `kernel_`, or the linked kernel section. */
	std::span<const std::byte> kernel_view_;
	::uml::runtime::Context runtime_;
	uml::network::Provider network_;
	uml::network::ProviderContext network_context_;
	std::mutex guest_lifecycle_mutex_;
	bool guest_file_locks_released_{};
	/* What holds the quiesced guest's threads: its hibernation, or a fence
	 * of the handle's own where the RAM is not hibernated. Guarded by the
	 * lifecycle mutex, like the fence. */
	enum class GuestHold { none, hibernation, fence };
	GuestHold guest_hold_{GuestHold::none};
	std::unique_ptr<::uml::runtime::ThreadFence> guest_fence_;
	int guest_hold_failure_{};
	bool guest_hold_failure_logged_{};
	::uml::device::GuestSuspension guest_suspension_ {
		::uml::device::GuestSuspension::none};
	/* A trimming change requested while the guest was quiesced. */
	std::optional<bool> pending_page_cache_trim_;
	::uml::runtime::HibernationMode hibernation_mode_ {
		::uml::runtime::HibernationMode::automatic};
	/* Made by the first quiesce that hibernates. Guarded by the lifecycle
	 * mutex; the pointer is also published, under its own mutex, for the
	 * abort request, which a stop or a resume sends before it waits for the
	 * lifecycle mutex. The mutex is a leaf: nothing is taken under it but the
	 * request itself. */
	std::unique_ptr<::uml::runtime::GuestHibernation> hibernation_;
	std::mutex hibernation_abort_mutex_;
	::uml::runtime::GuestHibernation *hibernation_pointer_{};
	/* The handle's last run ended with a restart, so its guest goes back to
	 * the process when the handle runs again. Only the handle's one run at a
	 * time reads or writes it. */
	bool restartable_{};
	/* Only the handle that owns a cleanly powered-off guest can release it.
	 * Clearing this on success prevents an old handle from releasing a later
	 * guest belonging to another handle. */
	bool power_off_pending_{};
	/* How the handle's most recent run ended, for uml_emulator_get_run_end. */
	mutable std::mutex run_end_mutex_;
	uml_emulator_run_end run_end_ {
		.struct_size = sizeof(uml_emulator_run_end),
		.kind = UML_EMULATOR_RUN_END_NONE,
		.restart_command = {},
	};
};

} // namespace uml::emulator

struct uml_emulator {
	explicit uml_emulator(const uml_emulator_config &config) : implementation(config) {}

	uml::emulator::Emulator implementation;
};

#if UML_EMULATOR_TESTING
namespace uml::emulator::testing {

void use_hibernation(uml_emulator *emulator,
	std::unique_ptr<::uml::runtime::HibernationSteps> steps,
	void (*on_failure)() noexcept)
{
	emulator->implementation.use_hibernation(std::move(steps), on_failure);
}

bool kernel_image_borrowed(const uml_emulator *emulator) noexcept
{
	return emulator->implementation.kernel_image_borrowed();
}

} // namespace uml::emulator::testing
#endif

extern "C" int uml_emulator_create(const uml_emulator_config *config,
				  uml_emulator **result)
{
	if (result == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*result = nullptr;
	if (config == nullptr ||
	    config->abi_version != UML_EMULATOR_ABI_VERSION ||
	    config->struct_size < sizeof(*config) ||
	    (config->kernel_image == nullptr && config->kernel_image_size != 0) ||
	    (config->network != nullptr &&
	     (config->network->struct_size < sizeof(*config->network) ||
	      config->network->open == nullptr ||
	      config->network->irq_fd == nullptr ||
	      config->network->read == nullptr ||
	      config->network->write == nullptr)))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	std::unique_ptr<uml_emulator> emulator;
	try {
		emulator.reset(new uml_emulator(*config));
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
	*result = emulator.release();
	return UML_EMULATOR_OK;
}

extern "C" void uml_emulator_destroy(uml_emulator *emulator)
{
	std::unique_ptr<uml_emulator>{emulator};
}

extern "C" int uml_emulator_get_capabilities(
	const uml_emulator *emulator, uml_emulator_capabilities *capabilities)
{
	if (emulator == nullptr || capabilities == nullptr ||
	    capabilities->struct_size < sizeof(*capabilities))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	capabilities->abi_version = UML_EMULATOR_ABI_VERSION;
	capabilities->userspace_execution =
		static_cast<uml_emulator_execution_model>(
			emulator->implementation.execution_engine());
	const bool elf_kernel = UML_EMULATOR_HAS_KERNEL_RUNNER &&
		emulator->implementation.has_embedded_kernel();
	capabilities->can_run = elf_kernel;
	capabilities->sandbox_safe = !elf_kernel;
	capabilities->platform = UML_APPLE_PLATFORM_NAME;
	return UML_EMULATOR_OK;
}

extern "C" int uml_emulator_run(uml_emulator *emulator, int argc,
			       const char *const argv[], const char *const envp[])
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	try {
		return emulator->implementation.run(argc, argv, envp);
	} catch (const std::bad_alloc &) {
		return -UML_EMULATOR_OUT_OF_MEMORY;
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
}

extern "C" int uml_emulator_get_run_end(const uml_emulator *emulator,
					uml_emulator_run_end *end)
{
	if (emulator == nullptr || end == nullptr ||
	    end->struct_size < sizeof(*end))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	*end = emulator->implementation.run_end();
	return UML_EMULATOR_OK;
}

extern "C" int uml_emulator_release_guest(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	try {
		return emulator->implementation.release_guest();
	} catch (...) {
		return -UML_EMULATOR_RUNTIME_ERROR;
	}
}

extern "C" void uml_emulator_request_stop(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.request_stop();
}

extern "C" int uml_emulator_sync(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	/* Ask the guest first. Only it can write back its own page cache, and
	 * flushing the host before it would push down a prefix of what the guest
	 * still holds. A guest without a management console simply does not
	 * answer, which leaves the host flush doing what it did before. */
	(void)::uml::device::request_guest_writeback(
		::uml::emulator::guest_writeback_settle_ms);
	const auto result = ::uml::runtime::sync_guest_files();
	return static_cast<int>(result.failed);
}

extern "C" int uml_emulator_quiesce(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.quiesce();
}

extern "C" int uml_emulator_quiesce_resident(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.quiesce(true);
}

extern "C" void uml_emulator_resume(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.resume();
}

extern "C" void uml_emulator_wake(uml_emulator *emulator)
{
	if (emulator != nullptr)
		emulator->implementation.wake();
}

extern "C" int uml_emulator_set_page_cache_trimming(uml_emulator *emulator,
	int enabled)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.set_page_cache_trimming(enabled != 0);
}

extern "C" int uml_emulator_trim_page_cache(uml_emulator *emulator)
{
	if (emulator == nullptr)
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.trim_page_cache();
}

extern "C" int uml_emulator_get_statistics(const uml_emulator *emulator,
	uml_emulator_statistics *statistics)
{
	if (emulator == nullptr || statistics == nullptr ||
		statistics->struct_size < sizeof(*statistics))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.read_statistics(*statistics);
}

extern "C" int uml_emulator_get_guest_memory(uml_emulator *emulator,
	uml_emulator_guest_memory *memory)
{
	if (emulator == nullptr || memory == nullptr ||
		memory->struct_size < sizeof(*memory))
		return -UML_EMULATOR_INVALID_ARGUMENT;
	return emulator->implementation.read_guest_memory(*memory);
}

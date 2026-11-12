// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_hibernation.hpp"

#include "memory/guest_ram_commitment.hpp"
#include "session_record.hpp"
#include "thread_fence.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mach/mach.h>
#include <new>
#include <unistd.h>
#include <sysexits.h>

namespace uml::runtime {
namespace {

using memory::GuestRamCommitment;
using memory::GuestRamImage;
using memory::ImageStats;

constexpr std::uint64_t mib = 1024U * 1024U;
constexpr unsigned image_threads = 4;

[[nodiscard]] std::uint64_t now_ms() noexcept
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ULL;
}

/* What the system holds the process to. A system call, no allocation. */
[[nodiscard]] std::uint64_t task_footprint() noexcept
{
	task_vm_info_data_t information{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&information), &count) !=
	    KERN_SUCCESS)
		return 0;
	return information.phys_footprint;
}

/* One line for the log, built in place and written with write(2): it takes no
 * lock and allocates nothing, so it is safe while threads are held. */
class Line final {
public:
	Line &text(const char *value) noexcept
	{
		const std::size_t length = std::strlen(value);
		const std::size_t room = sizeof buffer_ - 2 - size_;
		const std::size_t take = std::min(length, room);
		std::memcpy(buffer_ + size_, value, take);
		size_ += take;
		return *this;
	}

	Line &number(std::uint64_t value) noexcept
	{
		char digits[24];
		std::size_t count = 0;
		do {
			digits[count++] = static_cast<char>('0' + value % 10);
			value /= 10;
		} while (value != 0);
		while (count != 0 && size_ < sizeof buffer_ - 2)
			buffer_[size_++] = digits[--count];
		return *this;
	}

	void emit() noexcept
	{
		buffer_[size_++] = '\n';
		(void)!write(STDERR_FILENO, buffer_, size_);
		size_ = 0;
	}

private:
	char buffer_[640];
	std::size_t size_{};
};

/* What the fence asks with every thread stopped: the caller's condition
 * first, which changes nothing, and then the freeze of the commitment. */
struct Settling final {
	GuestRamImage *image;
	HoldCondition condition;
};

[[nodiscard]] bool settled(void *context) noexcept
{
	const auto *const settling = static_cast<const Settling *>(context);
	return (settling->condition == nullptr || settling->condition()) &&
		settling->image->freeze_changes();
}

class RealSteps final : public HibernationSteps {
public:
	RealSteps(HibernationMode mode, std::string directory)
		: mode_(mode), directory_(std::move(directory)) {}

	bool prepare() noexcept override
	{
		if (mode_ == HibernationMode::off || given_up_)
			return false;
		GuestRamCommitment *const ram = memory::guest_ram_commitment();
		if (ram == nullptr || !hibernation_applies(mode_, ram->wires()))
			return false;
		try {
			if (!fence_)
				fence_ = std::make_unique<ThreadFence>();
			if (!image_) {
				GuestRamImage::Options options;
				options.path = directory_ + "/" +
					memory::guest_ram_image_name(static_cast<int>(getpid()));
				options.threads = image_threads;
				image_ = GuestRamImage::create(*ram, options);
				if (!image_) {
					const int error = errno;
					given_up_ = true;
					Line{}.text("uml: guest RAM hibernation is off: no image "
						"at ").text(options.path.c_str()).text(" (errno ")
						.number(static_cast<std::uint64_t>(error)).text(")")
						.emit();
					return false;
				}
				image_pointer_.store(image_.get(), std::memory_order_release);
			}
		} catch (const std::bad_alloc &) {
			given_up_ = true;
			return false;
		}
		return true;
	}

	bool hold(HibernationReport &report, HoldCondition condition) noexcept override
	{
		const std::uint64_t started = now_ms();
		Settling settling{image_.get(), condition};
		const bool held = fence_->hold(guest_hold_budget_ms, &settled,
			&settling);
		const ThreadFence::Report &fence = fence_->report();
		report.threads_held = fence.held;
		report.threads_named = fence.named;
		report.threads_registered = fence.registered;
		report.hold_passes = fence.passes;
		report.hold_deferred = fence.deferred;
		report.hold_unsettled = fence.unsettled;
		report.hold_failure = fence.failure;
		report.hold_ms = now_ms() - started;
		return held;
	}

	bool write(HibernationReport &report) noexcept override
	{
		const std::uint64_t started = now_ms();
		report.footprint_before = task_footprint();
		const bool complete = image_->hibernate(report.written);
		report.write_ms = now_ms() - started;
		report.footprint_after = task_footprint();
		return complete;
	}

	bool restore(HibernationReport &report) noexcept override
	{
		const std::uint64_t started = now_ms();
		const bool whole = image_->restore(report.restored);
		report.restore_ms = now_ms() - started;
		report.footprint_restored = task_footprint();
		return whole;
	}

	void release() noexcept override
	{
		fence_->release();
	}

	void set_abort(bool abort) noexcept override
	{
		GuestRamImage *const image =
			image_pointer_.load(std::memory_order_acquire);
		if (image == nullptr)
			return;
		if (abort)
			image->request_abort();
		else
			image->clear_abort();
	}

private:
	HibernationMode mode_;
	std::string directory_;
	std::unique_ptr<ThreadFence> fence_;
	std::unique_ptr<GuestRamImage> image_;
	std::atomic<GuestRamImage *> image_pointer_{nullptr};
	bool given_up_{false};
};

} // namespace

void GuestHibernation::end_process() noexcept
{
	end_session_record(SessionEnd::hibernation_restore_failed, EX_SOFTWARE,
		"guest RAM could not be restored after a hibernation");
	_exit(EX_SOFTWARE);
}

std::unique_ptr<GuestHibernation> GuestHibernation::create(
	HibernationMode mode, std::string directory)
{
	return std::make_unique<GuestHibernation>(
		std::make_unique<RealSteps>(mode, std::move(directory)));
}

GuestHibernation::GuestHibernation(std::unique_ptr<HibernationSteps> steps,
	FailureHandler on_failure) noexcept
	: steps_(std::move(steps)), on_failure_(on_failure)
{
}

GuestHibernation::~GuestHibernation() = default;

void GuestHibernation::request_abort() noexcept
{
	steps_->set_abort(true);
}

HibernationHold GuestHibernation::hold(HoldCondition condition) noexcept
{
	if (hibernated_)
		return HibernationHold::held;
	steps_->set_abort(false);
	report_ = {};
	if (!steps_->prepare())
		return HibernationHold::unavailable;
	held_at_ms_ = now_ms();
	if (!steps_->hold(report_, condition)) {
		Line{}.text("uml: guest RAM stays in the process: ")
			.text("could not hold the threads (reason ")
			.number(static_cast<std::uint64_t>(report_.hold_failure))
			.text(", ").number(report_.hold_passes).text(" passes, ")
			.number(report_.hold_deferred).text(" deferred, ")
			.number(report_.hold_unsettled).text(" unsettled, ")
			.number(report_.hold_ms).text(" ms)").emit();
		return HibernationHold::failed;
	}
	/* From here the guest's RAM may leave, and restore has to run. */
	hibernated_ = true;
	written_ = false;
	return HibernationHold::held;
}

void GuestHibernation::write() noexcept
{
	if (!hibernated_ || written_)
		return;
	written_ = true;
	const bool complete = steps_->write(report_);
	const memory::ImageStats &image = report_.written;
	Line line;
	line.text("uml: hibernated guest RAM").text(complete ? "" : " (partly)")
		.text(": ").number(image.evicted_bytes / mib).text(" MiB out, ")
		.number(image.written_bytes / 1024).text(" KiB written, ")
		.number(image.chunks_written).text(" chunks rewritten, ")
		.number(image.chunks_kept).text(" kept, ")
		.number(image.chunks_zero).text(" zero, ")
		.number(image.stored_bytes / mib).text(" MiB stored; ")
		.number(report_.threads_held).text(" threads held (")
		.number(report_.threads_named).text(" named, ")
		.number(report_.threads_registered).text(" registered), hold ")
		.number(report_.hold_ms).text(" ms, write ")
		.number(report_.write_ms).text(" ms, total ")
		.number(now_ms() - held_at_ms_).text(" ms, footprint ")
		.number(report_.footprint_before / mib).text(" -> ")
		.number(report_.footprint_after / mib).text(" MiB");
	if (image.chunks_failed != 0 || image.chunks_skipped != 0)
		line.text("; ").number(image.chunks_failed).text(" failed, ")
			.number(image.chunks_skipped).text(" left (errno ")
			.number(static_cast<std::uint64_t>(image.error)).text(")");
	line.emit();
}

bool GuestHibernation::hibernate(HoldCondition condition) noexcept
{
	if (hold(condition) != HibernationHold::held)
		return false;
	write();
	return true;
}

bool GuestHibernation::restore() noexcept
{
	if (!hibernated_)
		return true;
	if (!steps_->restore(report_)) {
		const memory::ImageStats &image = report_.restored;
		Line{}.text("uml: guest RAM could not be restored: ")
			.number(image.chunks_failed).text(" chunks failed (errno ")
			.number(static_cast<std::uint64_t>(image.error))
			.text("); the process ends so that the next launch boots again")
			.emit();
		on_failure_();
		return false;
	}
	steps_->release();
	hibernated_ = false;
	written_ = false;
	const memory::ImageStats &image = report_.restored;
	Line{}.text("uml: restored guest RAM: ")
		.number(image.evicted_bytes / mib).text(" MiB read back (")
		.number(image.read_bytes / 1024).text(" KiB), ")
		.number(image.wire_failures).text(" wire failures, ")
		.number(report_.restore_ms).text(" ms, footprint ")
		.number(report_.footprint_restored / mib)
		.text(" MiB; threads released").emit();
	return true;
}

std::string hibernation_directory()
{
	const char *const temporary = std::getenv("TMPDIR");
	if (temporary != nullptr && temporary[0] == '/') {
		std::string directory{temporary};
		while (directory.size() > 1 && directory.back() == '/')
			directory.pop_back();
		return directory;
	}
	char buffer[1024];
	const std::size_t length =
		confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof buffer);
	if (length > 1 && length <= sizeof buffer) {
		std::string directory{buffer};
		while (directory.size() > 1 && directory.back() == '/')
			directory.pop_back();
		return directory;
	}
	return "/tmp";
}

std::size_t remove_stale_hibernation_images(const std::string &directory) noexcept
{
	return memory::remove_stale_guest_ram_images(directory);
}

} // namespace uml::runtime

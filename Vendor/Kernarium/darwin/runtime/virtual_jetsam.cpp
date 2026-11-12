// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtual_jetsam.hpp"

#include "platform/host_environment.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mach/mach.h>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <utility>

namespace uml::runtime {

namespace {

/* How often the footprint is looked at. The lifetime peak catches what falls
 * between two looks, so this only bounds how late a line is. */
constexpr std::chrono::milliseconds sample_period{250};

constexpr double mebibytes(std::uint64_t bytes) noexcept
{
	return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

std::optional<VirtualJetsamCrossing> VirtualJetsam::observe(
	std::uint64_t footprint, std::uint64_t peak) noexcept
{
	if (limit_ == 0)
		return std::nullopt;
	if (footprint > limit_) {
		reported_peak_ = std::max(reported_peak_, peak);
		if (over_)
			return std::nullopt;
		over_ = true;
		return VirtualJetsamCrossing{footprint, limit_, peak, false};
	}
	over_ = false;
	if (peak > limit_ && peak > reported_peak_) {
		reported_peak_ = peak;
		return VirtualJetsamCrossing{footprint, limit_, peak, true};
	}
	return std::nullopt;
}

std::size_t format_virtual_jetsam_line(char *buffer, std::size_t size,
	const VirtualJetsamCrossing &crossing) noexcept
{
	if (size == 0)
		return 0;
	const int written = crossing.between_samples ?
		std::snprintf(buffer, size,
			"uml: virtual Jetsam: footprint %.1f MiB is back under the "
			"limit %.1f MiB, but the peak %.1f MiB was over it; iOS would "
			"have ended this process",
			mebibytes(crossing.footprint), mebibytes(crossing.limit),
			mebibytes(crossing.peak)) :
		std::snprintf(buffer, size,
			"uml: virtual Jetsam: footprint %.1f MiB is over the limit "
			"%.1f MiB (peak %.1f MiB); iOS would end this process",
			mebibytes(crossing.footprint), mebibytes(crossing.limit),
			mebibytes(crossing.peak));
	if (written < 0)
		return 0;
	return std::min(static_cast<std::size_t>(written), size - 1);
}

std::size_t format_stated_limit_line(char *buffer, std::size_t size,
	std::uint64_t limit, std::uint64_t gadget_cache_budget) noexcept
{
	if (size == 0)
		return 0;
	constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
	const int written = std::snprintf(buffer, size,
		"uml: memory limit %llu MiB stated by the launch (virtual Jetsam "
		"on); gadget cache budget %llu MiB",
		static_cast<unsigned long long>(limit / mebibyte),
		static_cast<unsigned long long>(gadget_cache_budget / mebibyte));
	if (written < 0)
		return 0;
	return std::min(static_cast<std::size_t>(written), size - 1);
}

FootprintSample sample_process_footprint() noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_VM_INFO_COUNT)
		return {};
	return {info.phys_footprint,
		static_cast<std::uint64_t>(info.ledger_phys_footprint_peak)};
}

struct VirtualJetsamWatch::State final {
	VirtualJetsam monitor;
	std::chrono::milliseconds period;
	Sampler sampler;
	Report report;
	std::mutex mutex;
	std::condition_variable stopped;
	bool stopping{};
	std::thread thread;

	State(std::uint64_t limit, std::chrono::milliseconds interval,
		Sampler sample, Report notify)
		: monitor{limit}, period{interval}, sampler{std::move(sample)},
		  report{std::move(notify)}
	{
	}

	void run()
	{
		(void)pthread_setname_np("uml-virtual-jetsam");
		std::unique_lock lock{mutex};
		do {
			lock.unlock();
			const auto sampled = sampler();
			if (const auto crossing =
				    monitor.observe(sampled.footprint, sampled.peak))
				report(*crossing);
			lock.lock();
		} while (!stopped.wait_for(lock, period, [this] { return stopping; }));
	}
};

VirtualJetsamWatch::VirtualJetsamWatch(std::uint64_t limit,
	std::chrono::milliseconds period, Sampler sampler, Report report)
	: state_{std::make_unique<State>(limit, period, std::move(sampler),
		  std::move(report))}
{
	state_->thread = std::thread{[state = state_.get()] { state->run(); }};
}

VirtualJetsamWatch::~VirtualJetsamWatch()
{
	{
		std::lock_guard lock{state_->mutex};
		state_->stopping = true;
	}
	state_->stopped.notify_all();
	state_->thread.join();
}

std::unique_ptr<VirtualJetsamWatch> start_virtual_jetsam_watch(
	std::size_t gadget_cache_budget)
{
	if (!::uml::platform::process_memory_limit_is_virtual())
		return nullptr;
	const auto limit = ::uml::platform::process_memory_limit_bytes();
	char start[256];
	format_stated_limit_line(start, sizeof start, limit, gadget_cache_budget);
	std::fprintf(stderr, "%s\n", start);
	return std::make_unique<VirtualJetsamWatch>(
		limit, sample_period,
		sample_process_footprint,
		[](const VirtualJetsamCrossing &crossing) {
			char line[256];
			format_virtual_jetsam_line(line, sizeof line, crossing);
			std::fprintf(stderr, "%s\n", line);
		});
}

} // namespace uml::runtime

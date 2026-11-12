// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace uml::runtime {

/*
 * A virtual Jetsam: the line a Mac run writes when its footprint crosses the
 * limit a launch stated (platform/host_environment.hpp), where iOS would have
 * ended the process.
 *
 * It is a measurement marker and nothing more. Nothing is killed, refused or
 * slowed; the run goes on and the line says when, and by how much, the process
 * went past the figure the budgets derived from.
 */

/* One crossing: the footprint now, the limit, and the lifetime peak. */
struct VirtualJetsamCrossing final {
	std::uint64_t footprint{};
	std::uint64_t limit{};
	std::uint64_t peak{};
	/* The footprint is back at or under the limit, and only the lifetime peak
	 * shows that it was over in between two samples. */
	bool between_samples{};
};

/*
 * Decides when a sequence of samples crosses the limit. A crossing is the
 * footprint rising above the limit, or the kernel's lifetime peak (which
 * catches a spike between two samples) passing both the limit and every peak
 * already reported. A footprint that stays above the limit is one crossing,
 * and it is a new one only after the footprint has been seen at or under the
 * limit again.
 */
class VirtualJetsam final {
public:
	explicit VirtualJetsam(std::uint64_t limit) noexcept : limit_{limit} {}

	[[nodiscard]] std::optional<VirtualJetsamCrossing> observe(
		std::uint64_t footprint, std::uint64_t peak) noexcept;

private:
	std::uint64_t limit_;
	std::uint64_t reported_peak_{};
	bool over_{};
};

/* The line for one crossing, without its newline, truncated to `size - 1`
 * characters and null-terminated; returns the length. */
std::size_t format_virtual_jetsam_line(char *buffer, std::size_t size,
	const VirtualJetsamCrossing &crossing) noexcept;

/* The line that says, once at the start of a run, which limit the launch
 * stated and the gadget-cache budget derived from it, in whole mebibytes. */
std::size_t format_stated_limit_line(char *buffer, std::size_t size,
	std::uint64_t limit, std::uint64_t gadget_cache_budget) noexcept;

/* What a sample reads: the process's footprint and its lifetime peak. */
struct FootprintSample final {
	std::uint64_t footprint{};
	std::uint64_t peak{};
};

/* The sampler thread of a run. It looks at the footprint every `period`,
 * and for each crossing calls `report`. The destructor ends the thread. */
class VirtualJetsamWatch final {
public:
	using Sampler = std::function<FootprintSample()>;
	using Report = std::function<void(const VirtualJetsamCrossing &)>;

	VirtualJetsamWatch(std::uint64_t limit,
		std::chrono::milliseconds period, Sampler sampler, Report report);
	~VirtualJetsamWatch();

	VirtualJetsamWatch(const VirtualJetsamWatch &) = delete;
	VirtualJetsamWatch &operator=(const VirtualJetsamWatch &) = delete;

private:
	struct State;
	std::unique_ptr<State> state_;
};

/* The footprint the process has and the peak it has had, from the system. */
[[nodiscard]] FootprintSample sample_process_footprint() noexcept;

/* Starts the watch of a run whose limit was stated by the launch: says on
 * stderr what the limit is and the gadget-cache budget (`gadget_cache_budget`,
 * what the run set) that derives from it, then writes each crossing there too.
 * Null, and silent, when the host sets a limit of its own (iOS ends the
 * process there) or none was stated. */
[[nodiscard]] std::unique_ptr<VirtualJetsamWatch> start_virtual_jetsam_watch(
	std::size_t gadget_cache_budget);

} // namespace uml::runtime

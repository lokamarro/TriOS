// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_environment.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "virtual_jetsam.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::FootprintSample;
using uml::runtime::VirtualJetsam;
using uml::runtime::VirtualJetsamCrossing;
using uml::runtime::VirtualJetsamWatch;
using uml::testing::require;
using uml::testing::waitUntil;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t limit = 2867 * mib;

void aFootprintUnderTheLimitSaysNothing()
{
	VirtualJetsam jetsam{limit};
	require(!jetsam.observe(100 * mib, 100 * mib));
	require(!jetsam.observe(limit, limit));
	require(!jetsam.observe(2000 * mib, limit));
	VirtualJetsam unlimited{0};
	require(!unlimited.observe(1ULL << 40, 1ULL << 40));
}

void aFootprintOverTheLimitIsOneLinePerCrossing()
{
	VirtualJetsam jetsam{limit};
	require(!jetsam.observe(2800 * mib, 2800 * mib));
	const auto first = jetsam.observe(2870 * mib, 2870 * mib);
	require(first.has_value());
	require(first->footprint == 2870 * mib && first->limit == limit &&
		first->peak == 2870 * mib && !first->between_samples);

	/* Staying over, and growing, is the same crossing. */
	require(!jetsam.observe(2900 * mib, 2900 * mib));
	require(!jetsam.observe(2880 * mib, 2900 * mib));

	/* Back under, and over again, is a second. */
	require(!jetsam.observe(2000 * mib, 2900 * mib));
	const auto second = jetsam.observe(2868 * mib, 2900 * mib);
	require(second.has_value() && second->footprint == 2868 * mib &&
		second->peak == 2900 * mib && !second->between_samples);
}

void aSpikeBetweenSamplesShowsInThePeak()
{
	VirtualJetsam jetsam{limit};
	require(!jetsam.observe(2000 * mib, 2500 * mib));
	const auto spike = jetsam.observe(2100 * mib, 2950 * mib);
	require(spike.has_value() && spike->between_samples &&
		spike->footprint == 2100 * mib && spike->peak == 2950 * mib);

	/* The same peak is not reported again, a higher one is. */
	require(!jetsam.observe(2100 * mib, 2950 * mib));
	require(!jetsam.observe(2200 * mib, 2950 * mib));
	const auto higher = jetsam.observe(2200 * mib, 3000 * mib);
	require(higher.has_value() && higher->between_samples &&
		higher->peak == 3000 * mib);

	/* A footprint that rises over the limit with a peak already reported
	 * is a crossing of its own. */
	const auto over = jetsam.observe(2900 * mib, 3000 * mib);
	require(over.has_value() && !over->between_samples);
}

void aRunThatStartsWithAPeakOverTheLimitReportsItOnce()
{
	VirtualJetsam jetsam{limit};
	const auto first = jetsam.observe(500 * mib, 3100 * mib);
	require(first.has_value() && first->between_samples);
	require(!jetsam.observe(500 * mib, 3100 * mib));
}

void theLineHasTheFootprintTheLimitAndThePeak()
{
	char line[256];
	const VirtualJetsamCrossing over{2870 * mib + mib / 2, limit, 2901 * mib,
		false};
	const auto length = uml::runtime::format_virtual_jetsam_line(
		line, sizeof line, over);
	require(length == std::strlen(line) && std::strchr(line, '\n') == nullptr);
	const std::string text{line};
	require(text.find("virtual Jetsam") != std::string::npos, text);
	require(text.find("2870.5 MiB") != std::string::npos, text);
	require(text.find("2867.0 MiB") != std::string::npos, text);
	require(text.find("2901.0 MiB") != std::string::npos, text);

	const VirtualJetsamCrossing spike{2100 * mib, limit, 2950 * mib, true};
	uml::runtime::format_virtual_jetsam_line(line, sizeof line, spike);
	const std::string back{line};
	require(back.find("virtual Jetsam") != std::string::npos &&
		back.find("2100.0 MiB") != std::string::npos &&
		back.find("2950.0 MiB") != std::string::npos &&
		back.find("peak") != std::string::npos, back);

	char tiny[8];
	const auto cut = uml::runtime::format_virtual_jetsam_line(
		tiny, sizeof tiny, over);
	require(cut == sizeof tiny - 1 && tiny[cut] == '\0');
	require(uml::runtime::format_virtual_jetsam_line(tiny, 0, over) == 0);
}

void theStartLineHasTheLimitAndTheBudget()
{
	char line[256];
	const auto length = uml::runtime::format_stated_limit_line(
		line, sizeof line, limit, 358 * mib + mib / 3);
	require(length == std::strlen(line));
	const std::string text{line};
	require(text.find("memory limit 2867 MiB") != std::string::npos, text);
	require(text.find("gadget cache budget 358 MiB") != std::string::npos,
		text);
	char tiny[8];
	require(uml::runtime::format_stated_limit_line(
		tiny, sizeof tiny, limit, 358 * mib) == sizeof tiny - 1);
	require(uml::runtime::format_stated_limit_line(
		tiny, 0, limit, 358 * mib) == 0);
}

void theWatchReportsTheCrossingsItSees()
{
	std::mutex mutex;
	std::vector<VirtualJetsamCrossing> reports;
	std::vector<FootprintSample> script{
		{2000 * mib, 2000 * mib}, {2900 * mib, 2900 * mib},
		{2950 * mib, 2950 * mib}, {2000 * mib, 2950 * mib},
		{2870 * mib, 2950 * mib}, {2000 * mib, 2950 * mib}};
	std::size_t next = 0;
	std::size_t calls = 0;
	{
		VirtualJetsamWatch watch{limit, std::chrono::milliseconds{1},
			[&]() -> FootprintSample {
				std::lock_guard lock{mutex};
				++calls;
				return script[std::min(next++, script.size() - 1)];
			},
			[&](const VirtualJetsamCrossing &crossing) {
				std::lock_guard lock{mutex};
				reports.push_back(crossing);
			}};
		require(waitUntil([&] {
			std::lock_guard lock{mutex};
			return next >= script.size() + 3;
		}, std::chrono::seconds{10}), "the watch never read its samples");
	}
	std::lock_guard lock{mutex};
	require(reports.size() == 2, "expected two crossings");
	require(reports[0].footprint == 2900 * mib && !reports[0].between_samples);
	require(reports[1].footprint == 2870 * mib && !reports[1].between_samples);

	/* The thread ended with the watch: no sample comes after it is gone. */
	const auto before = calls;
	std::this_thread::sleep_for(std::chrono::milliseconds{20});
	require(calls == before, "the watch sampled after it was destroyed");
}

/* Stderr, captured for the case that follows. */
class StderrCapture final {
public:
	StderrCapture()
	{
		char path[] = "/tmp/uml-virtual-jetsam.XXXXXX";
		file_ = ::mkstemp(path);
		require(file_ >= 0);
		::unlink(path);
		saved_ = ::dup(STDERR_FILENO);
		require(saved_ >= 0);
		require(::dup2(file_, STDERR_FILENO) >= 0);
	}

	~StderrCapture()
	{
		restore();
		::close(file_);
	}

	StderrCapture(const StderrCapture &) = delete;
	StderrCapture &operator=(const StderrCapture &) = delete;

	void restore()
	{
		if (saved_ >= 0) {
			::dup2(saved_, STDERR_FILENO);
			::close(saved_);
			saved_ = -1;
		}
	}

	[[nodiscard]] std::string text() const
	{
		std::string out;
		char buffer[512];
		off_t at = 0;
		for (;;) {
			const auto got = ::pread(file_, buffer, sizeof buffer, at);
			if (got <= 0)
				break;
			out.append(buffer, static_cast<std::size_t>(got));
			at += got;
		}
		return out;
	}

private:
	int file_{-1};
	int saved_{-1};
};

[[nodiscard]] std::size_t countOf(const std::string &text, const char *needle)
{
	std::size_t count = 0;
	for (auto at = text.find(needle); at != std::string::npos;
		at = text.find(needle, at + 1))
		++count;
	return count;
}

void aLaunchWithALimitWatchesThisProcess()
{
	const char *saved = std::getenv(platform::memory_limit_variable);
	const std::string kept = saved != nullptr ? saved : "";
	::unsetenv(platform::memory_limit_variable);
	platform::set_configured_process_memory_limit(0);

	/* No stated limit, no watch. */
	require(uml::runtime::start_virtual_jetsam_watch(358 * mib) == nullptr);

	if (platform::process_memory_limit_bytes() != 0) {
		/* iOS ends the process itself; nothing is virtual there. */
		require(platform::process_memory_limit_is_virtual() == false);
		return;
	}

	/* A limit of 1 MiB is far under this process's footprint, so the first
	 * sample crosses it; the footprint stays over, so that is the only line. */
	::setenv(platform::memory_limit_variable, "1", 1);
	{
		StderrCapture capture;
		{
			const auto watch = uml::runtime::start_virtual_jetsam_watch(358 * mib);
			require(watch != nullptr);
			require(waitUntil([&] {
				return capture.text().find("virtual Jetsam:") !=
					std::string::npos;
			}, std::chrono::seconds{10}),
				"the watch never wrote its line");
			/* Several of its periods pass without another line. */
			std::this_thread::sleep_for(std::chrono::milliseconds{900});
		}
		capture.restore();
		const auto text = capture.text();
		/* The start line says "virtual Jetsam on"; the crossing says
		 * "virtual Jetsam:". */
		require(countOf(text, "virtual Jetsam:") == 1, text);
		require(text.find("memory limit 1 MiB") != std::string::npos &&
			text.find("gadget cache budget 358 MiB") != std::string::npos,
			text);
		require(text.find("limit 1.0 MiB") != std::string::npos, text);
		require(text.find("footprint") != std::string::npos &&
			text.find("peak") != std::string::npos, text);
	}

	/* A limit far over the footprint writes nothing. */
	::setenv(platform::memory_limit_variable, "1048576", 1);
	{
		StderrCapture capture;
		{
			const auto watch = uml::runtime::start_virtual_jetsam_watch(358 * mib);
			require(watch != nullptr);
			std::this_thread::sleep_for(std::chrono::milliseconds{600});
		}
		capture.restore();
		require(capture.text().find("virtual Jetsam:") == std::string::npos,
			capture.text());
	}

	if (saved != nullptr)
		::setenv(platform::memory_limit_variable, kept.c_str(), 1);
	else
		::unsetenv(platform::memory_limit_variable);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"a-footprint-under-the-limit-says-nothing",
			aFootprintUnderTheLimitSaysNothing},
		{"a-footprint-over-the-limit-is-one-line-per-crossing",
			aFootprintOverTheLimitIsOneLinePerCrossing},
		{"a-spike-between-samples-shows-in-the-peak",
			aSpikeBetweenSamplesShowsInThePeak},
		{"a-run-that-starts-with-a-peak-over-the-limit-reports-it-once",
			aRunThatStartsWithAPeakOverTheLimitReportsItOnce},
		{"the-line-has-the-footprint-the-limit-and-the-peak",
			theLineHasTheFootprintTheLimitAndThePeak},
		{"the-start-line-has-the-limit-and-the-budget",
			theStartLineHasTheLimitAndTheBudget},
		{"the-watch-reports-the-crossings-it-sees",
			theWatchReportsTheCrossingsItSees},
		{"a-launch-with-a-limit-watches-this-process",
			aLaunchWithALimitWatchesThisProcess},
	};
	return uml::testing::runNamedTests(tests);
}

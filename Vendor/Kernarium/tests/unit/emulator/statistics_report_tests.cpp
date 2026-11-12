// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The line a macOS launcher prints for each --stats interval. The report is
 * given its samples, so the arithmetic and the cases that make no interval
 * (the first sample, a guest that restarted, a clock that stood still) are
 * checked against values made up for them.
 */
#include "named_tests.hpp"
#include "require.hpp"
#include "statistics_report.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using uml::emulator::GraphicsAttribution;
using uml::emulator::StatisticsMemory;
using uml::emulator::StatisticsReport;
using uml::emulator::StatisticsSample;
using uml::testing::require;

const std::chrono::steady_clock::time_point origin {1000s};
constexpr std::uint64_t mebibyte = 1ULL << 20;

StatisticsSample sample(std::chrono::milliseconds at, std::uint64_t cpu_ms,
	std::uint64_t presented = 0, std::uint64_t offered = 0,
	std::uint32_t processors = 2)
{
	return {
		.at = origin + at,
		.processors = processors,
		.cpu_time_ns = cpu_ms * 1000000ULL,
		.presented_frames = presented,
		.offered_frames = offered,
		.ram_bytes = 2048 * mebibyte,
		.ram_held_bytes = 1536 * mebibyte,
	};
}

void aFirstSampleMakesNoLine()
{
	StatisticsReport report;
	require(!report.add(sample(0ms, 0), std::nullopt).has_value());
}

void theLineHoldsTheRatesOfTheInterval()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 100, 10, 12), std::nullopt);
	/* 500 ms later: both vCPUs busy for 250 ms of it is 50%; 15 frames
	 * reached the screen and 25 were offered. */
	const auto line = report.add(sample(500ms, 600, 25, 37), std::nullopt);
	require(line.has_value());
	require(*line == "uml-darwin: stats 0.5 s cpu 50.0% of 2 vCPUs "
			 "frames 30.0/s presented 50.0/s offered held 1536 of "
			 "2048 MiB", *line);
}

/* The share is of the guest's vCPUs, not of one core, and no more than all of
 * them. */
void theShareIsOfAllTheVcpusAndNeverMoreThanAll()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 0, 0, 0, 4), std::nullopt);
	auto line = report.add(sample(1000ms, 1000, 0, 0, 4), std::nullopt);
	require(line && line->find("cpu 25.0% of 4 vCPUs") != std::string::npos,
		"one busy vCPU of four is a quarter");
	/* A thread read a moment after the clock can add up to a hair over. */
	line = report.add(sample(2000ms, 5001, 0, 0, 4), std::nullopt);
	require(line && line->find("cpu 100.0% of 4 vCPUs") != std::string::npos,
		"the share is capped at all of them");
}

void theMemoryFiguresAreInMebibytes()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 0), std::nullopt);
	const auto line = report.add(sample(1000ms, 0),
		StatisticsMemory {.total_bytes = 1826 * mebibyte,
			.available_bytes = 1008 * mebibyte});
	require(line.has_value());
	require(line->find(" memory 818 MiB in use of 1826 MiB held 1536 of "
			   "2048 MiB") != std::string::npos, *line);
}

void aGuestBeforeItsKernelClaimedRamHasNoHeldFigure()
{
	StatisticsReport report;
	auto first = sample(0ms, 0);
	auto second = sample(1000ms, 10);
	first.ram_bytes = first.ram_held_bytes = 0;
	second.ram_bytes = second.ram_held_bytes = 0;
	(void)report.add(first, std::nullopt);
	const auto line = report.add(second, std::nullopt);
	require(line && line->find("held") == std::string::npos, "held");
}

/* A restarted guest starts its CPU time from zero, and a pair spanning two
 * guests is not a rate. */
void aRestartBreaksTheBaseline()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 5000), std::nullopt);
	require(!report.add(sample(1000ms, 40), std::nullopt).has_value(),
		"CPU time that went backwards made a rate");
	require(report.add(sample(2000ms, 1040), std::nullopt).has_value(),
		"the sample after the break is not a baseline");
}

void aCounterThatWentBackwardsBreaksTheBaseline()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 0, 100, 100), std::nullopt);
	require(!report.add(sample(1000ms, 10, 50, 100), std::nullopt).has_value(),
		"presented frames that went backwards made a rate");
	require(!report.add(sample(2000ms, 20, 60, 40), std::nullopt).has_value(),
		"offered frames that went backwards made a rate");
}

void aGuestThatEndedMakesTheNextSampleABaseline()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 0), std::nullopt);
	report.guest_ended();
	require(!report.add(sample(1000ms, 500), std::nullopt).has_value(),
		"a rate spanned a gap with no guest");
	require(report.add(sample(2000ms, 1000), std::nullopt).has_value());
}

void aClockThatStoodStillMakesNoLine()
{
	StatisticsReport report;
	(void)report.add(sample(1000ms, 0), std::nullopt);
	require(!report.add(sample(1000ms, 10), std::nullopt).has_value());
	require(!report.add(sample(500ms, 20), std::nullopt).has_value());
}

void aGuestWithNoVcpuHasNoShare()
{
	StatisticsReport report;
	(void)report.add(sample(0ms, 0, 0, 0, 0), std::nullopt);
	const auto line = report.add(sample(1000ms, 0, 0, 0, 0), std::nullopt);
	require(line && line->find("cpu 0.0% of 0 vCPUs") != std::string::npos);
}

void theMemoryIsReadOnEverySecondSample()
{
	StatisticsReport report;
	require(!report.memory_due());
	require(report.memory_due());
	require(!report.memory_due());
	require(report.memory_due());
}

/* The count is cumulative, so every line states it as it stands; a launcher
 * with no display has no figure and says nothing. */
void theLineStatesTheGpuRefusalsOfADisplay()
{
	StatisticsReport report;
	auto first = sample(0ms, 0);
	auto second = sample(1000ms, 0);
	auto third = sample(2000ms, 0);
	first.gpu_memory_refusals = 0;
	second.gpu_memory_refusals = 0;
	third.gpu_memory_refusals = 3;
	(void)report.add(first, std::nullopt);
	auto line = report.add(second, std::nullopt);
	require(line && line->ends_with(" gpu-refusals 0"), *line);
	line = report.add(third, std::nullopt);
	require(line && line->ends_with(" held 1536 of 2048 MiB gpu-refusals 3"),
		*line);
	StatisticsReport bare;
	(void)bare.add(sample(0ms, 0), std::nullopt);
	line = bare.add(sample(1000ms, 0), std::nullopt);
	require(line && line->find("gpu-refusals") == std::string::npos, *line);
}

/* A launcher with a device memory budget says how much of it the host holds,
 * in MiB, after the refusals; one without a budget says nothing. */
void theLineStatesTheGpuMemoryBudgetAndItsUsage()
{
	StatisticsReport report;
	auto first = sample(0ms, 0);
	auto second = sample(1000ms, 0);
	second.gpu_memory_refusals = 2;
	second.gpu_memory_budget_bytes = 534 * mebibyte;
	second.gpu_memory_used_bytes = 487 * mebibyte + 512 * 1024;
	(void)report.add(first, std::nullopt);
	auto line = report.add(second, std::nullopt);
	require(line && line->ends_with(" gpu-refusals 2 gpu-memory 487/534 MiB"),
		*line);

	StatisticsReport bare;
	auto third = sample(0ms, 0);
	auto fourth = sample(1000ms, 0);
	fourth.gpu_memory_used_bytes = 12 * mebibyte;
	(void)bare.add(third, std::nullopt);
	line = bare.add(fourth, std::nullopt);
	require(line && line->find("gpu-memory") == std::string::npos, *line);
}

/* Who holds the budget is one more clause after the budget's own; a launcher
 * without a budget says nothing. */
void theLineStatesWhoHoldsTheGraphicsBudget()
{
	StatisticsReport report;
	auto first = sample(0ms, 0);
	auto second = sample(1000ms, 0);
	second.gpu_memory_budget_bytes = 534 * mebibyte;
	second.gpu_memory_used_bytes = 500 * mebibyte;
	second.graphics_attribution = GraphicsAttribution {
		.context_bytes = 420 * mebibyte,
		.holders = {
			{.pid = 2204, .bytes = 130 * mebibyte, .idle_ms = 61'500,
				.name = "jelly"},
			{.pid = 0, .bytes = 8 * mebibyte, .idle_ms = 0,
				.name = "RenderThread"},
		},
	};
	(void)report.add(first, std::nullopt);
	auto line = report.add(second, std::nullopt);
	require(line && line->ends_with(
			" gpu-memory 500/534 MiB attributed 420 MiB "
			"holders 2204:jelly:130 MiB:61 s 0:RenderThread:8 MiB:0 s"),
		*line);

	StatisticsReport quiet;
	auto third = sample(0ms, 0);
	auto fourth = sample(1000ms, 0);
	fourth.gpu_memory_budget_bytes = 534 * mebibyte;
	fourth.gpu_memory_used_bytes = 12 * mebibyte;
	fourth.graphics_attribution = GraphicsAttribution {};
	(void)quiet.add(third, std::nullopt);
	line = quiet.add(fourth, std::nullopt);
	require(line && line->ends_with(" gpu-memory 12/534 MiB attributed 0 MiB"),
		*line);

	StatisticsReport unbudgeted;
	auto fifth = sample(0ms, 0);
	auto sixth = sample(1000ms, 0);
	sixth.graphics_attribution = GraphicsAttribution {
		.context_bytes = 12 * mebibyte};
	(void)unbudgeted.add(fifth, std::nullopt);
	line = unbudgeted.add(sixth, std::nullopt);
	require(line && line->find("gpu-memory") == std::string::npos &&
			line->find("attributed") == std::string::npos,
		*line);
}

/* The guest's own reclaimer is the last clause, after the holders it ends,
 * and only while it asks. */
void theLineStatesWhenTheGuestReclaimsGraphicsMemory()
{
	StatisticsReport report;
	auto first = sample(0ms, 0);
	auto second = sample(1000ms, 0);
	second.gpu_memory_budget_bytes = 534 * mebibyte;
	second.gpu_memory_used_bytes = 500 * mebibyte;
	second.graphics_attribution = GraphicsAttribution {
		.context_bytes = 420 * mebibyte,
		.holders = {{.pid = 2204, .bytes = 130 * mebibyte, .idle_ms = 61'500,
			.name = "jelly"}},
	};
	second.guest_reclaims = true;
	(void)report.add(first, std::nullopt);
	auto line = report.add(second, std::nullopt);
	require(line && line->ends_with(
			" holders 2204:jelly:130 MiB:61 s guest-reclaim=on"),
		*line);

	StatisticsReport idle;
	auto third = sample(0ms, 0);
	auto fourth = sample(1000ms, 0);
	fourth.gpu_memory_budget_bytes = 534 * mebibyte;
	fourth.gpu_memory_used_bytes = 12 * mebibyte;
	(void)idle.add(third, std::nullopt);
	line = idle.add(fourth, std::nullopt);
	require(line && line->find("guest-reclaim") == std::string::npos, *line);
}

void theElapsedTimeCountsFromTheFirstSample()
{
	StatisticsReport report;
	(void)report.add(sample(20000ms, 0), std::nullopt);
	const auto line = report.add(sample(23500ms, 0), std::nullopt);
	require(line && line->starts_with("uml-darwin: stats 3.5 s "), *line);
}

}  // namespace

int main()
{
	constexpr uml::testing::NamedTest tests[] = {
		{"a-first-sample-makes-no-line", aFirstSampleMakesNoLine},
		{"the-line-holds-the-rates-of-the-interval",
			theLineHoldsTheRatesOfTheInterval},
		{"the-share-is-of-all-the-vcpus",
			theShareIsOfAllTheVcpusAndNeverMoreThanAll},
		{"the-memory-figures-are-in-mebibytes",
			theMemoryFiguresAreInMebibytes},
		{"no-ram-claimed-no-held-figure",
			aGuestBeforeItsKernelClaimedRamHasNoHeldFigure},
		{"a-restart-breaks-the-baseline", aRestartBreaksTheBaseline},
		{"a-counter-that-went-backwards-breaks-the-baseline",
			aCounterThatWentBackwardsBreaksTheBaseline},
		{"a-guest-that-ended-makes-the-next-sample-a-baseline",
			aGuestThatEndedMakesTheNextSampleABaseline},
		{"a-clock-that-stood-still-makes-no-line",
			aClockThatStoodStillMakesNoLine},
		{"a-guest-with-no-vcpu-has-no-share", aGuestWithNoVcpuHasNoShare},
		{"the-memory-is-read-on-every-second-sample",
			theMemoryIsReadOnEverySecondSample},
		{"the-line-states-the-gpu-refusals-of-a-display",
			theLineStatesTheGpuRefusalsOfADisplay},
		{"the-line-states-the-gpu-memory-budget-and-its-usage",
			theLineStatesTheGpuMemoryBudgetAndItsUsage},
		{"the-line-states-who-holds-the-graphics-budget",
			theLineStatesWhoHoldsTheGraphicsBudget},
		{"the-line-states-when-the-guest-reclaims-graphics-memory",
			theLineStatesWhenTheGuestReclaimsGraphicsMemory},
		{"the-elapsed-time-counts-from-the-first-sample",
			theElapsedTimeCountsFromTheFirstSample},
	};
	return uml::testing::runNamedTests(tests);
}

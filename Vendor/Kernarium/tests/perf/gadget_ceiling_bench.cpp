// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Matched native/ideal-gadget ceiling benchmark.
 *
 * Every workload is one labeled loop body that the native control executes
 * directly and the ideal mode feeds to the production gadget Builder. The
 * dependent ADD-immediate chain is a best-case scalar ceiling; the scalar and
 * SIMD mixes follow the gadget families real guests execute. None of them is
 * a claim about syscall or whole-guest throughput.
 */

#include "gadget_ceiling_workload.hpp"

#include <time.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#if defined(UML_GADGET_CEILING_WITH_IDEAL)
#include "gadget_ceiling_ideal.hpp"
#endif

namespace {

constexpr unsigned default_repetitions = 7;

struct Workload final {
	std::string_view name;
	UmlGadgetCeilingNative native;
	const std::uint32_t *begin;
	const std::uint32_t *end;
	std::size_t body_instructions;
	/* Instructions each data-dependent branch skips when taken; keep equal
	 * to the contract in the body's .inc header. */
	std::array<std::uint64_t, 2> path_skips;
	std::uint64_t default_iterations;
};

const std::array workloads{
	Workload{"add-immediate", uml_gadget_ceiling_native_add_immediate,
		uml_gadget_ceiling_add_immediate_begin,
		uml_gadget_ceiling_add_immediate_end, 18, {0, 0}, 100'000'000},
	Workload{"scalar-mix", uml_gadget_ceiling_native_scalar_mix,
		uml_gadget_ceiling_scalar_mix_begin,
		uml_gadget_ceiling_scalar_mix_end, 41, {3, 3}, 30'000'000},
	Workload{"simd-mix", uml_gadget_ceiling_native_simd_mix,
		uml_gadget_ceiling_simd_mix_begin,
		uml_gadget_ceiling_simd_mix_end, 39, {3, 2}, 30'000'000},
};

struct Configuration final {
	const Workload *workload{&workloads[0]};
	std::uint64_t iterations{};
	unsigned repetitions{default_repetitions};
	bool dump_stream{};
};

struct Sample final {
	std::uint64_t nanoseconds{};
	double giga_instructions_per_second{};
};

struct Distribution final {
	double median{};
	double mad{};
	double minimum{};
	double maximum{};
};

[[noreturn]] void usage(const char *program)
{
	std::fprintf(stderr,
		"usage: %s [--workload name] [--iterations positive-integer] "
		"[--repetitions positive-integer] [--dump-stream]\nworkloads:",
		program);
	for (const auto &workload : workloads)
		std::fprintf(stderr, " %.*s",
			static_cast<int>(workload.name.size()),
			workload.name.data());
	std::fputc('\n', stderr);
	std::exit(2);
}

template <typename Integer>
[[nodiscard]] Integer parse_positive(std::string_view text, const char *name)
{
	Integer value{};
	const auto [end, error] = std::from_chars(
		text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size() ||
	    value == 0) {
		std::fprintf(stderr, "%s must be a positive integer\n", name);
		std::exit(2);
	}
	return value;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv)
{
	Configuration result;
	for (int index = 1; index < argc; ++index) {
		const std::string_view option{argv[index]};
		if (option == "--dump-stream") {
			result.dump_stream = true;
			continue;
		}
		if (index + 1 == argc)
			usage(argv[0]);
		const std::string_view value{argv[++index]};
		if (option == "--iterations") {
			result.iterations = parse_positive<std::uint64_t>(
				value, "--iterations");
		} else if (option == "--repetitions") {
			result.repetitions = parse_positive<unsigned>(
				value, "--repetitions");
		} else if (option == "--workload") {
			const auto found = std::find_if(workloads.begin(),
				workloads.end(), [value](const Workload &workload) {
					return workload.name == value;
				});
			if (found == workloads.end())
				usage(argv[0]);
			result.workload = &*found;
		} else {
			usage(argv[0]);
		}
	}
	if (result.iterations == 0)
		result.iterations = result.workload->default_iterations;
	if (result.iterations >
	    std::numeric_limits<std::uint64_t>::max() /
		    result.workload->body_instructions) {
		std::fprintf(stderr, "--iterations is too large\n");
		std::exit(2);
	}
	return result;
}

[[nodiscard]] std::uint64_t now_ns() noexcept
{
#if defined(__APPLE__)
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
	timespec value{};
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
		std::perror("clock_gettime");
		std::abort();
	}
	return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
		static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

[[nodiscard]] std::span<const std::uint32_t> body(const Workload &workload)
{
	const auto begin = reinterpret_cast<std::uintptr_t>(workload.begin);
	const auto end = reinterpret_cast<std::uintptr_t>(workload.end);
	if (end < begin || begin % alignof(std::uint32_t) != 0 ||
	    (end - begin) % sizeof(std::uint32_t) != 0)
		return {};
	return {workload.begin, (end - begin) / sizeof(std::uint32_t)};
}

[[nodiscard]] std::uint64_t body_hash(std::span<const std::uint32_t> words)
{
	constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
	constexpr std::uint64_t prime = 1'099'511'628'211ULL;
	std::vector<unsigned char> bytes(words.size_bytes());
	std::memcpy(bytes.data(), words.data(), bytes.size());
	std::uint64_t hash = offset;
	for (const auto byte : bytes) {
		hash ^= byte;
		hash *= prime;
	}
	return hash;
}

/* Guest instructions one run retired: every body word per iteration, less the
 * words each data-dependent branch skipped whenever it was taken. */
[[nodiscard]] std::uint64_t guest_instructions(const Workload &workload,
	std::uint64_t iterations, const UmlGadgetCeilingResult &result)
{
	auto count = iterations * workload.body_instructions;
	for (std::size_t path = 0; path < result.paths.size(); ++path) {
		if (result.paths[path] > iterations) {
			std::fprintf(stderr,
				"path counter %zu exceeds the iteration count\n", path);
			std::exit(1);
		}
		count -= (iterations - result.paths[path]) *
			workload.path_skips[path];
	}
	return count;
}

class NativeBuffer final {
public:
	NativeBuffer()
	{
		if (posix_memalign(&memory_, uml_gadget_ceiling_buffer_alignment,
			    uml_gadget_ceiling_buffer_bytes) != 0 ||
		    memory_ == nullptr) {
			std::fprintf(stderr, "could not allocate the native buffer\n");
			std::exit(1);
		}
	}
	~NativeBuffer() { std::free(memory_); }
	NativeBuffer(const NativeBuffer &) = delete;
	NativeBuffer &operator=(const NativeBuffer &) = delete;

	void reset() noexcept
	{
		std::memset(memory_, 0, uml_gadget_ceiling_buffer_bytes);
	}
	[[nodiscard]] void *memory() const noexcept { return memory_; }

private:
	void *memory_{};
};

NativeBuffer native_buffer;

[[nodiscard]] UmlGadgetCeilingResult run_native(const Workload &workload,
	std::uint64_t iterations)
{
	UmlGadgetCeilingResult result;
	result.checksum = workload.native(iterations, native_buffer.memory(),
		result.paths.data());
	return result;
}

void print_result(const char *mode, const Workload &workload,
	std::uint64_t iterations, const UmlGadgetCeilingResult &result)
{
	std::printf(
		"UML_GADGET_CEILING_RESULT mode=%s workload=%.*s iterations=0x%llx "
		"checksum=0x%016llx paths=0x%016llx,0x%016llx "
		"instructions=0x%016llx\n",
		mode, static_cast<int>(workload.name.size()), workload.name.data(),
		static_cast<unsigned long long>(iterations),
		static_cast<unsigned long long>(result.checksum),
		static_cast<unsigned long long>(result.paths[0]),
		static_cast<unsigned long long>(result.paths[1]),
		static_cast<unsigned long long>(
			guest_instructions(workload, iterations, result)));
}

void require_equal(const char *mode, std::uint64_t iterations,
	const UmlGadgetCeilingResult &actual,
	const UmlGadgetCeilingResult &expected)
{
	if (actual == expected)
		return;
	std::fprintf(stderr,
		"%s result mismatch at %llu iterations: checksum %016llx "
		"paths %llu,%llu; expected %016llx paths %llu,%llu\n",
		mode, static_cast<unsigned long long>(iterations),
		static_cast<unsigned long long>(actual.checksum),
		static_cast<unsigned long long>(actual.paths[0]),
		static_cast<unsigned long long>(actual.paths[1]),
		static_cast<unsigned long long>(expected.checksum),
		static_cast<unsigned long long>(expected.paths[0]),
		static_cast<unsigned long long>(expected.paths[1]));
	std::exit(1);
}

/* Both modes start from the same zeroed buffer and CPU state; the ideal run
 * must reproduce the native checksum and path counts exactly. */
[[nodiscard]] UmlGadgetCeilingResult verify(const Workload &workload,
	std::uint64_t iterations)
{
	native_buffer.reset();
	const auto native = run_native(workload, iterations);
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	uml_gadget_ceiling_ideal_reset();
	require_equal("ideal-gadgets", iterations,
		uml_gadget_ceiling_ideal_workload(iterations), native);
#endif
	return native;
}

template <typename Run>
[[nodiscard]] Sample measure(const Workload &workload, const char *mode,
	unsigned repetition, std::uint64_t iterations,
	const UmlGadgetCeilingResult &expected, Run run)
{
	const auto start = now_ns();
	const auto result = run(iterations);
	const auto elapsed = now_ns() - start;
	require_equal(mode, iterations, result, expected);
	const auto instructions = guest_instructions(workload, iterations,
		result);
	const auto rate = static_cast<double>(
		static_cast<long double>(instructions) / elapsed);
	std::printf(
		"UML_GADGET_CEILING sample=%u mode=%s iterations=%llu "
		"guest_instructions=%llu elapsed_ns=%llu "
		"giga_instructions_per_second=%.6f\n",
		repetition, mode, static_cast<unsigned long long>(iterations),
		static_cast<unsigned long long>(instructions),
		static_cast<unsigned long long>(elapsed), rate);
	return {elapsed, rate};
}

[[nodiscard]] double median_of_sorted(const std::vector<double> &values)
{
	const auto middle = values.size() / 2;
	if ((values.size() & 1U) != 0)
		return values[middle];
	return values[middle - 1] +
		(values[middle] - values[middle - 1]) / 2.0;
}

[[nodiscard]] Distribution describe(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	const auto median = median_of_sorted(values);
	std::vector<double> deviations;
	deviations.reserve(values.size());
	for (const auto value : values)
		deviations.push_back(std::abs(value - median));
	std::sort(deviations.begin(), deviations.end());
	return {
		.median = median,
		.mad = median_of_sorted(deviations),
		.minimum = values.front(),
		.maximum = values.back(),
	};
}

[[nodiscard]] double summarize(const char *mode,
	const std::vector<Sample> &samples)
{
	std::vector<double> rates;
	rates.reserve(samples.size());
	for (const auto &sample : samples)
		rates.push_back(sample.giga_instructions_per_second);
	const auto distribution = describe(std::move(rates));
	std::printf(
		"UML_GADGET_CEILING_SUMMARY mode=%s samples=%zu "
		"median_giga_instructions_per_second=%.6f mad=%.6f min=%.6f "
		"max=%.6f\n",
		mode, samples.size(), distribution.median, distribution.mad,
		distribution.minimum, distribution.maximum);
	return distribution.median;
}

} // namespace

int main(int argc, char **argv)
{
	const auto configuration = parse_arguments(argc, argv);
	const auto &workload = *configuration.workload;
	const auto words = body(workload);
	if (words.size() != workload.body_instructions) {
		std::fprintf(stderr, "unexpected workload body size: %zu\n",
			words.size());
		return 1;
	}
#if defined(__APPLE__)
	if (pthread_set_qos_class_self_np(
		    QOS_CLASS_USER_INTERACTIVE, 0) != 0)
		std::fprintf(stderr, "warning: could not select performance QoS\n");
#endif

	std::printf(
		"UML_GADGET_CEILING_CONFIG version=2 workload=%.*s "
		"body_instructions=%zu path_skips=%llu,%llu body_fnv1a64=%016llx "
		"iterations=%llu repetitions=%u\n",
		static_cast<int>(workload.name.size()), workload.name.data(),
		words.size(),
		static_cast<unsigned long long>(workload.path_skips[0]),
		static_cast<unsigned long long>(workload.path_skips[1]),
		static_cast<unsigned long long>(body_hash(words)),
		static_cast<unsigned long long>(configuration.iterations),
		configuration.repetitions);

#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	uml_gadget_ceiling_prepare_ideal(words);
	if (configuration.dump_stream) {
		uml_gadget_ceiling_ideal_dump_stream();
		return 0;
	}
#else
	if (configuration.dump_stream) {
		std::fprintf(stderr, "this build has no ideal gadget mode\n");
		return 2;
	}
#endif
	for (const std::uint64_t count : {1ULL, 2ULL, 3ULL, 17ULL, 101ULL,
		     4'096ULL, 100'003ULL})
		(void)verify(workload, std::min(count, configuration.iterations));
	const auto expected = verify(workload, configuration.iterations);
	print_result("native", workload, configuration.iterations, expected);

	std::vector<Sample> native_samples;
	native_samples.reserve(configuration.repetitions);
	const auto native_run = [&workload](std::uint64_t iterations) {
		native_buffer.reset();
		return run_native(workload, iterations);
	};
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	std::vector<Sample> ideal_samples;
	ideal_samples.reserve(configuration.repetitions);
	const auto ideal_run = [](std::uint64_t iterations) {
		uml_gadget_ceiling_ideal_reset();
		return uml_gadget_ceiling_ideal_workload(iterations);
	};
#endif
	for (unsigned repetition = 1;
	     repetition <= configuration.repetitions; ++repetition) {
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
		if ((repetition & 1U) == 0)
			ideal_samples.push_back(measure(workload, "ideal-gadgets",
				repetition, configuration.iterations, expected,
				ideal_run));
#endif
		native_samples.push_back(measure(workload, "native", repetition,
			configuration.iterations, expected, native_run));
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
		if ((repetition & 1U) != 0)
			ideal_samples.push_back(measure(workload, "ideal-gadgets",
				repetition, configuration.iterations, expected,
				ideal_run));
#endif
	}
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	const auto native_median = summarize("native", native_samples);
	const auto ideal_median = summarize("ideal-gadgets", ideal_samples);
	std::vector<double> paired_percentages;
	paired_percentages.reserve(configuration.repetitions);
	for (std::size_t index = 0; index < native_samples.size(); ++index) {
		paired_percentages.push_back(
			100.0 * ideal_samples[index].giga_instructions_per_second /
			native_samples[index].giga_instructions_per_second);
	}
	const auto paired = describe(std::move(paired_percentages));
	std::printf(
		"UML_GADGET_CEILING_RATIO samples=%u "
		"median_ideal_gadgets_percent_of_native=%.3f mad=%.3f "
		"min=%.3f max=%.3f ratio_of_medians=%.3f\n",
		configuration.repetitions, paired.median, paired.mad,
		paired.minimum, paired.maximum,
		100.0 * ideal_median / native_median);
#else
	(void)summarize("native", native_samples);
#endif
	std::puts("OK");
	return 0;
}

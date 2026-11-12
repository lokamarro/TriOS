/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "options.hpp"
#include "guest_memory_argument.hpp"
#include "platform/host_topology.hpp"
#include "runtime/guest_memory_budget.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "guest_console.hpp"
#include "ubd_path.hpp"

namespace uml::emulator {

constexpr std::string_view jit_cache_budget_prefix{"--jit-cache-budget="};
constexpr std::string_view jit_executor_cache_budget_prefix{
	"--jit-executor-cache-budget="};
constexpr std::string_view jit_tier_prefix{"--jit-tier="};
constexpr std::string_view gadget_cache_budget_prefix{
	"--gadget-cache-budget="};
constexpr std::string_view guest_hibernation_prefix{"--guest-hibernation="};
constexpr std::string_view memory_limit_prefix{"--memory-limit="};
constexpr std::string_view session_record_prefix{"--session-record="};
namespace {

[[nodiscard]] bool has_argument(
	const std::vector<std::string> &arguments, std::string_view value)
{
	return std::ranges::any_of(arguments, [&](const auto &argument) {
		return argument == value;
	});
}

[[nodiscard]] bool has_prefix(
	const std::vector<std::string> &arguments, std::string_view prefix)
{
	return std::ranges::any_of(arguments, [&](const auto &argument) {
		return argument.starts_with(prefix);
	});
}

/* The guest's own memory size and CPU count, read from the kernel arguments
 * the host is about to pass it. Both keep their kernel meaning; the host only
 * reads them to decide how much it may spend on translated-code caches.
 */
[[nodiscard]] unsigned memory_digit(char character) noexcept
{
	if (character >= '0' && character <= '9')
		return static_cast<unsigned>(character - '0');
	if (character >= 'a' && character <= 'f')
		return static_cast<unsigned>(character - 'a') + 10U;
	if (character >= 'A' && character <= 'F')
		return static_cast<unsigned>(character - 'A') + 10U;
	return 16U;
}

/* Linux memparse() uses simple_strtoull(..., base 0), then applies one
 * case-insensitive K/M/G/T/P/E suffix and leaves any following text to its
 * caller. UML's mem= setup ignores that end pointer, so the host must do the
 * same when deriving the budget: base prefixes, unsigned wrap, suffixes and
 * trailing text are all guest-visible kernel behavior here.
 */
[[nodiscard]] std::size_t parse_guest_memory_quantity(
	std::string_view text) noexcept
{
	if (text.empty())
		return 0;
	unsigned base = 10;
	std::size_t cursor = 0;
	if (text.front() == '0') {
		base = 8;
		if (text.size() >= 3 && (text[1] == 'x' || text[1] == 'X') &&
		    memory_digit(text[2]) < 16U) {
			base = 16;
			cursor = 2;
		}
	}
	std::uint64_t value = 0;
	while (cursor < text.size()) {
		const auto digit = memory_digit(text[cursor]);
		if (digit >= base)
			break;
		value = value * base + digit;
		++cursor;
	}
	unsigned shifts = 0;
	if (cursor < text.size()) {
		switch (text[cursor]) {
		case 'K': case 'k': shifts = 1; break;
		case 'M': case 'm': shifts = 2; break;
		case 'G': case 'g': shifts = 3; break;
		case 'T': case 't': shifts = 4; break;
		case 'P': case 'p': shifts = 5; break;
		case 'E': case 'e': shifts = 6; break;
		default: break;
		}
	}
	while (shifts-- != 0)
		value <<= 10U;
	return static_cast<std::size_t>(value);
}

[[nodiscard]] bool parse_guest_hibernation(std::string_view text,
	::uml::runtime::HibernationMode &mode) noexcept
{
	using ::uml::runtime::HibernationMode;
	if (text == "auto")
		mode = HibernationMode::automatic;
	else if (text == "off")
		mode = HibernationMode::off;
	else if (text == "on")
		mode = HibernationMode::on;
	else
		return false;
	return true;
}

[[nodiscard]] std::optional<int> parse_guest_cpu_count(
	std::string_view text) noexcept
{
	/* kstrtoint() accepts one terminal newline and rejects every other
	 * trailing byte. Kernel command-line arguments do not ordinarily carry
	 * one, but matching it here keeps the cache-budget divisor exact for
	 * embedding callers too.
	 */
	if (!text.empty() && text.back() == '\n')
		text.remove_suffix(1);
	if (text.empty())
		return std::nullopt;
	if (text.front() == '+') {
		text.remove_prefix(1);
		if (text.empty() || text.front() == '+' || text.front() == '-')
			return std::nullopt;
	}
	int parsed = 0;
	const auto result = std::from_chars(
		text.data(), text.data() + text.size(), parsed, 10);
	if (result.ec != std::errc{} ||
	    result.ptr != text.data() + text.size())
		return std::nullopt;
	return parsed;
}

[[nodiscard]] unsigned guest_cpu_count(
	const std::vector<std::string> &arguments) noexcept
{
	std::optional<unsigned> explicit_count;
	for (const auto &argument : arguments) {
		constexpr std::string_view prefix{"ncpus="};
		if (!argument.starts_with(prefix))
			continue;
		const auto parsed = parse_guest_cpu_count(
			std::string_view{argument}.substr(prefix.size()));
		/* The kernel consumes a malformed value but leaves the preceding
		 * valid value (or its automatic default) unchanged.
		 */
		if (!parsed)
			continue;
		explicit_count = static_cast<unsigned>(std::clamp(
			*parsed, 1,
			static_cast<int>(
				::uml::runtime::maximum_guest_cpu_count)));
	}
	return explicit_count ? *explicit_count :
		::uml::runtime::default_guest_cpu_count();
}

[[nodiscard]] bool informational(
	const std::vector<std::string> &arguments)
{
	return has_argument(arguments, "--help") ||
		has_argument(arguments, "--version") ||
		has_argument(arguments, "--showconfig");
}

[[nodiscard]] bool parse_execution_engine(
	std::string_view value, ::uml::runtime::ExecutionEngine &engine)
{
	if (value == "auto")
		engine = ::uml::runtime::ExecutionEngine::automatic;
	else if (value == "jit")
		engine = ::uml::runtime::ExecutionEngine::jit;
	else if (value == "gadgets")
		engine = ::uml::runtime::ExecutionEngine::gadgets;
	else
		return false;
	return true;
}

} // namespace

unsigned configured_guest_cpu_count(
	const std::vector<std::string> &arguments) noexcept
{
	return guest_cpu_count(arguments);
}

unsigned default_application_guest_cpu_count() noexcept
{
	return ::uml::runtime::guest_memory::application_vcpu_limit(
		::uml::platform::physical_processor_count());
}

unsigned launch_guest_cpu_count(
	const std::vector<std::string> &arguments) noexcept
{
	for (const auto &argument : arguments)
		if (argument.starts_with("ncpus="))
			return guest_cpu_count(arguments);
	return default_application_guest_cpu_count();
}

std::size_t configured_guest_memory_bytes(
	const std::vector<std::string> &arguments)
{
	std::size_t bytes = 0;
	for (const auto &argument : arguments)
		if (argument.starts_with("mem="))
			bytes = parse_guest_memory_quantity(
				std::string_view{argument}.substr(4));
	return bytes;
}

EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs)
{
	EmulatorOptions prepared;
	/* Policy can append at most nine kernel arguments. Keep the final argc
	 * representable by the fixed-width kernel entry ABI before allocating or
	 * inspecting a caller-supplied vector. */
	constexpr int maximum_appended_arguments = 9;
	if (argc < 0 ||
	    argc > std::numeric_limits<int>::max() - maximum_appended_arguments ||
	    (argc != 0 && argv == nullptr)) {
		prepared.valid = false;
		return prepared;
	}
	prepared.arguments.reserve(static_cast<std::size_t>(argc) +
		static_cast<std::size_t>(maximum_appended_arguments));
	for (int index = 0; index < argc; ++index) {
		if (argv[index] == nullptr) {
			prepared.valid = false;
			return prepared;
		}
		const std::string_view argument{argv[index]};
		constexpr std::string_view prefix{"--execution-engine="};
		if (argument.starts_with(prefix)) {
			if (!parse_execution_engine(
					argument.substr(prefix.size()), prepared.engine)) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument == "--execution-engine") {
			if (++index >= argc || argv[index] == nullptr ||
				!parse_execution_engine(argv[index], prepared.engine)) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument == "--guest-top-byte-ignore") {
			prepared.top_byte_ignore = true;
			continue;
		}
		if (argument == "--guest-no-top-byte-ignore") {
			prepared.top_byte_ignore = false;
			continue;
		}
		if (argument.starts_with("--jit-gadget-fallback")) {
			prepared.valid = false;
			return prepared;
		}
		if (argument.starts_with(jit_cache_budget_prefix)) {
			/* Host-only, so it is consumed rather than handed to the
			 * guest kernel.
			 */
			const std::string quantity{argument.substr(
				jit_cache_budget_prefix.size())};
			prepared.jit_cache_budget =
				::uml::runtime::parse_translation_cache_quantity(
					quantity.c_str());
			if (prepared.jit_cache_budget == 0) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(gadget_cache_budget_prefix)) {
			const std::string quantity{argument.substr(
				gadget_cache_budget_prefix.size())};
			prepared.gadget_cache_budget =
				::uml::runtime::parse_translation_cache_quantity(
					quantity.c_str());
			if (prepared.gadget_cache_budget == 0) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(memory_limit_prefix)) {
			const std::string quantity{argument.substr(
				memory_limit_prefix.size())};
			prepared.memory_limit_bytes =
				::uml::runtime::parse_translation_cache_quantity(
					quantity.c_str());
			if (prepared.memory_limit_bytes == 0) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(session_record_prefix)) {
			prepared.session_record_path =
				argument.substr(session_record_prefix.size());
			if (prepared.session_record_path.empty()) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(guest_hibernation_prefix)) {
			if (!parse_guest_hibernation(
					argument.substr(guest_hibernation_prefix.size()),
					prepared.guest_hibernation)) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(jit_tier_prefix)) {
			/* A plain count of instructions; zero keeps the JIT on from
			 * the first. */
			const auto count = argument.substr(jit_tier_prefix.size());
			const auto *end = count.data() + count.size();
			const auto parsed = std::from_chars(count.data(), end,
				prepared.jit_tier_instructions);
			if (count.empty() || parsed.ec != std::errc{} ||
			    parsed.ptr != end) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		if (argument.starts_with(jit_executor_cache_budget_prefix)) {
			const std::string quantity{argument.substr(
				jit_executor_cache_budget_prefix.size())};
			prepared.jit_executor_cache_budget =
				::uml::runtime::parse_translation_cache_quantity(
					quantity.c_str());
			if (prepared.jit_executor_cache_budget == 0 ||
			    prepared.jit_executor_cache_budget >
				    maximum_jit_executor_cache_budget) {
				prepared.valid = false;
				return prepared;
			}
			continue;
		}
		prepared.arguments.emplace_back(argument);
	}
	if (prepared.arguments.empty())
		prepared.arguments.emplace_back("uml-darwin");
	if (informational(prepared.arguments)) {
		prepared.guest_memory_bytes = has_prefix(prepared.arguments, "mem=") ?
			configured_guest_memory_bytes(prepared.arguments) :
			64U * 1024U * 1024U;
		prepared.informational = true;
		return prepared;
	}
	// The unified rootfs is a raw EROFS image. The UBD `c` flag suppresses its
	// host-file lock so parallel guests can share the image; EROFS supplies the
	// read-only filesystem contract.
	if (!has_prefix(prepared.arguments, "ubd0")) {
		if (!::uml::block::usable_ubd_path(rootfs)) {
			prepared.valid = false;
			return prepared;
		}
		prepared.arguments.emplace_back("ubd0c=" + std::string(rootfs));
	}
	if (!has_prefix(prepared.arguments, "root="))
		prepared.arguments.emplace_back("root=/dev/ubda");
	if (!has_prefix(prepared.arguments, "rootfstype="))
		prepared.arguments.emplace_back("rootfstype=erofs");
	if (!has_prefix(prepared.arguments, "console=")) {
		prepared.arguments.emplace_back("console=tty0");
		prepared.arguments.emplace_back("console=ttyS0");
	}
	if (!has_prefix(prepared.arguments, "mem="))
		prepared.arguments.emplace_back("mem=256M");
	// Place the guest's management console where the host looks for it.
	for (auto &argument :
	     ::uml::device::guest_management_console_arguments()) {
		const auto name = argument.substr(0, argument.find('=') + 1U);
		if (has_prefix(prepared.arguments, name))
			continue;
		if (name == "uml_dir=")
			prepared.host_console_directory = true;
		prepared.arguments.push_back(std::move(argument));
	}
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	/* The compatibility slice does not support object-cgroup charging of
	 * kernel allocations. Keep ordinary userspace memory accounting enabled,
	 * and let an explicit kernel command-line policy take precedence. */
	if (!has_prefix(prepared.arguments, "cgroup.memory="))
		prepared.arguments.emplace_back("cgroup.memory=nokmem");
#endif
	prepared.guest_cpus = guest_cpu_count(prepared.arguments);
	prepared.guest_memory_bytes =
		configured_guest_memory_bytes(prepared.arguments);
	if (prepared.jit_cache_budget == 0)
		prepared.jit_cache_budget = prepared.guest_memory_bytes / 2U;
	return prepared;
}

} // namespace uml::emulator

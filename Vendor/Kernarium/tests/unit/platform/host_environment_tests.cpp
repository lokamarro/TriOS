// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_environment.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

using uml::testing::require;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

/* Leaves the environment and the launch option as the case found them. */
class LimitScope final {
public:
	LimitScope()
	{
		if (const char *value = std::getenv(platform::memory_limit_variable)) {
			had_ = true;
			saved_ = value;
		}
		::unsetenv(platform::memory_limit_variable);
		platform::set_configured_process_memory_limit(0);
	}

	~LimitScope()
	{
		platform::set_configured_process_memory_limit(0);
		if (had_)
			::setenv(platform::memory_limit_variable, saved_.c_str(), 1);
		else
			::unsetenv(platform::memory_limit_variable);
	}

	LimitScope(const LimitScope &) = delete;
	LimitScope &operator=(const LimitScope &) = delete;

private:
	bool had_{};
	std::string saved_;
};

void setVariable(const char *value)
{
	require(::setenv(platform::memory_limit_variable, value, 1) == 0);
}

void theVariableIsAWholeNumberOfMebibytes()
{
	require(platform::parse_memory_limit_mib("2867") == 2867 * mib);
	require(platform::parse_memory_limit_mib("1") == mib);
	require(platform::parse_memory_limit_mib("1048576") == 1048576 * mib);
	for (const char *refused : {"", "0", "00", "-1", "+5", "1.5", "2867M",
		     "2867 ", " 2867", "0x10", "1048577", "99999999999999999999"})
		require(platform::parse_memory_limit_mib(refused) == 0, refused);
	require(platform::parse_memory_limit_mib(nullptr) == 0);
}

void theHostsOwnLimitAlwaysWins()
{
	require(platform::resolve_process_memory_limit(0, 0) == 0);
	require(platform::resolve_process_memory_limit(0, 2867 * mib) ==
		2867 * mib);
	require(platform::resolve_process_memory_limit(2900 * mib, 0) ==
		2900 * mib);
	require(platform::resolve_process_memory_limit(2900 * mib, 1536 * mib) ==
		2900 * mib);
	require(platform::resolve_process_memory_limit(1536 * mib, 2900 * mib) ==
		1536 * mib);
}

void theLaunchStatesTheLimitByOptionOrVariable()
{
	const LimitScope scope;
	require(platform::configured_process_memory_limit_bytes() == 0);

	setVariable("2867");
	require(platform::configured_process_memory_limit_bytes() == 2867 * mib);

	/* The option is the launch's own word, so it beats the variable, and
	 * withdrawing it gives the variable back. */
	platform::set_configured_process_memory_limit(2048 * mib);
	require(platform::configured_process_memory_limit_bytes() == 2048 * mib);
	platform::set_configured_process_memory_limit(0);
	require(platform::configured_process_memory_limit_bytes() == 2867 * mib);

	/* A variable that is not a whole number of MiB states nothing. */
	setVariable("2867M");
	require(platform::configured_process_memory_limit_bytes() == 0);
	setVariable("");
	require(platform::configured_process_memory_limit_bytes() == 0);
}

void aHostWithoutALimitTakesTheStatedOne()
{
	const LimitScope scope;
	/* macOS sets no limit on an ordinary process. A host that does set one
	 * is iOS, where the stated limit changes nothing. */
	const auto host = platform::process_memory_limit_bytes();
	require(!platform::process_memory_limit_is_virtual());

	setVariable("2867");
	if (host == 0) {
		require(platform::process_memory_limit_bytes() == 2867 * mib);
		require(platform::process_memory_limit_is_virtual());
		platform::set_configured_process_memory_limit(1536 * mib);
		require(platform::process_memory_limit_bytes() == 1536 * mib);
		platform::set_configured_process_memory_limit(0);
	} else {
		require(!platform::process_memory_limit_is_virtual());
	}

	::unsetenv(platform::memory_limit_variable);
	require(platform::process_memory_limit_bytes() == host);
	require(!platform::process_memory_limit_is_virtual());
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-variable-is-a-whole-number-of-mebibytes",
			theVariableIsAWholeNumberOfMebibytes},
		{"the-hosts-own-limit-always-wins", theHostsOwnLimitAlwaysWins},
		{"the-launch-states-the-limit-by-option-or-variable",
			theLaunchStatesTheLimitByOptionOrVariable},
		{"a-host-without-a-limit-takes-the-stated-one",
			aHostWithoutALimitTakesTheStatedOne},
	};
	return uml::testing::runNamedTests(tests);
}

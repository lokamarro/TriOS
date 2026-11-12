// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <span>
#include <type_traits>
#include <vector>

namespace uml::testing {

/* Wire records as bytes and back, for the device tests that build a request
 * and read a reply. The records are trivially copyable structs in the host's
 * byte order. */

/* Appends the bytes of `value` to `data`. */
template<typename Value>
void append(std::vector<std::uint8_t> &data, const Value &value)
{
	static_assert(std::is_trivially_copyable_v<Value>);
	const auto size = data.size();
	data.resize(size + sizeof(value));
	std::memcpy(data.data() + size, &value, sizeof(value));
}

/* The bytes of `values`, one after another. */
template<typename... Values>
[[nodiscard]] std::vector<std::uint8_t> bytes(const Values &...values)
{
	static_assert((std::is_trivially_copyable_v<Values> && ...));
	std::vector<std::uint8_t> result;
	result.reserve((sizeof(values) + ... + 0U));
	(append(result, values), ...);
	return result;
}

/* The `Value` that starts `offset` bytes into `source`, which must hold all of
 * it. */
template<typename Value>
[[nodiscard]] Value load(std::span<const std::uint8_t> source,
	std::size_t offset = 0,
	const std::source_location location = std::source_location::current())
{
	static_assert(std::is_trivially_copyable_v<Value>);
	require(offset <= source.size() &&
			sizeof(Value) <= source.size() - offset,
		"the bytes are too short for the record", location);
	Value result{};
	std::memcpy(&result, source.data() + offset, sizeof(result));
	return result;
}

/* The `Value` that `source` is exactly the size of. */
template<typename Value>
[[nodiscard]] Value loadExact(std::span<const std::uint8_t> source,
	const std::source_location location = std::source_location::current())
{
	require(source.size() == sizeof(Value),
		"the bytes are not the size of the record", location);
	return load<Value>(source, 0, location);
}

} // namespace uml::testing

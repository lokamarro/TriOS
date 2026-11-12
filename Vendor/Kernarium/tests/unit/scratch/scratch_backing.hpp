// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "scratch_directory.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace uml::testing {

/* A scratch file that stands in for guest memory. It is unlinked as soon as
 * it exists, so the descriptor is its only reference. `name` is a
 * scratch_template() name ending in XXXXXX; each test keeps its own prefix so
 * a file a crash leaves behind names its test. Every failure aborts with the
 * caller's location. */
class ScratchBacking {
public:
	ScratchBacking(std::string_view name, std::size_t size,
		const std::source_location location =
			std::source_location::current())
	{
		auto path = scratch_template(name);
		descriptor_ = mkstemp(path.data());
		check(descriptor_ >= 0, "mkstemp", location);
		check(unlink(path.c_str()) == 0, "unlink", location);
		check(ftruncate(descriptor_, static_cast<off_t>(size)) == 0,
			"ftruncate", location);
	}

	~ScratchBacking()
	{
		if (descriptor_ >= 0)
			(void)close(descriptor_);
	}

	ScratchBacking(const ScratchBacking &) = delete;
	ScratchBacking &operator=(const ScratchBacking &) = delete;

	void put(std::size_t offset, std::span<const std::byte> bytes,
		const std::source_location location =
			std::source_location::current())
	{
		check(pwrite(descriptor_, bytes.data(), bytes.size(),
			      static_cast<off_t>(offset)) ==
				static_cast<ssize_t>(bytes.size()),
			"pwrite", location);
	}

	void get(std::size_t offset, std::span<std::byte> bytes,
		const std::source_location location =
			std::source_location::current()) const
	{
		check(pread(descriptor_, bytes.data(), bytes.size(),
			      static_cast<off_t>(offset)) ==
				static_cast<ssize_t>(bytes.size()),
			"pread", location);
	}

	void put_byte(std::size_t offset, std::byte value,
		const std::source_location location =
			std::source_location::current())
	{
		put(offset, std::span{&value, 1}, location);
	}

	void put32(std::size_t offset, std::uint32_t value,
		const std::source_location location =
			std::source_location::current())
	{
		put(offset, std::as_bytes(std::span{&value, 1}), location);
	}

	void put64(std::size_t offset, std::uint64_t value,
		const std::source_location location =
			std::source_location::current())
	{
		put(offset, std::as_bytes(std::span{&value, 1}), location);
	}

	[[nodiscard]] std::byte get_byte(std::size_t offset,
		const std::source_location location =
			std::source_location::current()) const
	{
		std::byte value{};
		get(offset, std::span{&value, 1}, location);
		return value;
	}

	[[nodiscard]] std::uint32_t get32(std::size_t offset,
		const std::source_location location =
			std::source_location::current()) const
	{
		std::uint32_t value{};
		get(offset, std::as_writable_bytes(std::span{&value, 1}),
			location);
		return value;
	}

	[[nodiscard]] std::uint64_t get64(std::size_t offset,
		const std::source_location location =
			std::source_location::current()) const
	{
		std::uint64_t value{};
		get(offset, std::as_writable_bytes(std::span{&value, 1}),
			location);
		return value;
	}

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
	static void check(bool condition, const char *operation,
		const std::source_location &location)
	{
		if (condition)
			return;
		std::fprintf(stderr, "%s:%u: scratch backing %s failed: %s\n",
			location.file_name(),
			static_cast<unsigned>(location.line()), operation,
			std::strerror(errno));
		std::abort();
	}

	int descriptor_{-1};
};

} // namespace uml::testing

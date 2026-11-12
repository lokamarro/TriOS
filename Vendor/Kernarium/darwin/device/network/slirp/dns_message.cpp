// SPDX-License-Identifier: GPL-2.0-or-later
#include "dns_message.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace uml::network::dns {
namespace {

constexpr std::size_t header_size = 12;
constexpr std::size_t max_name = 255;
constexpr std::size_t max_label = 63;
constexpr std::uint16_t flag_response = 0x8000;
constexpr std::uint16_t opcode_mask = 0x7800;
constexpr std::uint16_t flag_truncated = 0x0200;
constexpr std::uint16_t flag_recursion_desired = 0x0100;
constexpr std::uint16_t flag_recursion_available = 0x0080;
constexpr std::uint16_t flag_checking_disabled = 0x0010;
/* What this server accepts from the guest (RFC 9715's common value); the
 * guest's own advertisement bounds what it is sent. */
constexpr std::uint16_t advertised_payload = 1232;

std::uint16_t read16(std::span<const std::uint8_t> bytes,
		     std::size_t offset) noexcept
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) |
					  bytes[offset + 1]);
}

void append16(std::vector<std::uint8_t> &out, std::uint16_t value)
{
	out.push_back(static_cast<std::uint8_t>(value >> 8));
	out.push_back(static_cast<std::uint8_t>(value));
}

void append32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
	append16(out, static_cast<std::uint16_t>(value >> 16));
	append16(out, static_cast<std::uint16_t>(value));
}

/* An uncompressed name at `offset`; returns the offset past it. */
std::optional<std::size_t> read_plain_name(std::span<const std::uint8_t> bytes,
					   std::size_t offset,
					   std::vector<std::uint8_t> *name)
{
	std::size_t length = 0;
	for (;;) {
		if (offset >= bytes.size())
			return std::nullopt;
		const std::uint8_t label = bytes[offset];
		if (label > max_label)
			return std::nullopt; /* a pointer or a reserved label type */
		length += 1U + label;
		if (length > max_name || offset + 1U + label > bytes.size())
			return std::nullopt;
		if (name != nullptr)
			name->insert(name->end(), bytes.begin() + offset,
				     bytes.begin() + offset + 1 + label);
		offset += 1U + label;
		if (label == 0)
			return offset;
	}
}

/* Skips a possibly compressed name; a pointer ends it. */
std::optional<std::size_t> skip_name(std::span<const std::uint8_t> bytes,
				     std::size_t offset)
{
	for (;;) {
		if (offset >= bytes.size())
			return std::nullopt;
		const std::uint8_t label = bytes[offset];
		if ((label & 0xc0) == 0xc0)
			return offset + 2 <= bytes.size() ?
				std::optional{offset + 2} : std::nullopt;
		if (label > max_label)
			return std::nullopt;
		offset += 1U + label;
		if (label == 0)
			return offset <= bytes.size() ?
				std::optional{offset} : std::nullopt;
	}
}

std::uint8_t ascii_lower(std::uint8_t value) noexcept
{
	return value >= 'A' && value <= 'Z' ?
		static_cast<std::uint8_t>(value - 'A' + 'a') : value;
}

} // namespace

Parsed parse_query(std::span<const std::uint8_t> message)
{
	Parsed result;
	if (message.size() < header_size)
		return result;
	auto &query = result.query;
	query.id = read16(message, 0);
	query.flags = read16(message, 2);
	if ((query.flags & flag_response) != 0)
		return result;
	result.disposition = Disposition::answer;
	result.rcode = rcode_formerr;
	const std::uint16_t questions = read16(message, 4);
	const std::uint16_t answers = read16(message, 6);
	const std::uint16_t authorities = read16(message, 8);
	const std::uint16_t additionals = read16(message, 10);
	if (questions != 1)
		return result;
	const auto name_end = read_plain_name(message, header_size, &query.name);
	if (!name_end || *name_end + 4 > message.size()) {
		query.name.clear();
		return result;
	}
	query.type = read16(message, *name_end);
	query.klass = read16(message, *name_end + 2);
	std::size_t offset = *name_end + 4;
	query.question.assign(message.begin() + header_size,
			      message.begin() + offset);

	/* Walk the remaining sections for the OPT pseudo-record. */
	const unsigned int records = 0U + answers + authorities + additionals;
	for (unsigned int index = 0; index < records; ++index) {
		const auto owner = offset;
		const auto end = skip_name(message, offset);
		if (!end || *end + 10 > message.size())
			return result;
		const std::uint16_t type = read16(message, *end);
		const std::uint16_t data_length = read16(message, *end + 8);
		if (*end + 10 + data_length > message.size())
			return result;
		if (type == type_opt) {
			const bool additional =
				index >= 0U + answers + authorities;
			if (query.edns || !additional || message[owner] != 0)
				return result;
			query.edns = true;
			query.edns_payload = read16(message, *end + 2);
			query.edns_version = message[*end + 5];
		}
		offset = *end + 10 + data_length;
	}

	if ((query.flags & opcode_mask) != 0) {
		result.rcode = rcode_notimp;
		return result;
	}
	if (query.edns && query.edns_version != 0) {
		result.rcode = rcode_badvers;
		return result;
	}
	if (query.type == 0 || query.type == type_opt)
		return result;
	if (query.type == type_axfr || query.type == type_ixfr) {
		result.rcode = rcode_notimp;
		return result;
	}
	result.disposition = Disposition::resolve;
	result.rcode = rcode_noerror;
	return result;
}

std::size_t udp_limit(const Query &query) noexcept
{
	if (!query.edns)
		return udp_classic_limit;
	return std::clamp<std::size_t>(query.edns_payload, udp_classic_limit,
				       udp_link_limit);
}

std::vector<std::uint8_t> build_response(
	const Query &query, std::uint16_t rcode,
	std::span<const Record> answers, std::size_t limit)
{
	const auto build = [&](std::span<const Record> records,
			       std::uint16_t code, bool truncated) {
		std::vector<std::uint8_t> out;
		out.reserve(512);
		append16(out, query.id);
		std::uint16_t flags = flag_response | flag_recursion_available;
		flags |= query.flags & (opcode_mask | flag_recursion_desired |
					flag_checking_disabled);
		if (truncated)
			flags |= flag_truncated;
		flags |= code & 0x0f;
		append16(out, flags);
		append16(out, query.question.empty() ? 0 : 1);
		append16(out, static_cast<std::uint16_t>(records.size()));
		append16(out, 0);
		append16(out, query.edns ? 1 : 0);
		out.insert(out.end(), query.question.begin(),
			   query.question.end());
		for (const auto &record : records) {
			/* The question's name sits at offset 12. */
			if (!query.question.empty() &&
			    same_name(record.owner, query.name)) {
				out.push_back(0xc0);
				out.push_back(header_size);
			} else {
				out.insert(out.end(), record.owner.begin(),
					   record.owner.end());
			}
			append16(out, record.type);
			append16(out, record.klass);
			append32(out, record.ttl);
			append16(out, static_cast<std::uint16_t>(
					      record.data.size()));
			out.insert(out.end(), record.data.begin(),
				   record.data.end());
		}
		if (query.edns) {
			out.push_back(0); /* root owner */
			append16(out, type_opt);
			append16(out, advertised_payload);
			out.push_back(static_cast<std::uint8_t>(code >> 4));
			out.push_back(0); /* version */
			append16(out, 0); /* flags */
			append16(out, 0); /* no options */
		}
		return out;
	};

	if (answers.size() > 0xffff ||
	    std::ranges::any_of(answers, [](const Record &record) {
		    return record.data.size() > 0xffff;
	    }))
		return build({}, rcode_servfail, false);
	auto out = build(answers, rcode, false);
	if (out.size() <= limit)
		return out;
	if (limit >= tcp_limit)
		return build({}, rcode_servfail, false);
	return build({}, rcode, true);
}

std::optional<std::vector<std::uint8_t>> wire_name(
	std::string_view presentation)
{
	std::vector<std::uint8_t> out;
	if (presentation.empty() || presentation == ".") {
		out.push_back(0);
		return out;
	}
	std::vector<std::uint8_t> label;
	const auto finish_label = [&]() {
		if (label.empty() || label.size() > max_label)
			return false;
		out.push_back(static_cast<std::uint8_t>(label.size()));
		out.insert(out.end(), label.begin(), label.end());
		label.clear();
		return out.size() < max_name;
	};
	for (std::size_t index = 0; index < presentation.size(); ++index) {
		const auto character =
			static_cast<std::uint8_t>(presentation[index]);
		if (character == '.') {
			if (!finish_label())
				return std::nullopt;
			continue;
		}
		if (character != '\\') {
			label.push_back(character);
			continue;
		}
		if (++index >= presentation.size())
			return std::nullopt;
		const auto digit = [&](std::size_t at) {
			return at < presentation.size() &&
				presentation[at] >= '0' && presentation[at] <= '9';
		};
		if (digit(index) && digit(index + 1) && digit(index + 2)) {
			const int value = (presentation[index] - '0') * 100 +
				(presentation[index + 1] - '0') * 10 +
				(presentation[index + 2] - '0');
			if (value > 255)
				return std::nullopt;
			label.push_back(static_cast<std::uint8_t>(value));
			index += 2;
		} else {
			label.push_back(
				static_cast<std::uint8_t>(presentation[index]));
		}
	}
	if (!label.empty() && !finish_label())
		return std::nullopt;
	out.push_back(0);
	if (out.size() > max_name)
		return std::nullopt;
	return out;
}

std::string presentation_name(std::span<const std::uint8_t> wire)
{
	std::string out;
	std::size_t offset = 0;
	while (offset < wire.size() && wire[offset] != 0) {
		const std::size_t length = wire[offset];
		if (offset + 1 + length > wire.size())
			break;
		for (std::size_t index = 0; index < length; ++index) {
			const std::uint8_t character = wire[offset + 1 + index];
			if (character == '.' || character == '\\') {
				out.push_back('\\');
				out.push_back(static_cast<char>(character));
			} else if (character > 0x20 && character < 0x7f) {
				out.push_back(static_cast<char>(character));
			} else {
				std::array<char, 5> escape{};
				std::snprintf(escape.data(), escape.size(),
					      "\\%03u",
					      static_cast<unsigned int>(character));
				out.append(escape.data());
			}
		}
		out.push_back('.');
		offset += 1 + length;
	}
	if (out.empty())
		out.push_back('.');
	return out;
}

bool same_name(std::span<const std::uint8_t> left,
	       std::span<const std::uint8_t> right) noexcept
{
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index)
		if (ascii_lower(left[index]) != ascii_lower(right[index]))
			return false;
	return true;
}

std::optional<std::vector<std::uint8_t>> cname_target(
	std::span<const std::uint8_t> data)
{
	std::vector<std::uint8_t> name;
	const auto end = read_plain_name(data, 0, &name);
	if (!end || *end != data.size())
		return std::nullopt;
	return name;
}

} // namespace uml::network::dns

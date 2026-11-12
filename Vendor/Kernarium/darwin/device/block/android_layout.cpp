// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_layout.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <sys/stat.h>

namespace uml::android {
namespace {

constexpr std::size_t maximumLayoutBytes = mebibyte;
constexpr unsigned int maximumJsonDepth = 8;
constexpr std::uint64_t schemaVersion = 2;
constexpr std::uint64_t maximumEntryCount = 4096;

[[noreturn]] void bad(const std::string &message)
{
	throw std::invalid_argument("partition layout: " + message);
}

/* The JSON subset the layout uses: objects, arrays, strings, non-negative
 * integers and null. Duplicate keys, other numbers, literals and non-ASCII
 * text are errors. */
struct Json {
	enum class Kind { null, integer, string, array, object };

	Kind kind = Kind::null;
	std::uint64_t integer = 0;
	std::string string;
	std::vector<Json> array;
	std::vector<std::pair<std::string, Json>> object;

	const Json *find(std::string_view key) const
	{
		for (const auto &member : object)
			if (member.first == key)
				return &member.second;
		return nullptr;
	}
};

class JsonParser final {
public:
	explicit JsonParser(std::string_view text) : text_(text) {}

	Json parse()
	{
		auto value = parseValue(0);
		skipSpace();
		if (position_ != text_.size())
			bad("unexpected text after the document");
		return value;
	}

private:
	void skipSpace()
	{
		while (position_ < text_.size() &&
		       (text_[position_] == ' ' || text_[position_] == '\n' ||
			text_[position_] == '\r' || text_[position_] == '\t'))
			++position_;
	}

	char peek()
	{
		skipSpace();
		if (position_ >= text_.size())
			bad("the document ends early");
		return text_[position_];
	}

	void expect(char character)
	{
		if (peek() != character)
			bad(std::string{"expected '"} + character + "'");
		++position_;
	}

	Json parseValue(unsigned int depth)
	{
		if (depth > maximumJsonDepth)
			bad("the document nests too deeply");
		Json value;
		const char first = peek();
		if (first == '{') {
			++position_;
			value.kind = Json::Kind::object;
			if (peek() == '}') {
				++position_;
				return value;
			}
			for (;;) {
				if (peek() != '"')
					bad("an object key must be a string");
				auto key = parseString();
				if (value.find(key) != nullptr)
					bad("duplicate key " + key);
				expect(':');
				auto member = parseValue(depth + 1U);
				value.object.emplace_back(std::move(key), std::move(member));
				if (peek() == ',') {
					++position_;
					continue;
				}
				expect('}');
				return value;
			}
		}
		if (first == '[') {
			++position_;
			value.kind = Json::Kind::array;
			if (peek() == ']') {
				++position_;
				return value;
			}
			for (;;) {
				value.array.push_back(parseValue(depth + 1U));
				if (peek() == ',') {
					++position_;
					continue;
				}
				expect(']');
				return value;
			}
		}
		if (first == '"') {
			value.kind = Json::Kind::string;
			value.string = parseString();
			return value;
		}
		if (first >= '0' && first <= '9') {
			value.kind = Json::Kind::integer;
			value.integer = parseInteger();
			return value;
		}
		if (text_.substr(position_, 4) == "null") {
			position_ += 4;
			return value;
		}
		bad("unexpected character in the document");
	}

	std::uint64_t parseInteger()
	{
		const auto begin = position_;
		std::uint64_t value = 0;
		while (position_ < text_.size() && text_[position_] >= '0' &&
		       text_[position_] <= '9') {
			const auto digit = static_cast<std::uint64_t>(text_[position_] - '0');
			if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
				bad("an integer is too large");
			value = value * 10U + digit;
			++position_;
		}
		if (position_ - begin > 1U && text_[begin] == '0')
			bad("an integer has a leading zero");
		if (position_ < text_.size() &&
		    (text_[position_] == '.' || text_[position_] == 'e' ||
		     text_[position_] == 'E'))
			bad("numbers must be non-negative integers");
		return value;
	}

	std::string parseString()
	{
		++position_;
		std::string result;
		for (;;) {
			if (position_ >= text_.size())
				bad("a string is not terminated");
			const char character = text_[position_++];
			if (character == '"')
				return result;
			const auto byte = static_cast<unsigned char>(character);
			if (byte < 0x20U || byte >= 0x7fU)
				bad("a string has a control or non-ASCII character");
			if (character != '\\') {
				result.push_back(character);
				continue;
			}
			if (position_ >= text_.size())
				bad("a string is not terminated");
			const char escape = text_[position_++];
			switch (escape) {
			case '"':
			case '\\':
			case '/':
				result.push_back(escape);
				break;
			case 'b':
				result.push_back('\b');
				break;
			case 'f':
				result.push_back('\f');
				break;
			case 'n':
				result.push_back('\n');
				break;
			case 'r':
				result.push_back('\r');
				break;
			case 't':
				result.push_back('\t');
				break;
			default:
				bad("a string has an unsupported escape");
			}
		}
	}

	std::string_view text_;
	std::size_t position_ = 0;
};

void requireKeys(const Json &object, std::initializer_list<std::string_view> keys,
		 const char *label)
{
	if (object.kind != Json::Kind::object)
		bad(std::string{label} + " must be an object");
	if (object.object.size() != keys.size())
		bad(std::string{label} + " keys differ from the contract");
	for (const auto key : keys)
		if (object.find(key) == nullptr)
			bad(std::string{label} + " lacks " + std::string{key});
}

const Json &member(const Json &object, std::string_view key)
{
	const auto *found = object.find(key);
	if (found == nullptr)
		bad("missing " + std::string{key});
	return *found;
}

std::uint64_t integerOf(const Json &object, std::string_view key,
			std::uint64_t minimum = 0)
{
	const auto &value = member(object, key);
	if (value.kind != Json::Kind::integer || value.integer < minimum)
		bad(std::string{key} + " must be an integer of at least " +
		    std::to_string(minimum));
	return value.integer;
}

const std::string &stringOf(const Json &object, std::string_view key)
{
	const auto &value = member(object, key);
	if (value.kind != Json::Kind::string || value.string.empty())
		bad(std::string{key} + " must be a non-empty string");
	return value.string;
}

Guid guidOf(const Json &object, std::string_view key)
{
	try {
		return parseGuid(stringOf(object, key));
	} catch (const std::invalid_argument &) {
		bad(std::string{key} + " is not a canonical lower-case UUID");
	}
}

std::string nameOf(const Json &object)
{
	const auto &name = stringOf(object, "name");
	const bool valid = name.size() <= 36U && name[0] >= 'a' && name[0] <= 'z' &&
		std::all_of(name.begin(), name.end(), [](char character) {
			return (character >= 'a' && character <= 'z') ||
			       (character >= '0' && character <= '9') ||
			       character == '_';
		});
	if (!valid)
		bad("unsafe partition name " + name);
	return name;
}

std::string digestOf(const Json &object)
{
	const auto &digest = stringOf(object, "sha256");
	const bool valid = digest.size() == 64U &&
		std::all_of(digest.begin(), digest.end(), [](char character) {
			return (character >= '0' && character <= '9') ||
			       (character >= 'a' && character <= 'f');
		});
	if (!valid)
		bad("sha256 is not a lower-case hexadecimal digest");
	return digest;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
	return (value + alignment - 1U) / alignment * alignment;
}

std::uint32_t entryCountOf(const Json &object)
{
	const auto count = integerOf(object, "partition_entry_count", 1);
	if (count > maximumEntryCount ||
	    count * gptEntrySize % sectorSize != 0)
		bad("partition entry count is unsupported");
	return static_cast<std::uint32_t>(count);
}

DataDiskTemplate parseDataDisk(const Json &data,
			       const std::set<std::string> &systemNames)
{
	requireKeys(data,
		    {"layout_version", "alignment_bytes", "partition_entry_count",
		     "disk_guid", "trailing_free_bytes", "capacity_gib", "partitions"},
		    "data disk");
	DataDiskTemplate result {};
	const auto version = integerOf(data, "layout_version", 1);
	if (version > std::numeric_limits<std::uint32_t>::max())
		bad("layout_version is too large");
	result.layoutVersion = static_cast<std::uint32_t>(version);
	result.alignment = integerOf(data, "alignment_bytes", 4096);
	if (result.alignment % 4096U != 0)
		bad("data disk alignment must be a multiple of 4 KiB");
	result.entryCount = entryCountOf(data);
	result.diskGuid = guidOf(data, "disk_guid");
	result.trailingFreeBytes = integerOf(data, "trailing_free_bytes", 1);
	if (result.trailingFreeBytes % result.alignment != 0)
		bad("data disk trailing space must be aligned");
	const auto &capacity = member(data, "capacity_gib");
	requireKeys(capacity, {"minimum", "default", "maximum"}, "data disk capacity");
	const auto minimum = integerOf(capacity, "minimum", 1);
	const auto fallback = integerOf(capacity, "default", minimum);
	const auto maximum = integerOf(capacity, "maximum", fallback);
	if (maximum > 1U << 20U)
		bad("data disk capacity range is unreasonable");
	result.minimumGiB = static_cast<std::uint32_t>(minimum);
	result.defaultGiB = static_cast<std::uint32_t>(fallback);
	result.maximumGiB = static_cast<std::uint32_t>(maximum);

	const auto &partitions = member(data, "partitions");
	constexpr std::array<std::string_view, 3> expectedNames {
		"misc", "metadata", "userdata"};
	if (partitions.kind != Json::Kind::array ||
	    partitions.array.size() != expectedNames.size())
		bad("data disk partitions must be misc, metadata and userdata");
	std::uint64_t previousEnd = 0;
	std::set<std::array<std::uint8_t, 16>> uniques;
	for (std::size_t index = 0; index < expectedNames.size(); ++index) {
		const auto &item = partitions.array[index];
		requireKeys(item,
			    {"name", "start_bytes", "size_bytes", "type_guid",
			     "unique_guid"},
			    "data partition");
		DataPartition partition {};
		partition.name = nameOf(item);
		if (partition.name != expectedNames[index])
			bad("data partition " + std::to_string(index) + " must be " +
			    std::string{expectedNames[index]});
		if (systemNames.contains(partition.name))
			bad("data partition " + partition.name +
			    " repeats a system disk name");
		partition.offset = integerOf(item, "start_bytes", result.alignment);
		const bool last = index + 1U == expectedNames.size();
		std::uint64_t size = 0;
		const auto &sizeValue = member(item, "size_bytes");
		if (last) {
			if (sizeValue.kind != Json::Kind::null)
				bad("the last data partition must fill the disk");
		} else {
			size = integerOf(item, "size_bytes", 1);
			if (size % result.alignment != 0)
				bad("data partition " + partition.name + " is not aligned");
			partition.size = size;
		}
		if (partition.offset % result.alignment != 0 ||
		    partition.offset < previousEnd)
			bad("data partition " + partition.name +
			    " is misaligned or out of order");
		previousEnd = partition.offset + size;
		partition.type = guidOf(item, "type_guid");
		partition.unique = guidOf(item, "unique_guid");
		if (!uniques.insert(partition.unique.bytes).second)
			bad("data partition " + partition.name +
			    " duplicates a unique GUID");
		result.partitions.push_back(std::move(partition));
	}
	if (result.partitions.back().offset + result.trailingFreeBytes >=
	    static_cast<std::uint64_t>(result.minimumGiB) * gibibyte)
		bad("the smallest data disk leaves userdata no room");
	return result;
}

} // namespace

const DataPartition &DataDiskTemplate::partition(std::string_view name) const
{
	for (const auto &candidate : partitions)
		if (candidate.name == name)
			return candidate;
	throw std::invalid_argument("the data disk has no partition " +
				    std::string{name});
}

const SystemPartition &Layout::systemPartition(std::string_view name) const
{
	for (const auto &candidate : systemPartitions)
		if (candidate.name == name)
			return candidate;
	throw std::invalid_argument("the system disk has no partition " +
				    std::string{name});
}

GptImage Layout::systemGpt() const
{
	std::vector<GptPartition> partitions;
	partitions.reserve(systemPartitions.size());
	for (const auto &partition : systemPartitions)
		partitions.push_back(GptPartition{partition.name, partition.offset,
						  partition.size, partition.type,
						  partition.unique});
	return buildGpt(systemDiskBytes, systemEntryCount, systemDiskGuid, partitions);
}

GptImage Layout::dataGpt(std::uint32_t gib) const
{
	if (gib < data.minimumGiB || gib > data.maximumGiB)
		throw std::invalid_argument(
			"the data disk capacity must be " +
			std::to_string(data.minimumGiB) + " through " +
			std::to_string(data.maximumGiB) + " GiB");
	const auto bytes = static_cast<std::uint64_t>(gib) * gibibyte;
	std::vector<GptPartition> partitions;
	for (const auto &partition : data.partitions) {
		const auto size = partition.size.value_or(
			bytes - data.trailingFreeBytes - partition.offset);
		partitions.push_back(GptPartition{partition.name, partition.offset, size,
						  partition.type, partition.unique});
	}
	return buildGpt(bytes, data.entryCount, data.diskGuid, partitions);
}

Layout parseLayout(std::string_view jsonText)
{
	if (jsonText.size() > maximumLayoutBytes)
		bad("the file is too large");
	const auto document = JsonParser{jsonText}.parse();
	requireKeys(document, {"schema_version", "sector_size", "system_disk", "data_disk"},
		    "layout");
	if (integerOf(document, "schema_version") != schemaVersion)
		bad("only schema version 2 is supported");
	if (integerOf(document, "sector_size") != sectorSize)
		bad("the sector size must be 512 bytes");

	const auto &system = member(document, "system_disk");
	requireKeys(system,
		    {"size_bytes", "sha256", "alignment_bytes",
		     "partition_entry_count", "disk_guid", "partitions"},
		    "system disk");
	Layout layout {};
	layout.systemDiskBytes = integerOf(system, "size_bytes", sectorSize);
	layout.systemAlignment = integerOf(system, "alignment_bytes", 4096);
	layout.systemEntryCount = entryCountOf(system);
	if (layout.systemDiskBytes % layout.systemAlignment != 0 ||
	    layout.systemAlignment % 4096U != 0)
		bad("the system disk size and alignment must be multiples of 4 KiB");
	layout.systemDiskSha256 = digestOf(system);
	layout.systemDiskGuid = guidOf(system, "disk_guid");

	const auto &partitions = member(system, "partitions");
	if (partitions.kind != Json::Kind::array || partitions.array.empty() ||
	    partitions.array.size() > layout.systemEntryCount)
		bad("system disk partitions must be a non-empty array");
	const auto tableSectors = layout.systemEntryCount * gptEntrySize / sectorSize;
	/* Exact fit: the first partition follows the table at the next alignment
	 * boundary and each later one starts where its predecessor ends. */
	auto cursor = alignUp((2U + tableSectors) * sectorSize, layout.systemAlignment);
	std::set<std::string> names;
	std::set<std::array<std::uint8_t, 16>> uniques;
	for (const auto &item : partitions.array) {
		requireKeys(item,
			    {"name", "start_bytes", "size_bytes", "image_bytes", "sha256",
			     "type_guid", "unique_guid"},
			    "system partition");
		SystemPartition partition {};
		partition.name = nameOf(item);
		partition.offset = integerOf(item, "start_bytes");
		partition.size = integerOf(item, "size_bytes", layout.systemAlignment);
		partition.imageBytes = integerOf(item, "image_bytes", 1);
		partition.sha256 = digestOf(item);
		if (partition.offset % layout.systemAlignment != 0 ||
		    partition.size % layout.systemAlignment != 0)
			bad("partition " + partition.name + " is not aligned");
		if (partition.imageBytes > partition.size ||
		    partition.size - partition.imageBytes >= layout.systemAlignment)
			bad("partition " + partition.name + " is not exact fit");
		if (partition.offset != cursor)
			bad("partition " + partition.name +
			    " is not packed against its predecessor");
		partition.type = guidOf(item, "type_guid");
		partition.unique = guidOf(item, "unique_guid");
		if (!names.insert(partition.name).second ||
		    !uniques.insert(partition.unique.bytes).second)
			bad("partition " + partition.name +
			    " duplicates a name or unique GUID");
		cursor = partition.offset + partition.size;
		layout.systemPartitions.push_back(std::move(partition));
	}
	if (cursor > (layout.systemDiskBytes / sectorSize - tableSectors - 1U) * sectorSize)
		bad("the last partition overlaps the backup GPT");
	if (alignUp(cursor + (tableSectors + 1U) * sectorSize, layout.systemAlignment) !=
	    layout.systemDiskBytes)
		bad("the system disk has bytes beyond its exact fit");
	for (const char *required : {"init_boot", "recovery"})
		if (!names.contains(required))
			bad(std::string{"the system disk lacks "} + required);
	layout.data = parseDataDisk(member(document, "data_disk"), names);
	return layout;
}

Layout readLayout(const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0)
		throw std::system_error(errno, std::generic_category(),
					"cannot inspect " + path.string());
	if (!S_ISREG(details.st_mode) || details.st_size <= 0 ||
	    static_cast<std::uint64_t>(details.st_size) > maximumLayoutBytes)
		throw std::invalid_argument(
			"the partition layout is not a regular file of at most 1 MiB: " +
			path.string());
	std::string text(static_cast<std::size_t>(details.st_size), '\0');
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
		throw std::system_error(errno, std::generic_category(),
					"cannot open " + path.string());
	const auto count = std::fread(text.data(), 1, text.size(), file);
	const int closed = std::fclose(file);
	if (count != text.size() || closed != 0)
		throw std::runtime_error("cannot read " + path.string());
	return parseLayout(text);
}

} // namespace uml::android

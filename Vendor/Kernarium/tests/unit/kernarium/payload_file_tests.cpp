// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumPayloadFile.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <sys/stat.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {

using kernarium::harness::readPayload;
using kernarium::harness::removalRequested;
using kernarium::harness::takePayload;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::string_view segment {"070701000000000000000000"};

void write(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "writing a test file");
}

/* A name that could leave Documents is refused before the file system is
 * consulted, and only a padded newc segment is a payload. */
void onlyAPaddedSegmentNamedByAPlainNameIsRead(
    const std::filesystem::path& documents)
{
    std::filesystem::create_directory(documents / "sub");
    write(documents / "sub" / "inner.cpio", segment);
    for (const char* name : {"", ".", "..", "../outside.cpio",
             "sub/inner.cpio", "./sub/inner.cpio", "/outside.cpio"})
        requireThrows<std::invalid_argument>(
            [&] { (void)readPayload(documents, name); },
            "a name that is not a plain file name was read");
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "absent.cpio"); },
        "an absent payload was read");

    write(documents / "short.cpio", "07070");
    write(documents / "unpadded.cpio", "07070100000");
    write(documents / "other.cpio", "070702000000000000000000");
    for (const char* name : {"short.cpio", "unpadded.cpio", "other.cpio"})
        requireThrows<std::invalid_argument>(
            [&] { (void)readPayload(documents, name); },
            "a file that is not a padded newc segment was read");

    write(documents / "good.cpio", segment);
    const auto bytes = readPayload(documents, "good.cpio");
    require(bytes.size() == segment.size(), "the payload size");
    require(std::memcmp(bytes.data(), segment.data(), segment.size()) == 0,
        "the payload bytes");

    std::filesystem::create_directory(documents / "directory.cpio");
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "directory.cpio"); },
        "a directory was read as a payload");
}

void aPayloadLargerThanTheKernelCanBootIsRefused(
    const std::filesystem::path& documents)
{
    const auto path = documents / "huge.cpio";
    write(path, segment);
    std::filesystem::resize_file(
        path, kernarium::harness::maximumPayloadBytes + 4U);
    requireThrows<std::invalid_argument>(
        [&] { (void)readPayload(documents, "huge.cpio"); },
        "a payload over 64 MiB was read");
    std::filesystem::resize_file(
        path, kernarium::harness::maximumPayloadBytes);
    require(readPayload(documents, "huge.cpio").size() ==
            kernarium::harness::maximumPayloadBytes,
        "a payload of exactly 64 MiB");
}

bool held(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

/* Only "1" asks for removal; a typo is refused rather than read as "keep". */
void onlyOneAsksForRemoval()
{
    require(!removalRequested(nullptr), "an unset setting asked");
    require(!removalRequested(""), "an empty setting asked");
    require(!removalRequested("0"), "0 asked");
    require(removalRequested("1"), "1 did not ask");
    for (const char* value : {"2", "01", "yes", "true", " 1", "1 "})
        requireThrows<std::invalid_argument>(
            [&] { (void)removalRequested(value); },
            "a setting other than 0 or 1 was accepted");
}

/* Without the request the file stays; with it, exactly the named file goes
 * once its bytes have been read, and a payload that was not accepted stays. */
void removalDeletesOnlyTheNamedPayloadAndOnlyOnRequest(
    const std::filesystem::path& documents)
{
    write(documents / "keep.cpio", segment);
    write(documents / "run.cpio", segment);
    write(documents / "other.cpio.0", segment);
    write(documents / "bad.cpio", "not a segment");
    std::error_code removal {std::make_error_code(std::errc::io_error)};

    auto bytes = takePayload(documents, "keep.cpio", false, removal);
    require(bytes.size() == segment.size(), "the kept payload's bytes");
    require(!removal, "a kept payload reported a removal error");
    require(held(documents / "keep.cpio"), "an unrequested removal ran");

    bytes = takePayload(documents, "run.cpio", true, removal);
    require(bytes.size() == segment.size(), "the removed payload's bytes");
    require(!removal, "a removed payload reported an error");
    require(!held(documents / "run.cpio"), "the payload was not removed");
    for (const char* name : {"keep.cpio", "other.cpio.0", "bad.cpio"})
        require(held(documents / name), "removal touched another file");

    requireThrows<std::invalid_argument>(
        [&] { (void)takePayload(documents, "run.cpio", true, removal); },
        "a payload that is gone was read");
    requireThrows<std::invalid_argument>(
        [&] { (void)takePayload(documents, "bad.cpio", true, removal); },
        "a malformed payload was read");
    require(held(documents / "bad.cpio"),
        "a payload the runtime refused was removed");
    for (const char* name : {"../outside.cpio", "sub/inner.cpio"})
        requireThrows<std::invalid_argument>(
            [&] { (void)takePayload(documents, name, true, removal); },
            "a name that is not a plain file name was removed");
    require(held(documents.parent_path() / "outside.cpio") &&
            held(documents / "sub" / "inner.cpio"),
        "removal left Documents");
}

/* A file the runtime cannot unlink is reported and the boot goes on with the
 * bytes it read. A directory without write permission refuses the unlink. */
void aFailedRemovalIsReportedAndDoesNotFailTheRead(
    const std::filesystem::path& documents)
{
    if (::geteuid() == 0)
        return;
    const auto directory = documents / "locked";
    std::filesystem::create_directory(directory);
    write(directory / "run.cpio", segment);
    require(::chmod(directory.c_str(), 0555) == 0, "locking the directory");
    std::error_code removal;
    const auto bytes = takePayload(directory, "run.cpio", true, removal);
    require(::chmod(directory.c_str(), 0755) == 0, "unlocking the directory");
    require(bytes.size() == segment.size(), "the unremovable payload's bytes");
    require(removal == std::errc::permission_denied, "the unlink error");
    require(held(directory / "run.cpio"), "the file left the directory");
}

} // namespace

int main()
{
    const uml::testing::ScratchSubdirectory scratch {
        "kernarium-payload-XXXXXX"};
    const auto documents = scratch.path() / "Documents";
    std::filesystem::create_directory(documents);
    write(scratch.path() / "outside.cpio", segment);
    onlyAPaddedSegmentNamedByAPlainNameIsRead(documents);
    aPayloadLargerThanTheKernelCanBootIsRefused(documents);
    onlyOneAsksForRemoval();
    removalDeletesOnlyTheNamedPayloadAndOnlyOnRequest(documents);
    aFailedRemovalIsReportedAndDoesNotFailTheRead(documents);
    std::puts("PASS: the runtime reads a boot payload and removes it only on "
              "request");
    return EXIT_SUCCESS;
}

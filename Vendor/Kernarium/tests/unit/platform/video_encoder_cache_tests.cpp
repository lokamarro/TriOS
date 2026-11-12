// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern char** environ;

/* The encoder's capability cache, through the processes that use it.
 *
 * Discovery encodes for seconds, so a process that finds a valid cache skips
 * it, and a process that finds anything else (nothing, a file of another
 * host or revision, a file that is not what a writer of this revision stores)
 * discovers again and leaves a valid file behind. Each case runs in a child
 * process because the discovery of a process happens once; the child prints
 * what it was told the host can do, and the parent compares. */

namespace {
using namespace uml::platform;
using uml::testing::require;
namespace fs = std::filesystem;

const char* codecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::h264: return "h264";
    case VideoCodec::hevc: return "hevc";
    case VideoCodec::jpeg: return "jpeg";
    case VideoCodec::vp9: return "vp9";
    }
    return "?";
}

// Everything the capabilities state, as text. The measured frame rates are
// last on their own line: they are measurements, so two discoveries differ in
// them and a cache is the only thing that repeats them.
std::string describe(const std::vector<VideoEncodeCapability>& capabilities)
{
    std::ostringstream out;
    for (const auto& c : capabilities) {
        out << codecName(c.codec) << " size " << c.minimumWidth << "x" << c.minimumHeight << ".." << c.maximumWidth << "x" << c.maximumHeight
            << " align " << c.sizeAlignment << " bitrate " << c.maximumBitrate << " qp " << c.minimumQp << ".." << c.maximumQp
            << " b " << c.maximumBFrames << " layers " << c.maximumTemporalLayers << " entropy " << c.entropyModeSelectable
            << " hdr " << c.hdrStaticMetadata << " sessions " << c.concurrentSessions << " rc";
        for (const auto mode : c.rateControls) out << ' ' << int(mode);
        out << " profiles";
        for (const auto& p : c.profiles) {
            out << " [" << int(p.profile) << " level " << p.maximumLevel << (p.levelSelectable ? "s" : "f") << " in";
            for (const auto layout : p.inputs) out << ' ' << int(layout);
            out << ']';
        }
        out << '\n';
    }
    for (const auto& c : capabilities)
        out << codecName(c.codec) << " fps " << c.measuredFramesPerSecond720p << " " << c.measuredFramesPerSecond1080p << '\n';
    return out.str();
}

// ---- The child.

int child()
{
    const auto capabilities = videoEncodeCapabilities();
    std::fputs(describe(capabilities).c_str(), stdout);
    return capabilities.empty() ? 3 : 0;
}

// ---- The parent.

std::string executablePath()
{
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    require(_NSGetExecutablePath(path.data(), &size) == 0, "executable path");
    path.resize(std::strlen(path.c_str()));
    return fs::canonical(path).string();
}

struct Result {
    std::string output;
    int status {-1};
    double seconds {};
};

// Runs this binary as the child with the cache variable set (or unset when
// `cache` is null). Waits at most `bound` seconds and kills the child it
// started if the bound passes.
Result runChild(const char* cache, unsigned bound)
{
    static const std::string self = executablePath();
    std::vector<std::string> environment;
    for (char** entry = environ; entry && *entry; ++entry)
        if (std::strncmp(*entry, "UML_VIDEO_ENCODE_CACHE=", 23) != 0) environment.emplace_back(*entry);
    if (cache) environment.push_back(std::string("UML_VIDEO_ENCODE_CACHE=") + cache);
    std::vector<char*> envp;
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);

    int pipes[2];
    require(::pipe(pipes) == 0, "pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    char* argv[] = {const_cast<char*>(self.c_str()), const_cast<char*>("--child"), nullptr};
    pid_t pid = 0;
    const auto start = std::chrono::steady_clock::now();
    require(posix_spawn(&pid, self.c_str(), &actions, nullptr, argv, envp.data()) == 0, "spawn");
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipes[1]);

    Result result;
    const auto deadline = start + std::chrono::seconds(bound);
    char buffer[4096];
    bool open = true;
    while (open && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor {pipes[0], POLLIN, 0};
        if (::poll(&descriptor, 1, 200) <= 0) continue;
        const auto got = ::read(pipes[0], buffer, sizeof buffer);
        if (got > 0) result.output.append(buffer, std::size_t(got));
        else open = false;
    }
    ::close(pipes[0]);
    if (open) {
        // The bound passed: the child is the one this test started.
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, &result.status, 0);
        require(false, "the child did not finish within its bound");
    }
    while (std::chrono::steady_clock::now() < deadline + std::chrono::seconds(10)) {
        const auto done = ::waitpid(pid, &result.status, WNOHANG);
        if (done == pid) break;
        usleep(20'000);
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

std::string slurp(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void write(const fs::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    require(bool(file), "write the cache file");
}

// Replaces the first `from` after `after` with `to`.
std::string replaceFirst(std::string text, const std::string& from, const std::string& to, std::size_t after = 0)
{
    const auto at = text.find(from, after);
    require(at != std::string::npos, ("the cache has no " + from).c_str());
    text.replace(at, from.size(), to);
    return text;
}

// The text of the first h264 line with the maximum width replaced.
std::string withWidth(std::string text, const std::string& width)
{
    const auto at = text.find("h264 size ");
    require(at != std::string::npos, "an h264 line");
    const auto dots = text.find("..", at);
    const auto x = text.find('x', dots);
    text.replace(dots + 2, x - dots - 2, width);
    return text;
}

// The minimum width and height that the first line of `codec` reports.
std::pair<unsigned, unsigned> minimumOf(const std::string& text, const char* codec)
{
    const std::string prefix = std::string(codec) + " size ";
    const auto at = text.find(prefix);
    require(at != std::string::npos, "a line for the codec");
    unsigned width = 0, height = 0;
    require(std::sscanf(text.c_str() + at + prefix.size(), "%ux%u", &width, &height) == 2, "a size on the line");
    return {width, height};
}

// No video codec lists a picture below the floor.
void requireFloor(const std::string& text, const char* what)
{
    for (const char* codec : {"h264", "hevc"}) {
        if (text.find(std::string(codec) + " size ") == std::string::npos) continue;
        const auto [width, height] = minimumOf(text, codec);
        require(width >= kVideoEncodeMinimumSide && height >= kVideoEncodeMinimumSide, what);
    }
}

void requireValidCache(const fs::path& path)
{
    const auto text = slurp(path);
    require(!text.empty() && text.front() == '{', "the cache is a JSON object");
    require(text.find("\"revision\":") != std::string::npos && text.find("\"host\":\"") != std::string::npos &&
            text.find("\"codecs\":[") != std::string::npos, "with a revision, a host and codecs");
}

void run(const fs::path& directory)
{
    const auto cache = directory / "capabilities.json";
    const std::string cachePath = cache.string();
    constexpr unsigned kBound = 240;

    // No file: the child discovers, answers, and leaves a cache.
    fs::remove(cache);
    const auto fresh = runChild(cachePath.c_str(), kBound);
    require(fresh.status == 0, "the first child succeeds");
    require(!fresh.output.empty(), "and reports capabilities");
    std::printf("discovery: %.1f s\n", fresh.seconds);
    std::fflush(stdout);
    requireValidCache(cache);
    requireFloor(fresh.output, "discovery lists a video picture below the floor");
    const auto stored = slurp(cache);

    // A valid file: the child reports exactly what was stored, fast, and does
    // not rewrite the file.
    const auto reused = runChild(cachePath.c_str(), kBound);
    require(reused.status == 0 && reused.output == fresh.output, "a valid cache is reused as it is, measurements included");
    std::printf("cached: %.1f s\n", reused.seconds);
    require(slurp(cache) == stored, "and left alone");

    // What the file says is what the child reports: change a value and the
    // change shows, which proves the child read the file and did not rediscover.
    const auto digitsAt = stored.find("\"maximumWidth\":") + std::strlen("\"maximumWidth\":");
    auto digitsEnd = digitsAt;
    while (digitsEnd < stored.size() && std::isdigit(static_cast<unsigned char>(stored[digitsEnd]))) ++digitsEnd;
    auto edited = stored;
    edited.replace(digitsAt, digitsEnd - digitsAt, "1234");
    write(cache, edited);
    const auto followed = runChild(cachePath.c_str(), kBound);
    require(followed.status == 0 && followed.output == withWidth(fresh.output, "1234"), "the child reports what the file says");
    require(slurp(cache) == edited, "and does not replace a valid file");

    // Files that cannot be vouched for are not used, and are replaced.
    struct Case {
        const char* what;
        std::string text;
    };
    std::vector<Case> invalid;
    invalid.push_back({"not JSON", "this is not a capability cache\n"});
    invalid.push_back({"another host", replaceFirst(edited, "\"host\":\"", "\"host\":\"elsewhere/")});
    invalid.push_back({"another revision", replaceFirst(edited, "\"revision\":", "\"revision\":9")});
    invalid.push_back({"a value out of range", replaceFirst(edited, "\"minimumQp\":", "\"minimumQp\":9")});
    invalid.push_back({"a missing field", replaceFirst(edited, "\"sizeAlignment\":", "\"sizeAlignmentX\":")});
    {
        // A result that lists a video picture below the floor, as one written before the floor was.
        const auto key = std::string("\"minimumWidth\":");
        const auto at = edited.find(key) + key.size();
        auto end = at;
        while (end < edited.size() && std::isdigit(static_cast<unsigned char>(edited[end]))) ++end;
        auto small = edited;
        small.replace(at, end - at, "16");
        invalid.push_back({"a size below the floor", small});
    }
    for (const auto& c : invalid) {
        write(cache, c.text);
        const auto answered = runChild(cachePath.c_str(), kBound);
        std::printf("%s: %.1f s\n", c.what, answered.seconds);
        std::fflush(stdout);
        require(answered.status == 0, c.what);
        // Discovered, not read: the edited width is gone.
        require(answered.output.find("..1234x") == std::string::npos && answered.output.find("h264 size") == 0, "an unusable cache is ignored");
        requireFloor(answered.output, "a rediscovery lists a video picture below the floor");
        requireValidCache(cache);
        require(slurp(cache) != c.text, "and replaced");
        // What replaced it is what a later process reuses.
        const auto again = runChild(cachePath.c_str(), kBound);
        require(again.status == 0 && again.output == answered.output, "the replacement is a valid cache");
    }

    // An empty variable turns the cache off: nothing is read and nothing is written.
    fs::remove(cache);
    write(cache, edited);
    const auto off = runChild("", kBound);
    require(off.status == 0, "a child with the cache off");
    require(off.output.find("..1234x") == std::string::npos && off.output.find("h264 size") == 0, "does not read the file");
    require(slurp(cache) == edited, "and does not write it");
    for (const auto& entry : fs::directory_iterator(directory))
        require(entry.path() == cache, "no stray file is left beside the cache");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) return child();
    if (argc != 1) {
        std::fprintf(stderr, "usage: uml_video_encoder_cache_tests\n");
        return 2;
    }
    try {
        const uml::testing::ScratchSubdirectory directory("video-encoder-cache-XXXXXX");
        run(directory.path());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder cache ok");
    return 0;
}

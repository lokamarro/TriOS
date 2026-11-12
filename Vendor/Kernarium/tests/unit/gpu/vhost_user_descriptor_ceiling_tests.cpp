// SPDX-License-Identifier: GPL-2.0-or-later
//
// A full host file table is how the GPU transport dies on a platform whose
// application ceiling is smaller than the mappings a guest asks for. These
// tests own that boundary: the ceiling is raised to whatever the host offers,
// and a refusal to install an arriving descriptor is reported as what it is
// rather than as Darwin's bare spelling for it, "Message too long".

#include "gpu_test_support.hpp"
#include "vhost_user_message.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace uml::gpu::testing {
namespace {

// Restores whatever ceiling the harness started this process with.
class CeilingGuard final {
public:
    CeilingGuard() { require(::getrlimit(RLIMIT_NOFILE, &saved_) == 0); }
    ~CeilingGuard() { (void)::setrlimit(RLIMIT_NOFILE, &saved_); }

    CeilingGuard(const CeilingGuard&) = delete;
    CeilingGuard& operator=(const CeilingGuard&) = delete;

    [[nodiscard]] const rlimit& saved() const noexcept { return saved_; }

    void apply(rlim_t soft) const
    {
        rlimit limit = saved_;
        limit.rlim_cur = soft;
        require(::setrlimit(RLIMIT_NOFILE, &limit) == 0);
    }

private:
    rlimit saved_ {};
};

// The ceiling an iOS application is given before anything asks for more.
constexpr rlim_t applicationCeiling = 256;

void raisingTakesEverythingTheHostOffers()
{
    const CeilingGuard guard;
    if (guard.saved().rlim_max <= applicationCeiling)
        return;
    guard.apply(applicationCeiling);
    require(descriptorCeiling().soft == applicationCeiling);

    const auto raised = raiseDescriptorCeiling();
    require(raised > applicationCeiling);
    require(descriptorCeiling().soft == raised);
    // Idempotent: a second connection must not lower what the first took.
    require(raiseDescriptorCeiling() == raised);
}

/* The margin left above what is already open before the table is filled. A
 * harness can hand this process a ceiling of a million, and filling that would
 * cost a million descriptors to prove what a few hundred prove just as well.
 *
 * It is deliberately larger than any file table this process starts with. A
 * ceiling lowered underneath an already-grown table leaves free slots above
 * it, and Darwin then refuses an arriving descriptor with EMFILE instead of
 * the EMSGSIZE a genuinely full table gives. Both are this exhaustion and the
 * transport names both, but the field failure was the full table, so grow into
 * the ceiling rather than dropping the ceiling onto the table.
 */
constexpr std::size_t fillMargin = 256;

// Opens descriptors until the table is full; the caller releases them.
[[nodiscard]] std::vector<int> fillDescriptorTable()
{
    std::vector<int> held;
    held.reserve(fillMargin + 1);
    for (;;) {
        const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
            break;
        held.push_back(descriptor);
    }
    return held;
}

void releaseDescriptors(std::vector<int>& held)
{
    for (const int descriptor : held)
        (void)::close(descriptor);
    held.clear();
}

void aFullTableNamesItself()
{
    const auto before = descriptorCount();
    {
        const CeilingGuard guard;
        auto sockets = makeSocketPair();
        auto carried = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = 1,
            .flags = protocol::vhostUserVersion,
            .size = 0,
        };
        // One message carries a descriptor, the one behind it carries none.
        // Both are queued before the table fills, because sending needs room.
        const std::array carriedDescriptors {carried.get()};
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, carriedDescriptors);
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, std::span<const int> {});

        // Bring the ceiling within reach of what is already open, so the fill
        // below grows the table into it rather than exhausting a real one.
        guard.apply(static_cast<rlim_t>(descriptorCount() + fillMargin));
        auto held = fillDescriptorTable();
        require(!held.empty());

        // The descriptor-bearing message is refused, and says why.
        std::string reported;
        try {
            VhostUserMessage message;
            (void)receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        } catch (const std::runtime_error& error) {
            reported = error.what();
        }
        if (reported.find("file table is full") == std::string_view::npos) {
            std::fprintf(
                stderr,
                "a refused descriptor was reported as: %s\n",
                reported.empty() ? "(no failure at all)" : reported.c_str());
        }
        require(reported.find("file table is full") != std::string_view::npos);

        // Still at the ceiling: the bytes survive the refusal and are read
        // without the descriptors the host could not install. This is the
        // discriminator, because a message that carries none never fails.
        VhostUserMessage stripped;
        require(receiveVhostUserMessage(
            sockets.descriptors[1].get(), stripped));
        require(stripped.header.request == header.request);
        require(stripped.descriptors.empty());

        releaseDescriptors(held);

        VhostUserMessage plain;
        require(receiveVhostUserMessage(sockets.descriptors[1].get(), plain));
        require(plain.header.request == header.request);
        require(plain.descriptors.empty());
    }
    require(descriptorCount() == before);
}

} // namespace

} // namespace uml::gpu::testing

int main()
{
    using namespace uml::gpu::testing;

    raisingTakesEverythingTheHostOffers();
    aFullTableNamesItself();
    std::puts("GPU transport descriptor ceiling tests passed");
    return 0;
}

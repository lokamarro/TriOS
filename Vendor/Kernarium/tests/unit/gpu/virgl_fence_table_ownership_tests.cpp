// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Virglrenderer keeps every registered fence fd in one process-global table,
 * and this address space has two owners of it. The VMM side takes it in
 * virgl_renderer_init() and holds it for the life of the renderer. The
 * same-process render server takes it for as long as it owns contexts, and
 * losing its last context is ordinary: a Venus context whose host device is
 * lost ends its worker. That must not take the table away from the VMM side,
 * whose next fence lookup would otherwise search a NULL table and fault.
 *
 * The installed renderer header does not expose that ownership, so this
 * regression exercises the dependency's own fence entry points directly.
 * Retirement has no entry point of its own: registering a fence sweeps the
 * table first and drops every entry whose sync file has become readable,
 * which is how the renderer retires fences in production.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

extern "C" {
int virgl_fence_table_init();
void virgl_fence_table_cleanup();
int virgl_fence_set_fd(std::uint64_t fenceId, int fd);
int virgl_fence_get_fd(std::uint64_t fenceId);
}

namespace {

int failureCount = 0;

void check(bool condition, const char* description)
{
    if (condition)
        return;
    std::fprintf(stderr, "Virgl fence table ownership: %s\n", description);
    ++failureCount;
}

/*
 * A pipe stands in for a sync file: the table only ever polls the descriptor
 * for readability. A pipe with nothing written to it never becomes readable,
 * so the sweep keeps the entry, which is how a fence behaves while its work
 * is still outstanding. Completing one makes the read end readable.
 */
class Fence {
public:
    Fence()
    {
        if (::pipe(descriptors_) != 0) {
            std::perror("pipe");
            std::exit(EXIT_FAILURE);
        }
    }

    ~Fence()
    {
        ::close(descriptors_[0]);
        ::close(descriptors_[1]);
    }

    Fence(const Fence&) = delete;
    Fence& operator=(const Fence&) = delete;

    [[nodiscard]] int descriptor() const noexcept
    {
        return descriptors_[0];
    }

    void complete() const
    {
        if (::write(descriptors_[1], "", 1) != 1) {
            std::perror("write");
            std::exit(EXIT_FAILURE);
        }
    }

private:
    int descriptors_[2] {};
};

// Close a descriptor the table handed back, reporting when it withheld one.
void expectHandedBack(int descriptor, const char* description)
{
    check(descriptor >= 0, description);
    if (descriptor >= 0)
        ::close(descriptor);
}

constexpr std::uint64_t completingFenceId = 66198;
constexpr std::uint64_t outstandingFenceId = 66199;
constexpr std::uint64_t sweepingFenceId = 66200;

} // namespace

int main()
{
    Fence completing;
    Fence outstanding;
    Fence sweeping;

    // The VMM side takes the table and registers its pending fences.
    check(virgl_fence_table_init() == 0,
        "the renderer owner could not take the fence table");
    check(virgl_fence_set_fd(completingFenceId, completing.descriptor()) == 0,
        "the renderer owner could not register a pending fence");
    check(virgl_fence_set_fd(outstandingFenceId, outstanding.descriptor()) == 0,
        "the renderer owner could not register a second pending fence");

    // A same-process render-server context takes its own reference, then
    // gives it back when that context ends.
    check(virgl_fence_table_init() == 0,
        "the render server could not take the fence table");
    virgl_fence_table_cleanup();

    // Searching the table is the first thing the renderer does with a fence
    // the render server is no longer involved in, and it is the lookup that
    // faulted on a table the render server had destroyed.
    expectHandedBack(virgl_fence_get_fd(outstandingFenceId),
        "the renderer owner's fence did not survive the render server");

    // Retirement is the sweep the table runs as each new fence arrives.
    // Completing one fence and registering the next must drop exactly the
    // completed entry, and must find the surviving table to do it.
    completing.complete();
    check(virgl_fence_set_fd(sweepingFenceId, sweeping.descriptor()) == 0,
        "the renderer owner could not register a fence after the render server");
    check(virgl_fence_get_fd(completingFenceId) < 0,
        "a completed fence stayed in the table");
    expectHandedBack(virgl_fence_get_fd(outstandingFenceId),
        "the sweep retired a fence that had not completed");

    // The last owner releases the table.
    virgl_fence_table_cleanup();

    // A later owner gets a usable table again, with nothing left behind.
    check(virgl_fence_table_init() == 0,
        "the fence table could not be taken again");
    check(virgl_fence_get_fd(outstandingFenceId) < 0,
        "a released fence table kept an entry");
    check(virgl_fence_set_fd(outstandingFenceId, outstanding.descriptor()) == 0,
        "a re-created fence table rejected a fence");
    virgl_fence_table_cleanup();

    if (failureCount != 0)
        return EXIT_FAILURE;

    std::puts("Virgl fence table ownership tests passed");
    return 0;
}

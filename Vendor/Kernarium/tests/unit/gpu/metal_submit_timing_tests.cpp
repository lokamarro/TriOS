// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_submit_timing.hpp"

#include <functional>
#include <utility>

namespace {
using namespace uml::gpu::test;

constexpr const char *record =
    "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=1.125 error=0";

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void rejects(const std::function<void()> &operation)
{
    try {
        operation();
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("invalid submit timing was accepted");
}

// Isolate this executable's stderr so forwarding can be checked byte-for-byte.
struct StderrFile {
    FILE *file = std::tmpfile();
    int saved = dup(STDERR_FILENO);
    StderrFile()
    {
        require(file && saved >= 0, "stderr fixture creation failed");
        require(dup2(fileno(file), STDERR_FILENO) >= 0, "stderr fixture redirect failed");
    }
    ~StderrFile()
    {
        std::fflush(stderr);
        if (saved >= 0) {
            dup2(saved, STDERR_FILENO);
            close(saved);
        }
        if (file)
            std::fclose(file);
    }
};
} // namespace

int main()
{
    try {
        SubmitTimingLedger valid;
        valid.ingest("ordinary stderr remains ordinary");
        valid.begin();
        valid.ingest(record);
        require(valid.complete(0.2).end_seconds == 1.125, "raw interval changed");
        valid.begin();
        valid.ingest("KK_SUBMIT_TIMING device=0x123 gpu_start_s=2 gpu_end_s=2.25 error=0");
        valid.complete(0.3);
        valid.finish();
        require(valid.matched() == 2, "incorrect matched callback count");

        for (const char *invalid : {
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=0 gpu_end_s=1 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=0 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=1 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=nan gpu_end_s=2 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=inf error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=7.68e20 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=1.5 error=1",
                 "KK_SUBMIT_TIMING device=0x0 gpu_start_s=1 gpu_end_s=1.5 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=bad gpu_end_s=2 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=1.1 error=0 junk",
                 "KK_SUBMIT_TIMING truncated"}) {
            rejects([&] {
                SubmitTimingLedger ledger;
                ledger.begin();
                ledger.ingest(invalid);
                ledger.complete(0.2);
            });
        }
        rejects([] { SubmitTimingLedger ledger; ledger.ingest(record); });
        rejects([] { SubmitTimingLedger ledger; ledger.begin(); ledger.finish(); });
        rejects([] {
            SubmitTimingLedger ledger;
            ledger.begin(); ledger.ingest(record); ledger.ingest(record);
        });
        rejects([] {
            SubmitTimingLedger ledger;
            ledger.begin(); ledger.ingest(record); ledger.complete(0.2);
            ledger.ingest(record); // Late callback after the last draw.
        });
        for (const char *invalid_next : {
                 "KK_SUBMIT_TIMING device=0x456 gpu_start_s=2 gpu_end_s=2.1 error=0",
                 "KK_SUBMIT_TIMING device=0x123 gpu_start_s=1 gpu_end_s=1.125 error=0"}) {
            rejects([&] {
                SubmitTimingLedger ledger;
                ledger.begin(); ledger.ingest(record); ledger.complete(0.2);
                ledger.begin(); ledger.ingest(invalid_next); ledger.complete(0.2);
            });
        }

        {
            StderrFile sink;
            SubmitTimingCollector collector;
            collector.begin();
            const std::string text = std::string("ordinary stderr\n") + record + "\n";
            // Split the diagnostic across writes to cover stream framing.
            std::fwrite(text.data(), 7, 1, stderr);
            std::fwrite(text.data() + 7, text.size() - 7, 1, stderr);
            std::fflush(stderr);
            collector.complete(0.2);
            require(collector.finish() == 1, "collector count mismatch");
            std::rewind(sink.file);
            std::array<char, 512> bytes{};
            const auto count = std::fread(bytes.data(), 1, bytes.size(), sink.file);
            require(std::string_view(bytes.data(), count) == text, "stderr was not preserved");
        }
        {
            StderrFile sink;
            SubmitTimingCollector collector;
            collector.begin();
            std::fprintf(stderr, "%s\n", record);
            collector.complete(0.2);
            std::fprintf(stderr, "%s\n", record);
            rejects([&] { collector.finish(); }); // Drain detects final late record.
        }
        {
            StderrFile sink;
            SubmitTimingCollector collector;
            collector.begin();
            rejects([&] { collector.complete(0.2, std::chrono::milliseconds(1)); });
        }
        {
            StderrFile sink;
            SubmitTimingCollector collector;
            collector.begin();
            std::fprintf(stderr, "%s", record);
            rejects([&] { collector.finish(); }); // EOF without a complete record.
        }
        std::puts("Metal submit timing collector PASS (no GPU required)");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Metal submit timing collector FAIL: %s\n", error.what());
        return 1;
    }
}

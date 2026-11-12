// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/hevc_stream.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

/* The HEVC header readers of the Apple video decoder, on the checked-in
 * streams and on slice headers built here. Nothing in this binary touches
 * VideoToolbox. */

namespace {
using namespace uml::platform::hevc;
using uml::testing::require;
using Unit = std::vector<std::uint8_t>;

std::string fixtureDirectory;

Unit read(const std::string& name)
{
    std::ifstream file(fixtureDirectory + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + fixtureDirectory + "/" + name);
    return {std::istreambuf_iterator<char>(file), {}};
}

/* One coded picture of a stream, in decode order: the header its first slice
 * segment carries. */
struct Picture {
    SliceStart slice;
    unsigned index;
};

struct Stream {
    ParameterSets sets;
    std::vector<Picture> pictures;
};

Stream open(const std::string& name)
{
    Stream stream;
    const auto bytes = read(name);
    const auto units = uml::platform::bitstream::splitAnnexB(bytes);
    std::vector<Unit> slices;
    for (const auto unit : units) {
        const auto type = nalType(unit);
        if (type == nalVps || type == nalSps || type == nalPps) {
            require(stream.sets.store(unit) != ParameterSets::Result::rejected, "a parameter set was rejected");
        } else if (isPicture(type)) {
            const auto slice = parseSliceStart(unit, stream.sets);
            require(slice.has_value(), "a slice header could not be read");
            if (slice->firstInPicture) stream.pictures.push_back({*slice, unsigned(stream.pictures.size())});
        }
    }
    return stream;
}

/* What the decoder does with the pictures from `first` on: the order they are
 * shown in, as the indices of the pictures. The parameter sets of the whole
 * stream are known, as they are to a decoder that has seen the start. */
constexpr unsigned never = ~0u;
std::vector<unsigned> shown(const Stream& stream, unsigned first = 0, unsigned endSequenceAt = never)
{
    PictureOrder order;
    ReorderBuffer buffer;
    std::vector<ReorderBuffer::Id> released;
    for (const auto& picture : stream.pictures) {
        if (picture.index < first) continue;
        if (picture.index == endSequenceAt) order.endSequence();
        const auto* sps = stream.sets.sps(picture.slice.spsId);
        require(sps != nullptr, "a picture's SPS is unknown");
        const auto verdict = order.next(picture.slice, *sps);
        if (verdict.startsSequence) buffer.flush(released);
        if (!verdict.decode) continue;
        if (verdict.output)
            buffer.add(picture.index, verdict.poc, sps->maxNumReorder, sps->maxLatencyIncreasePlus1, released);
    }
    buffer.flush(released);
    return {released.begin(), released.end()};
}

/* The display position of each picture of a fixture, which the encoder's
 * source frame number states: both streams show source frame N as the Nth
 * picture. The decode order is what the hardware decoder returned for the
 * stream, and each picture's luma is 16 + 10 * N. */
const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 10, 9, 12, 16, 14, 13, 15, 19, 18, 17};
const unsigned openDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 12, 10, 9, 11, 16, 14, 13, 15,
                                20, 18, 17, 19, 23, 22, 21};

template<std::size_t N> std::vector<unsigned> expected(const unsigned (&display)[N], unsigned first = 0,
                                                       std::vector<unsigned> dropped = {})
{
    std::vector<unsigned> order;
    for (unsigned i = first; i < N; ++i)
        if (std::find(dropped.begin(), dropped.end(), i) == dropped.end()) order.push_back(i);
    std::sort(order.begin(), order.end(), [&display](unsigned a, unsigned b) { return display[a] < display[b]; });
    return order;
}

void print(const char* label, const std::vector<unsigned>& order)
{
    std::printf("%s:", label);
    for (const auto index : order) std::printf(" %u", index);
    std::printf("\n");
}

void parameterSets()
{
    const auto stream = open("hevc-bframes.bin");
    require(stream.sets.complete(), "the fixture has a VPS, an SPS and a PPS");
    const auto* sps = stream.sets.sps(0);
    require(sps != nullptr && stream.sets.pps(0) != nullptr, "parameter sets by identifier");
    std::printf("hevc-bframes: %ux%u, %u-bit, POC lsb bits %u, reorder %u, latency+1 %u\n", sps->width,
                sps->height, sps->bitDepth, sps->log2MaxPocLsb, sps->maxNumReorder, sps->maxLatencyIncreasePlus1);
    require(sps->width == 64 && sps->height == 64 && sps->bitDepth == 8 && !sps->hasWindow, "SPS picture format");
    require(sps->profile == 1, "Main profile");
    require(sps->maxNumReorder == 2, "a hierarchical B stream reorders two pictures");
    require(stream.pictures.size() == 20, "twenty pictures");
    require(stream.pictures[0].slice.type == nalIdrNLp && stream.pictures[12].slice.type == nalIdrNLp,
            "two IDR pictures");

    // The same parameter set again changes nothing; another one does.
    ParameterSets sets;
    const auto bytes = read("hevc-bframes.bin");
    const auto units = uml::platform::bitstream::splitAnnexB(bytes);
    require(sets.store(units[0]) == ParameterSets::Result::changed, "a new VPS");
    require(sets.store(units[0]) == ParameterSets::Result::unchanged, "the same VPS again");
    require(!sets.complete(), "a VPS alone is not enough for a description");
    require(sets.store(units[1]) == ParameterSets::Result::changed, "a new SPS");
    require(sets.store(units[2]) == ParameterSets::Result::changed, "a new PPS");
    require(sets.complete(), "all three");
    require(sets.store(units[3]) == ParameterSets::Result::rejected, "a slice is not a parameter set");
    require(sets.descriptionUnits().size() == 3, "three units describe the stream");
    // A unit of another layer is not the base layer's.
    Unit enhancement(units[1].begin(), units[1].end());
    enhancement[0] = std::uint8_t(enhancement[0] | 1);
    enhancement[1] = std::uint8_t((enhancement[1] & 7) | 0x08);
    require(sets.store(enhancement) == ParameterSets::Result::rejected, "a unit of layer 1 is ignored");
}

void conformanceWindow()
{
    const auto stream = open("hevc-window.bin");
    const auto* sps = stream.sets.sps(0);
    require(sps != nullptr && sps->hasWindow, "the fixture states a conformance window");
    std::printf("hevc-window: coded %ux%u, window left %u right %u top %u bottom %u, shown %ux%u\n", sps->width,
                sps->height, sps->window.left, sps->window.right, sps->window.top, sps->window.bottom,
                sps->visibleWidth(), sps->visibleHeight());
    require(sps->width == 128 && sps->height == 96, "coded size");
    require(sps->window.left == 6 && sps->window.right == 2 && sps->window.top == 4 && sps->window.bottom == 2,
            "window offsets in luma samples");
    require(sps->visibleWidth() == 120 && sps->visibleHeight() == 90, "shown size");

    const auto bytes = read("hevc-window.bin");
    const auto units = uml::platform::bitstream::splitAnnexB(bytes);
    bool seen = false;
    for (const auto unit : units) {
        if (nalType(unit) != nalSps) continue;
        seen = true;
        const auto stripped = withoutWindow(unit);
        require(stripped.has_value(), "the SPS can be rewritten");
        const auto after = parseSps(*stripped);
        require(after.has_value(), "the rewritten SPS can be read");
        require(!after->hasWindow && after->window.left == 0 && after->window.bottom == 0, "no window remains");
        require(after->width == sps->width && after->height == sps->height && after->bitDepth == sps->bitDepth &&
                after->log2MaxPocLsb == sps->log2MaxPocLsb && after->maxNumReorder == sps->maxNumReorder &&
                after->profile == sps->profile && after->id == sps->id,
                "everything but the window is kept");
        require(!stripped->empty() && stripped->back() != 0, "the unit still ends in its stop bit");
        require(stripped->size() <= unit.size(), "removing a window does not grow the unit");
        // Nothing was lost between the window and the end: the bits after it
        // are the same ones, shifted.
        const auto before = uml::platform::bitstream::rbsp(unit, 2);
        const auto rebuilt = uml::platform::bitstream::rbsp(*stripped, 2);
        std::size_t differing = 0;
        for (std::size_t bit = sps->windowEndBit; bit + 1 < before.size() * 8; ++bit) {
            const std::size_t shifted = bit - (sps->windowEndBit - sps->windowFlagBit - 1);
            if (shifted / 8 >= rebuilt.size()) break;
            const bool a = (before[bit / 8] >> (7 - bit % 8)) & 1;
            const bool b = (rebuilt[shifted / 8] >> (7 - shifted % 8)) & 1;
            // The tail of both units is the stop bit and its padding, which
            // differ in length, so only compare while the old unit has data.
            if (a != b && bit + 8 < before.size() * 8) ++differing;
        }
        require(differing == 0, "the syntax after the window is unchanged");
        require(withoutWindow(*stripped).value() == *stripped, "rewriting twice changes nothing");
    }
    require(seen, "the fixture has an SPS");
    // A stream with no window comes back as it was.
    const auto plainBytes = read("hevc-bframes.bin");
    const auto plain = uml::platform::bitstream::splitAnnexB(plainBytes);
    require(Unit(plain[1].begin(), plain[1].end()) == withoutWindow(plain[1]).value(), "no window, no change");
    // And the description a decoder builds has the rewritten unit.
    const auto described = stream.sets.descriptionUnits();
    require(described.size() == 3 && !parseSps(described[1])->hasWindow, "the description has no window");
}

void pictureOrder()
{
    const auto closed = open("hevc-bframes.bin");
    const auto order = shown(closed);
    print("closed GOP shown order", order);
    require(order == expected(closedDisplay), "closed GOP pictures leave in display order");

    // Decoding from a CRA picture drops the RASL pictures that follow it:
    // their references precede the start.
    const auto open_ = open("hevc-open-gop.bin");
    require(open_.pictures[5].slice.type == nalCra && open_.pictures[6].slice.type == nalRaslR &&
            open_.pictures[7].slice.type == nalRaslN, "the fixture has a CRA picture with leading pictures");
    const auto all = shown(open_);
    print("open GOP shown order", all);
    require(all == expected(openDisplay), "a CRA mid-stream keeps its leading pictures, in display order");
    const auto cut = shown(open_, 5);
    print("open GOP from the first CRA", cut);
    require(cut == expected(openDisplay, 5, {6, 7, 8}), "starting at a CRA drops its RASL pictures");
    // An end of sequence unit makes the next CRA picture one that starts a
    // sequence, so the leading pictures after it are dropped as well.
    const auto afterEnd = shown(open_, 0, 13);
    print("open GOP with an end of sequence before the second CRA", afterEnd);
    require(afterEnd == expected(openDisplay, 0, {14, 15, 16}), "a CRA after an end of sequence drops its RASL pictures");
}

SliceStart slice(unsigned type, unsigned lsb, unsigned temporalId = 0, bool output = true)
{
    SliceStart start;
    start.type = type;
    start.temporalId = temporalId;
    start.firstInPicture = true;
    start.pocLsb = lsb;
    start.output = output;
    return start;
}

void pictureOrderCount()
{
    Sps sps;
    sps.log2MaxPocLsb = 4;   // sixteen values, so the count wraps quickly
    PictureOrder order;
    const auto poc = [&](unsigned type, unsigned lsb, unsigned tid = 0) {
        return order.next(slice(type, lsb, tid), sps).poc;
    };
    require(poc(nalIdrNLp, 0) == 0, "an IDR picture is zero");
    require(poc(1, 6) == 6 && poc(1, 12) == 12, "counting up");
    require(poc(1, 14) == 14, "within half the range of the last reference");
    require(poc(1, 2) == 18, "the count wraps into the next cycle");
    // A picture that precedes the last one in output order goes back a cycle.
    PictureOrder backwards;
    require(backwards.next(slice(nalIdrNLp, 0), sps).poc == 0, "start");
    require(backwards.next(slice(1, 8), sps).poc == 8, "P picture");
    require(backwards.next(slice(1, 0, 0), sps).poc == 16, "next cycle");
    require(backwards.next(slice(0, 15, 0), sps).poc == 15, "a B picture shown before it, in the previous cycle");
    // A sub-layer non-reference picture is not the reference for the next
    // count, so this one is still counted from the picture at 16.
    require(backwards.next(slice(1, 4, 0), sps).poc == 20, "counted from the picture at 16");
    // A picture of a higher temporal layer is not the reference either.
    PictureOrder layers;
    require(layers.next(slice(nalIdrNLp, 0), sps).poc == 0, "start");
    require(layers.next(slice(1, 7, 1), sps).poc == 7, "sub-layer 1");
    require(layers.next(slice(1, 14, 1), sps).poc == -2, "sub-layer 1 again, still counted from the picture at 0");
    require(layers.next(slice(1, 1, 0), sps).poc == 1, "a lower-layer picture follows the IDR in output order");

    // Pictures nobody shows.
    PictureOrder flags;
    require(!flags.next(slice(nalIdrNLp, 0, 0, false), sps).output, "pic_output_flag = 0");
    // BLA pictures start a sequence like IDR pictures do and drop RASL pictures.
    PictureOrder bla;
    require(bla.next(slice(1, 3), sps).decode, "a stream that begins mid-sequence decodes what it is given");
    const auto start = bla.next(slice(nalBlaWLp, 9), sps);
    require(start.startsSequence && start.poc == 9, "a BLA picture starts a sequence, with its own count");
    require(!bla.next(slice(nalRaslR, 5), sps).decode, "RASL after a BLA is dropped");
    require(bla.next(slice(7, 6), sps).decode, "RADL is decoded");
    // A CRA that is not first, and not after an end of sequence, keeps its leading pictures.
    PictureOrder cra;
    (void)cra.next(slice(nalIdrNLp, 0), sps);
    const auto mid = cra.next(slice(nalCra, 6), sps);
    require(!mid.startsSequence && mid.poc == 6, "a CRA in the middle of a sequence");
    require(cra.next(slice(nalRaslN, 4), sps).decode, "its RASL pictures decode");
    cra.endSequence();
    require(cra.next(slice(nalCra, 6), sps).startsSequence, "a CRA after an end of sequence starts one");
    require(!cra.next(slice(nalRaslN, 4), sps).decode, "and drops its RASL pictures");
    cra.reset();
    require(cra.next(slice(nalCra, 3), sps).startsSequence, "after a reset the next CRA is the first picture");
}

void reorderBuffer()
{
    std::vector<ReorderBuffer::Id> out;
    ReorderBuffer buffer;
    // Nothing reorders: each picture leaves as it arrives.
    buffer.add(1, 0, 0, 0, out);
    buffer.add(2, 1, 0, 0, out);
    require(out == std::vector<ReorderBuffer::Id>({1, 2}), "no reordering allowed, no delay");
    out.clear();
    // Two may wait: 0, then 4, 2, 1, 3 leaves 0 immediately, then 1 once 2 pictures are behind.
    buffer.add(10, 0, 2, 0, out);
    require(out.empty(), "the first picture waits for the next two");
    buffer.add(11, 4, 2, 0, out);
    require(out.empty(), "two pictures wait");
    buffer.add(12, 2, 2, 0, out);
    require(out == std::vector<ReorderBuffer::Id>({10}), "the third picture releases the lowest count");
    buffer.add(13, 1, 2, 0, out);
    require(out == std::vector<ReorderBuffer::Id>({10, 13}), "and the next the new lowest");
    buffer.add(14, 3, 2, 0, out);
    require(out == std::vector<ReorderBuffer::Id>({10, 13, 12}), "in output order");
    buffer.flush(out);
    require(out == std::vector<ReorderBuffer::Id>({10, 13, 12, 14, 11}), "a flush releases the rest in order");
    require(buffer.size() == 0, "nothing is left");

    // A latency limit: a picture overtaken by as many pictures as the stream
    // allows (reorder + plus1 - 1 = 2 here) leaves, with every picture that
    // precedes it, even though the reorder limit alone would hold it.
    out.clear();
    buffer.add(1, 100, 2, 1, out);
    buffer.add(2, 99, 2, 1, out);
    require(out.empty(), "overtaken once");
    buffer.add(3, 98, 2, 1, out);
    require(out == std::vector<ReorderBuffer::Id>({3, 2, 1}), "overtaken twice: it leaves, after those before it");
    // No latency limit when the stream states none.
    out.clear();
    buffer.add(1, 100, 2, 0, out);
    buffer.add(2, 99, 2, 0, out);
    buffer.add(3, 98, 2, 0, out);
    require(out == std::vector<ReorderBuffer::Id>({3}), "only the reorder limit applies");
    buffer.clear();
    require(buffer.size() == 0, "cleared");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_hevc_stream_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        parameterSets();
        conformanceWindow();
        pictureOrder();
        pictureOrderCount();
        reorderBuffer();
        std::printf("hevc stream tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

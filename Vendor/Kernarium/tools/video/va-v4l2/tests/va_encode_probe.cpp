// SPDX-License-Identifier: GPL-2.0-or-later
// A VA-API client that encodes the way Chromium's VaapiVideoEncodeAccelerator
// does: surfaces and coded buffers at the aligned picture size, the sequence,
// picture, slice and rate-control buffers submitted with every picture, a few
// pictures in flight before the oldest is synced and its coded buffer mapped.
// It writes the frames as an Annex B elementary stream and checks what it can
// without a decoder: that every frame came back, that key frames are where they
// were asked for and carry their parameter sets, and that the stream's own
// size is the size that was asked for, cropping included.
//
// It talks to libva, not to this driver, so it also runs against any other
// VA-API backend.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_str.h>

#include "annexb.hpp"

namespace {

using namespace annexb;

bool check(VAStatus status, const char* what)
{
    if (status == VA_STATUS_SUCCESS) return true;
    std::fprintf(stderr, "%s: %s\n", what, vaErrorStr(status));
    return false;
}

struct Options {
    bool hevc {false};
    const char* profile {"main"};
    const char* rate {"cbr"};
    unsigned width {640};
    unsigned height {360};
    unsigned frames {30};
    unsigned fps {30};
    unsigned bitrate {2000000};
    // Distance between the key frames the client asks for, 0 for the first only.
    unsigned idrPeriod {0};
    // A bit rate the stream moves to halfway through, 0 for none.
    unsigned bitrateStep {0};
    unsigned depth {4};
    unsigned sessions {1};
    unsigned contexts {1};
    unsigned qp {26};
    const char* out {nullptr};
    const char* input {nullptr};
    const char* node {nullptr};
    bool list {false};
};

unsigned align(unsigned value, unsigned to) noexcept { return (value + to - 1) / to * to; }

// ------------------------------------------------------------------- pictures

// A moving gradient with a box on it, as NV12, so that the output of an
// encoder is something a person can look at.
void paint(std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma, unsigned width, unsigned height,
           unsigned frame)
{
    luma.assign(std::size_t(width) * height, 0);
    chroma.assign(std::size_t(width) * ((height + 1) / 2), 128);
    const unsigned boxSize = std::max(16u, std::min(width, height) / 6);
    const unsigned boxX = (frame * 7) % std::max(1u, width - boxSize);
    const unsigned boxY = (frame * 3) % std::max(1u, height - boxSize);
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) {
            std::uint8_t value = std::uint8_t((column * 255 / std::max(1u, width - 1) + row / 4 + frame) & 0xff);
            if (column >= boxX && column < boxX + boxSize && row >= boxY && row < boxY + boxSize) value = 235;
            luma[std::size_t(row) * width + column] = value;
        }
    for (unsigned row = 0; row < height / 2; ++row)
        for (unsigned column = 0; column < width / 2; ++column) {
            const bool inside = 2 * column >= boxX && 2 * column < boxX + boxSize && 2 * row >= boxY &&
                                2 * row < boxY + boxSize;
            chroma[std::size_t(row) * width + 2 * column] = inside ? 90 : std::uint8_t(96 + (frame + column) % 64);
            chroma[std::size_t(row) * width + 2 * column + 1] = inside ? 240 : std::uint8_t(160 - (frame + row) % 64);
        }
}

class FileSource {
public:
    bool open(const char* path, unsigned width, unsigned height)
    {
        file_ = std::fopen(path, "rb");
        width_ = width;
        height_ = height;
        return file_ != nullptr;
    }
    ~FileSource() { if (file_) std::fclose(file_); }
    // One I420 frame, converted to NV12. Wraps to the start at the end of the file.
    bool read(std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma)
    {
        const std::size_t lumaSize = std::size_t(width_) * height_;
        const std::size_t quarter = std::size_t((width_ + 1) / 2) * ((height_ + 1) / 2);
        std::vector<std::uint8_t> raw(lumaSize + 2 * quarter);
        if (std::fread(raw.data(), 1, raw.size(), file_) != raw.size()) {
            std::rewind(file_);
            if (std::fread(raw.data(), 1, raw.size(), file_) != raw.size()) return false;
        }
        luma.assign(raw.begin(), raw.begin() + std::ptrdiff_t(lumaSize));
        chroma.assign(std::size_t(width_) * ((height_ + 1) / 2), 128);
        for (unsigned row = 0; row < (height_ + 1) / 2; ++row)
            for (unsigned column = 0; column < (width_ + 1) / 2; ++column) {
                chroma[std::size_t(row) * width_ + 2 * column] = raw[lumaSize + std::size_t(row) * ((width_ + 1) / 2) + column];
                chroma[std::size_t(row) * width_ + 2 * column + 1] =
                    raw[lumaSize + quarter + std::size_t(row) * ((width_ + 1) / 2) + column];
            }
        return true;
    }

private:
    std::FILE* file_ {};
    unsigned width_ {};
    unsigned height_ {};
};

// ------------------------------------------------------------------- encoding

unsigned h264Level(unsigned width, unsigned height, unsigned fps) noexcept
{
    const std::uint64_t rate = std::uint64_t(align(width, 16) / 16) * (align(height, 16) / 16) * fps;
    if (rate <= 108000) return 31;
    if (rate <= 245760) return 41;
    if (rate <= 589824) return 42;
    return 51;
}

unsigned hevcLevel(unsigned width, unsigned height, unsigned fps) noexcept
{
    const std::uint64_t rate = std::uint64_t(width) * height * fps;
    if (rate <= 33177600) return 93;
    if (rate <= 66846720) return 120;
    if (rate <= 133693440) return 123;
    return 153;
}

struct Result {
    unsigned frames {};
    unsigned keyFrames {};
    unsigned unexpectedKeys {};
    unsigned missingKeys {};
    unsigned empty {};
    unsigned overflow {};
    std::uint64_t bytes {};
    double seconds {};
    StreamInfo stream;
    bool headersOnKeys {true};
    std::vector<std::uint8_t> bitstream;
};

class Encoder {
public:
    Encoder(VADisplay display, const Options& options, VAProfile profile, VAConfigID config)
        : display_(display), options_(options), profile_(profile), config_(config)
    {
    }

    bool open()
    {
        codedWidth_ = options_.hevc ? align(options_.width, 8) : align(options_.width, 16);
        codedHeight_ = options_.hevc ? align(options_.height, 8) : align(options_.height, 16);
        if (options_.hevc && (codedWidth_ != options_.width || codedHeight_ != options_.height)) {
            // HEVC sequence parameters have no cropping to say which part is picture.
            std::fprintf(stderr, "HEVC sizes must be multiples of 8: the stream is the coded size\n");
            return false;
        }
        const unsigned count = options_.depth + 1;
        surfaces_.resize(count);
        VASurfaceAttrib attribute {};
        attribute.type = VASurfaceAttribPixelFormat;
        attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = VA_FOURCC_NV12;
        if (!check(vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420, codedWidth_, codedHeight_, surfaces_.data(), count,
                                    &attribute, 1),
                   "vaCreateSurfaces"))
            return false;
        if (!check(vaCreateContext(display_, config_, int(codedWidth_), int(codedHeight_), VA_PROGRESSIVE,
                                   surfaces_.data(), count, &context_),
                   "vaCreateContext"))
            return false;
        coded_.resize(count);
        const unsigned codedSize = std::max(1u << 16, codedWidth_ * codedHeight_ * 3 / 2);
        for (auto& buffer : coded_)
            if (!check(vaCreateBuffer(display_, context_, VAEncCodedBufferType, codedSize, 1, nullptr, &buffer),
                       "vaCreateBuffer coded"))
                return false;
        return true;
    }

    ~Encoder()
    {
        if (context_ != VA_INVALID_ID) vaDestroyContext(display_, context_);
        for (const auto buffer : coded_) vaDestroyBuffer(display_, buffer);
        if (!surfaces_.empty()) vaDestroySurfaces(display_, surfaces_.data(), int(surfaces_.size()));
    }

    // Uploads and submits one picture, and collects the oldest when the
    // pipeline is full.
    bool encode(unsigned index, const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& chroma,
                bool key, Result& result)
    {
        const unsigned slot = index % surfaces_.size();
        if (!upload(surfaces_[slot], luma, chroma)) return false;
        if (!submit(index, slot, key)) return false;
        queue_.push_back({index, slot, key});
        if (queue_.size() > options_.depth) return collectOne(result);
        return true;
    }

    bool drain(Result& result)
    {
        while (!queue_.empty())
            if (!collectOne(result)) return false;
        return true;
    }

private:
    struct InFlight {
        unsigned index;
        unsigned slot;
        bool key;
    };

    bool upload(VASurfaceID surface, const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& chroma)
    {
        VAImage image {};
        if (!check(vaDeriveImage(display_, surface, &image), "vaDeriveImage")) return false;
        void* mapped = nullptr;
        bool good = check(vaMapBuffer(display_, image.buf, &mapped), "vaMapBuffer image");
        if (good) {
            auto* base = static_cast<std::uint8_t*>(mapped);
            // The picture is the top-left of the surface, which is a little larger
            // when the picture size is not a multiple of the alignment.
            for (unsigned row = 0; row < codedHeight_; ++row) {
                const unsigned source = std::min(row, options_.height - 1);
                std::uint8_t* destination = base + image.offsets[0] + std::size_t(row) * image.pitches[0];
                std::memcpy(destination, luma.data() + std::size_t(source) * options_.width, options_.width);
                if (codedWidth_ > options_.width)
                    std::memset(destination + options_.width, destination[options_.width - 1],
                                codedWidth_ - options_.width);
            }
            for (unsigned row = 0; row < codedHeight_ / 2; ++row) {
                const unsigned source = std::min(row, (options_.height + 1) / 2 - 1);
                std::uint8_t* destination = base + image.offsets[1] + std::size_t(row) * image.pitches[1];
                std::memcpy(destination, chroma.data() + std::size_t(source) * options_.width, options_.width);
                for (unsigned column = options_.width; column < codedWidth_; column += 2) {
                    destination[column] = destination[options_.width - 2];
                    destination[column + 1] = destination[options_.width - 1];
                }
            }
            vaUnmapBuffer(display_, image.buf);
        }
        vaDestroyImage(display_, image.image_id);
        return good;
    }

    VABufferID make(VABufferType type, const void* data, std::size_t size)
    {
        VABufferID id = VA_INVALID_ID;
        if (!check(vaCreateBuffer(display_, context_, type, unsigned(size), 1, const_cast<void*>(data), &id),
                   "vaCreateBuffer"))
            return VA_INVALID_ID;
        return id;
    }

    VABufferID misc(VAEncMiscParameterType type, const void* payload, std::size_t size)
    {
        std::vector<std::uint8_t> bytes(sizeof(VAEncMiscParameterBuffer) + size);
        std::memcpy(bytes.data(), &type, sizeof(type));
        std::memcpy(bytes.data() + sizeof(VAEncMiscParameterBuffer), payload, size);
        return make(VAEncMiscParameterBufferType, bytes.data(), bytes.size());
    }

    bool submit(unsigned index, unsigned slot, bool key)
    {
        std::vector<VABufferID> ids;
        const unsigned bitrate = options_.bitrateStep && index >= options_.frames / 2 ? options_.bitrateStep
                                                                                       : options_.bitrate;
        const unsigned idr = options_.idrPeriod ? options_.idrPeriod : 2048;
        const bool cqp = std::strcmp(options_.rate, "cqp") == 0;
        const bool icq = std::strcmp(options_.rate, "icq") == 0;
        const bool vbr = std::strcmp(options_.rate, "vbr") == 0;
        if (options_.hevc) {
            VAEncSequenceParameterBufferHEVC seq {};
            seq.general_profile_idc = 1;
            seq.general_level_idc = std::uint8_t(hevcLevel(options_.width, options_.height, options_.fps));
            seq.intra_period = idr;
            seq.intra_idr_period = idr;
            seq.ip_period = 1;
            seq.bits_per_second = bitrate;
            seq.pic_width_in_luma_samples = std::uint16_t(codedWidth_);
            seq.pic_height_in_luma_samples = std::uint16_t(codedHeight_);
            seq.seq_fields.bits.chroma_format_idc = 1;
            seq.vui_parameters_present_flag = 1;
            seq.vui_fields.bits.vui_timing_info_present_flag = 1;
            seq.vui_num_units_in_tick = 1;
            seq.vui_time_scale = options_.fps;
            ids.push_back(make(VAEncSequenceParameterBufferType, &seq, sizeof(seq)));
            VAEncPictureParameterBufferHEVC pic {};
            pic.decoded_curr_pic.picture_id = surfaces_[slot];
            pic.coded_buf = coded_[slot];
            pic.pic_init_qp = std::uint8_t(options_.qp);
            pic.pic_fields.bits.idr_pic_flag = key;
            pic.pic_fields.bits.reference_pic_flag = 1;
            ids.push_back(make(VAEncPictureParameterBufferType, &pic, sizeof(pic)));
            VAEncSliceParameterBufferHEVC slice {};
            slice.slice_type = key ? 2 : 1;
            slice.slice_fields.bits.last_slice_of_pic_flag = 1;
            ids.push_back(make(VAEncSliceParameterBufferType, &slice, sizeof(slice)));
        } else {
            VAEncSequenceParameterBufferH264 seq {};
            seq.level_idc = std::uint8_t(h264Level(options_.width, options_.height, options_.fps));
            seq.intra_period = idr;
            seq.intra_idr_period = idr;
            seq.ip_period = 1;
            seq.bits_per_second = bitrate;
            seq.max_num_ref_frames = 1;
            seq.picture_width_in_mbs = std::uint16_t(codedWidth_ / 16);
            seq.picture_height_in_mbs = std::uint16_t(codedHeight_ / 16);
            seq.seq_fields.bits.chroma_format_idc = 1;
            seq.seq_fields.bits.frame_mbs_only_flag = 1;
            seq.seq_fields.bits.direct_8x8_inference_flag = 1;
            if (codedWidth_ != options_.width || codedHeight_ != options_.height) {
                seq.frame_cropping_flag = 1;
                seq.frame_crop_right_offset = (codedWidth_ - options_.width) / 2;
                seq.frame_crop_bottom_offset = (codedHeight_ - options_.height) / 2;
            }
            seq.vui_parameters_present_flag = 1;
            seq.vui_fields.bits.timing_info_present_flag = 1;
            seq.num_units_in_tick = 1;
            seq.time_scale = 2 * options_.fps;
            ids.push_back(make(VAEncSequenceParameterBufferType, &seq, sizeof(seq)));
            VAEncPictureParameterBufferH264 pic {};
            pic.CurrPic.picture_id = surfaces_[slot];
            pic.coded_buf = coded_[slot];
            pic.pic_init_qp = std::uint8_t(options_.qp);
            pic.pic_fields.bits.idr_pic_flag = key;
            pic.pic_fields.bits.reference_pic_flag = 1;
            pic.pic_fields.bits.entropy_coding_mode_flag = std::strcmp(options_.profile, "baseline") != 0;
            pic.pic_fields.bits.transform_8x8_mode_flag = std::strcmp(options_.profile, "high") == 0;
            ids.push_back(make(VAEncPictureParameterBufferType, &pic, sizeof(pic)));
            VAEncSliceParameterBufferH264 slice {};
            slice.slice_type = key ? 2 : 0;
            ids.push_back(make(VAEncSliceParameterBufferType, &slice, sizeof(slice)));
        }
        VAEncMiscParameterRateControl rate {};
        rate.bits_per_second = bitrate;
        rate.target_percentage = vbr ? 70 : 100;
        rate.window_size = 1500;
        rate.initial_qp = options_.qp;
        rate.min_qp = 10;
        rate.max_qp = 51;
        rate.rc_flags.bits.disable_frame_skip = 1;
        if (icq) rate.ICQ_quality_factor = options_.qp;
        if (!cqp) ids.push_back(misc(VAEncMiscParameterTypeRateControl, &rate, sizeof(rate)));
        VAEncMiscParameterFrameRate framerate {};
        framerate.framerate = options_.fps;
        ids.push_back(misc(VAEncMiscParameterTypeFrameRate, &framerate, sizeof(framerate)));
        VAEncMiscParameterHRD hrd {};
        hrd.buffer_size = bitrate * 3 / 2;
        hrd.initial_buffer_fullness = hrd.buffer_size / 2;
        if (!cqp) ids.push_back(misc(VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd)));

        bool good = std::none_of(ids.begin(), ids.end(), [](VABufferID id) { return id == VA_INVALID_ID; });
        good = good && check(vaBeginPicture(display_, context_, surfaces_[slot]), "vaBeginPicture") &&
               check(vaRenderPicture(display_, context_, ids.data(), int(ids.size())), "vaRenderPicture") &&
               check(vaEndPicture(display_, context_), "vaEndPicture");
        for (const VABufferID id : ids)
            if (id != VA_INVALID_ID) vaDestroyBuffer(display_, id);
        return good;
    }

    bool collectOne(Result& result)
    {
        const InFlight entry = queue_.front();
        queue_.pop_front();
        if (!check(vaSyncSurface(display_, surfaces_[entry.slot]), "vaSyncSurface")) return false;
        void* mapped = nullptr;
        if (!check(vaMapBuffer(display_, coded_[entry.slot], &mapped), "vaMapBuffer coded")) return false;
        std::vector<std::uint8_t> frame;
        for (auto* segment = static_cast<VACodedBufferSegment*>(mapped); segment;
             segment = static_cast<VACodedBufferSegment*>(segment->next)) {
            const auto* bytes = static_cast<const std::uint8_t*>(segment->buf);
            frame.insert(frame.end(), bytes, bytes + segment->size);
            if (segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) ++result.overflow;
        }
        vaUnmapBuffer(display_, coded_[entry.slot]);
        if (frame.empty()) { ++result.empty; return true; }

        bool isKey = false, hasSps = false, hasPps = false, hasVps = !options_.hevc;
        for (const Nal& nal : splitNals(frame, options_.hevc)) {
            if (options_.hevc) {
                isKey = isKey || nal.type == 19 || nal.type == 20;
                hasVps = hasVps || nal.type == 32;
                if (nal.type == 33) {
                    hasSps = true;
                    if (!result.stream.found) result.stream = parseHevcSps(nal.payload, nal.size);
                }
                hasPps = hasPps || nal.type == 34;
            } else {
                isKey = isKey || nal.type == 5;
                if (nal.type == 7) {
                    hasSps = true;
                    if (!result.stream.found) result.stream = parseH264Sps(nal.payload, nal.size);
                }
                hasPps = hasPps || nal.type == 8;
            }
        }
        ++result.frames;
        result.bytes += frame.size();
        if (isKey) ++result.keyFrames;
        if (isKey && !entry.key) ++result.unexpectedKeys;
        if (!isKey && entry.key) ++result.missingKeys;
        if (isKey && !(hasSps && hasPps && hasVps)) result.headersOnKeys = false;
        result.bitstream.insert(result.bitstream.end(), frame.begin(), frame.end());
        return true;
    }

    VADisplay display_;
    Options options_;
    VAProfile profile_;
    VAConfigID config_;
    VAContextID context_ {VA_INVALID_ID};
    unsigned codedWidth_ {};
    unsigned codedHeight_ {};
    std::vector<VASurfaceID> surfaces_;
    std::vector<VABufferID> coded_;
    std::deque<InFlight> queue_;
};

VAProfile profileFor(const Options& options) noexcept
{
    if (options.hevc) return VAProfileHEVCMain;
    if (std::strcmp(options.profile, "baseline") == 0) return VAProfileH264ConstrainedBaseline;
    if (std::strcmp(options.profile, "high") == 0) return VAProfileH264High;
    return VAProfileH264Main;
}

unsigned rateFor(const Options& options) noexcept
{
    if (std::strcmp(options.rate, "vbr") == 0) return VA_RC_VBR;
    if (std::strcmp(options.rate, "cqp") == 0) return VA_RC_CQP;
    if (std::strcmp(options.rate, "icq") == 0) return VA_RC_ICQ;
    return VA_RC_CBR;
}

void listCapabilities(VADisplay display)
{
    std::vector<VAProfile> profiles(std::size_t(vaMaxNumProfiles(display)));
    int count = 0;
    if (!check(vaQueryConfigProfiles(display, profiles.data(), &count), "vaQueryConfigProfiles")) return;
    for (int index = 0; index < count; ++index) {
        std::vector<VAEntrypoint> entrypoints(std::size_t(vaMaxNumEntrypoints(display)));
        int entries = 0;
        if (vaQueryConfigEntrypoints(display, profiles[index], entrypoints.data(), &entries) != VA_STATUS_SUCCESS)
            continue;
        for (int entry = 0; entry < entries; ++entry) {
            if (entrypoints[entry] != VAEntrypointEncSlice && entrypoints[entry] != VAEntrypointEncSliceLP) continue;
            VAConfigAttrib attributes[] {{VAConfigAttribRateControl, 0},
                                         {VAConfigAttribEncPackedHeaders, 0},
                                         {VAConfigAttribEncMaxRefFrames, 0},
                                         {VAConfigAttribMaxPictureWidth, 0},
                                         {VAConfigAttribMaxPictureHeight, 0}};
            vaGetConfigAttributes(display, profiles[index], entrypoints[entry], attributes, 5);
            std::printf("%s %s rate=0x%x packed=0x%x refs=0x%x max=%ux%u\n", vaProfileStr(profiles[index]),
                        vaEntrypointStr(entrypoints[entry]), attributes[0].value, attributes[1].value,
                        attributes[2].value, attributes[3].value, attributes[4].value);
        }
    }
}

}

int main(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool more = index + 1 < argc;
        if (argument == "--codec" && more) options.hevc = std::strcmp(argv[++index], "hevc") == 0;
        else if (argument == "--profile" && more) options.profile = argv[++index];
        else if (argument == "--rate" && more) options.rate = argv[++index];
        else if (argument == "--size" && more) {
            if (std::sscanf(argv[++index], "%ux%u", &options.width, &options.height) != 2) options.width = 0;
        }
        else if (argument == "--frames" && more) options.frames = unsigned(std::atoi(argv[++index]));
        else if (argument == "--fps" && more) options.fps = unsigned(std::atoi(argv[++index]));
        else if (argument == "--bitrate" && more) options.bitrate = unsigned(std::atoi(argv[++index]));
        else if (argument == "--bitrate-step" && more) options.bitrateStep = unsigned(std::atoi(argv[++index]));
        else if (argument == "--idr-period" && more) options.idrPeriod = unsigned(std::atoi(argv[++index]));
        else if (argument == "--depth" && more) options.depth = unsigned(std::atoi(argv[++index]));
        else if (argument == "--sessions" && more) options.sessions = unsigned(std::atoi(argv[++index]));
        else if (argument == "--contexts" && more) options.contexts = unsigned(std::atoi(argv[++index]));
        else if (argument == "--qp" && more) options.qp = unsigned(std::atoi(argv[++index]));
        else if (argument == "--out" && more) options.out = argv[++index];
        else if (argument == "--in" && more) options.input = argv[++index];
        else if (argument == "--device" && more) options.node = argv[++index];
        else if (argument == "--list") options.list = true;
        else {
            std::fprintf(stderr,
                         "usage: va_encode_probe [--list] [--codec h264|hevc] [--profile baseline|main|high]\n"
                         "       [--rate cbr|vbr|cqp|icq] [--size WxH] [--frames N] [--fps N] [--bitrate BPS]\n"
                         "       [--bitrate-step BPS] [--idr-period N] [--depth N] [--sessions N] [--contexts N]\n"
                         "       [--qp N] [--in FILE.i420] [--out FILE|-] [--device NODE]\n");
            return 2;
        }
    }
    if (!options.width || !options.height || (options.width & 1) || (options.height & 1) || !options.frames ||
        !options.fps || !options.bitrate || !options.depth || !options.sessions || !options.contexts) {
        std::fprintf(stderr, "bad option value\n");
        return 2;
    }

    const char* candidates[] {options.node, "/dev/dri/renderD128", "/dev/dri/card0", "/dev/video0"};
    int drm = -1;
    VADisplay display = nullptr;
    int major = 0, minor = 0;
    for (const char* candidate : candidates) {
        if (!candidate || !*candidate) continue;
        drm = ::open(candidate, O_RDWR | O_CLOEXEC);
        if (drm < 0) continue;
        display = vaGetDisplayDRM(drm);
        if (!display) {
            std::fprintf(stderr, "%s: libva takes no display from it\n", candidate);
        } else {
            const VAStatus status = vaInitialize(display, &major, &minor);
            if (status == VA_STATUS_SUCCESS) {
                std::fprintf(stderr, "display %s\n", candidate);
                break;
            }
            std::fprintf(stderr, "%s: vaInitialize: %s\n", candidate, vaErrorStr(status));
        }
        if (display) vaTerminate(display);
        display = nullptr;
        ::close(drm);
        drm = -1;
    }
    if (!display) { std::fprintf(stderr, "no VA display\n"); return 2; }
    std::fprintf(stderr, "libva %d.%d vendor \"%s\"\n", major, minor, vaQueryVendorString(display));

    if (options.list) {
        listCapabilities(display);
        return 0;
    }

    const VAProfile profile = profileFor(options);
    VAConfigAttrib attributes[2] {{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
                                  {VAConfigAttribRateControl, rateFor(options)}};
    VAConfigAttrib offered[2] {{VAConfigAttribRTFormat, 0}, {VAConfigAttribRateControl, 0}};
    if (!check(vaGetConfigAttributes(display, profile, VAEntrypointEncSlice, offered, 2), "vaGetConfigAttributes"))
        return 1;
    if (!(offered[1].value & rateFor(options))) {
        std::fprintf(stderr, "the driver does not offer rate control %s (offers 0x%x)\n", options.rate,
                     offered[1].value);
        return 1;
    }
    VAConfigID config = VA_INVALID_ID;
    if (!check(vaCreateConfig(display, profile, VAEntrypointEncSlice, attributes, 2, &config), "vaCreateConfig"))
        return 1;

    FileSource source;
    if (options.input && !source.open(options.input, options.width, options.height)) {
        std::perror("open input");
        return 2;
    }
    std::FILE* output = nullptr;
    if (options.out) output = std::strcmp(options.out, "-") == 0 ? stdout : std::fopen(options.out, "wb");
    if (options.out && !output) { std::perror("open output"); return 2; }

    bool good = true;
    Result total;
    for (unsigned session = 0; session < options.sessions && good; ++session) {
        std::vector<std::unique_ptr<Encoder>> encoders;
        std::vector<Result> results(options.contexts);
        for (unsigned context = 0; context < options.contexts && good; ++context) {
            encoders.push_back(std::make_unique<Encoder>(display, options, profile, config));
            good = encoders.back()->open();
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> luma, chroma;
        for (unsigned frame = 0; frame < options.frames && good; ++frame) {
            if (options.input) {
                good = source.read(luma, chroma);
                if (!good) std::fprintf(stderr, "cannot read a frame of %s\n", options.input);
            } else {
                paint(luma, chroma, options.width, options.height, frame);
            }
            const bool key = frame == 0 || (options.idrPeriod && frame % options.idrPeriod == 0);
            for (unsigned context = 0; context < options.contexts && good; ++context)
                good = encoders[context]->encode(frame, luma, chroma, key, results[context]);
        }
        for (unsigned context = 0; context < options.contexts && good; ++context)
            good = encoders[context]->drain(results[context]);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (unsigned context = 0; context < options.contexts; ++context) {
            Result& result = results[context];
            result.seconds = seconds;
            const unsigned expectedKeys =
                options.idrPeriod ? (options.frames + options.idrPeriod - 1) / options.idrPeriod : 1;
            const bool sized = result.stream.found && result.stream.width == options.width &&
                               result.stream.height == options.height;
            const bool complete = good && result.frames == options.frames && !result.empty && !result.overflow;
            const bool keysRight = result.keyFrames == expectedKeys && !result.unexpectedKeys && !result.missingKeys &&
                                   result.headersOnKeys;
            std::printf("session %u context %u: frames=%u/%u bytes=%llu keys=%u/%u headers=%s stream=%ux%u (coded %ux%u)"
                        " profile=%u level=%u %.1f fps %s\n",
                        session, context, result.frames, options.frames, (unsigned long long)result.bytes,
                        result.keyFrames, expectedKeys, result.headersOnKeys ? "ok" : "MISSING",
                        result.stream.width, result.stream.height, result.stream.codedWidth, result.stream.codedHeight,
                        result.stream.profile, result.stream.level, seconds > 0 ? result.frames / seconds : 0.0,
                        complete && keysRight && sized ? "PASS" : "FAIL");
            good = good && complete && keysRight && sized;
            total.frames += result.frames;
            total.bytes += result.bytes;
            if (output && session == 0 && context == 0)
                std::fwrite(result.bitstream.data(), 1, result.bitstream.size(), output);
        }
    }
    if (output && output != stdout) std::fclose(output);
    std::printf("VA_ENCODE_PROBE: %s (%u frames, %llu bytes)\n", good ? "PASS" : "FAIL", total.frames,
                (unsigned long long)total.bytes);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    if (drm >= 0) ::close(drm);
    return good ? 0 : 1;
}

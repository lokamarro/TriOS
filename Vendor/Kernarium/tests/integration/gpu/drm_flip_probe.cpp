// SPDX-License-Identifier: GPL-2.0-or-later
// Page-flip pacing of the Virtio-GPU display.
//
// Virtio-GPU has no vertical blank of its own, so the driver paces each CRTC
// with a DRM vblank timer at the refresh rate of the mode. The probe takes the
// first connected output of the virtio_gpu card, sets its preferred mode, and
//   - waits for 30 vblanks with DRM_IOCTL_WAIT_VBLANK, and
//   - flips two dumb buffers for two seconds, one flip in flight at a time,
//     asking for a completion event each time, and times the events.
// A paced display completes flips at the refresh rate of the mode; a CRTC with
// no vblank completes each flip as it is queued. Both rates must be within 10%
// of the refresh rate. The probe prints one PASS or FAIL line per check and
// ends with UML_DRM_VBLANK_PROBE: PASS or FAIL, exiting 0 only after PASS.
//
// Build for the guest architecture with c++ -std=c++20 -O2 -Wall -Wextra
// -Werror -static.
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

// The connector status of a connected output, which libdrm names and the kernel
// headers do not.
constexpr uint32_t connector_connected = 1;

void report(const char *name, bool ok, const std::string &detail)
{
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
	if (!ok)
		++failures;
}

std::string errno_text(int e)
{
	return std::to_string(e) + " " + std::strerror(e);
}

double now_ms()
{
	timespec t{};
	clock_gettime(CLOCK_MONOTONIC, &t);
	return static_cast<double>(t.tv_sec) * 1000.0 + static_cast<double>(t.tv_nsec) / 1e6;
}

int ioctl_retry(int fd, unsigned long request, void *argument)
{
	int result;
	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && (errno == EINTR || errno == EAGAIN));
	return result;
}

// The first card whose driver is virtio_gpu.
int open_virtio_gpu()
{
	for (int i = 0; i < 8; ++i) {
		std::string path = "/dev/dri/card" + std::to_string(i);
		int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		char name[64] = {};
		drm_version version{};
		version.name = name;
		version.name_len = sizeof name - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 &&
		    std::strcmp(name, "virtio_gpu") == 0) {
			std::printf("INFO card %s\n", path.c_str());
			return fd;
		}
		close(fd);
	}
	return -1;
}

struct Output {
	uint32_t connector = 0;
	uint32_t crtc = 0;
	drm_mode_modeinfo mode{};
};

bool find_output(int fd, Output &out)
{
	drm_mode_card_res resources{};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0)
		return false;
	std::vector<uint32_t> crtcs(resources.count_crtcs), connectors(resources.count_connectors),
		encoders(resources.count_encoders), fbs(resources.count_fbs);
	resources.crtc_id_ptr = reinterpret_cast<uintptr_t>(crtcs.data());
	resources.connector_id_ptr = reinterpret_cast<uintptr_t>(connectors.data());
	resources.encoder_id_ptr = reinterpret_cast<uintptr_t>(encoders.data());
	resources.fb_id_ptr = reinterpret_cast<uintptr_t>(fbs.data());
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0)
		return false;
	for (uint32_t connector_id : connectors) {
		drm_mode_get_connector connector{};
		connector.connector_id = connector_id;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0)
			continue;
		std::vector<drm_mode_modeinfo> modes(connector.count_modes);
		std::vector<uint32_t> connector_encoders(connector.count_encoders);
		connector.modes_ptr = reinterpret_cast<uintptr_t>(modes.data());
		connector.encoders_ptr = reinterpret_cast<uintptr_t>(connector_encoders.data());
		connector.count_props = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) != 0 ||
		    connector.connection != connector_connected || modes.empty())
			continue;
		drm_mode_get_encoder encoder{};
		encoder.encoder_id = connector.encoder_id ? connector.encoder_id
							  : (connector_encoders.empty() ? 0 : connector_encoders[0]);
		if (encoder.encoder_id == 0 || ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0)
			continue;
		uint32_t crtc = encoder.crtc_id;
		for (size_t i = 0; crtc == 0 && i < crtcs.size(); ++i)
			if (encoder.possible_crtcs & (1u << i))
				crtc = crtcs[i];
		if (crtc == 0)
			continue;
		out.connector = connector_id;
		out.crtc = crtc;
		out.mode = modes[0];
		return true;
	}
	return false;
}

struct Buffer {
	uint32_t handle = 0;
	uint32_t fb = 0;
};

bool make_buffer(int fd, const drm_mode_modeinfo &mode, uint8_t fill, Buffer &buffer)
{
	drm_mode_create_dumb create{};
	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
		return false;
	buffer.handle = create.handle;
	drm_mode_fb_cmd2 fb{};
	fb.width = mode.hdisplay;
	fb.height = mode.vdisplay;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = create.handle;
	fb.pitches[0] = create.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0)
		return false;
	buffer.fb = fb.fb_id;
	drm_mode_map_dumb map{};
	map.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0)
		return true; // the pixels are not what is measured
	void *pixels = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			    static_cast<off_t>(map.offset));
	if (pixels != MAP_FAILED) {
		std::memset(pixels, fill, create.size);
		munmap(pixels, create.size);
	}
	return true;
}

double median(std::vector<double> values)
{
	if (values.empty())
		return 0;
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

} // namespace

int main(int argc, char **)
{
	if (argc != 1) {
		std::fprintf(stderr, "usage: drm_flip_probe\n");
		return 125;
	}
	int fd = open_virtio_gpu();
	if (fd < 0) {
		report("virtio-gpu", false, "no virtio_gpu card in /dev/dri");
		return failures;
	}
	ioctl(fd, DRM_IOCTL_SET_MASTER, nullptr);
	Output output;
	if (!find_output(fd, output)) {
		report("output", false, "no connected output");
		return failures;
	}
	const drm_mode_modeinfo &mode = output.mode;
	double refresh_hz = static_cast<double>(mode.clock) * 1000.0 /
			    (static_cast<double>(mode.htotal) * static_cast<double>(mode.vtotal));
	std::printf("INFO mode %ux%u refresh %.2f Hz\n", mode.hdisplay, mode.vdisplay, refresh_hz);
	Buffer buffers[2];
	if (!make_buffer(fd, mode, 0x40, buffers[0]) || !make_buffer(fd, mode, 0xc0, buffers[1])) {
		report("buffers", false, errno_text(errno));
		return failures;
	}
	drm_mode_crtc set{};
	set.crtc_id = output.crtc;
	set.fb_id = buffers[0].fb;
	set.set_connectors_ptr = reinterpret_cast<uintptr_t>(&output.connector);
	set.count_connectors = 1;
	set.mode_valid = 1;
	set.mode = mode;
	if (ioctl_retry(fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
		report("modeset", false, errno_text(errno));
		return failures;
	}
	report("modeset", true, "preferred mode on the first output");

	// A bare CRTC index for the vblank requests: the position of the CRTC in
	// the resource list.
	uint32_t crtc_index = 0;
	{
		drm_mode_card_res resources{};
		ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
		std::vector<uint32_t> crtcs(resources.count_crtcs);
		resources.crtc_id_ptr = reinterpret_cast<uintptr_t>(crtcs.data());
		resources.count_connectors = resources.count_encoders = resources.count_fbs = 0;
		ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources);
		for (size_t i = 0; i < crtcs.size(); ++i)
			if (crtcs[i] == output.crtc)
				crtc_index = static_cast<uint32_t>(i);
	}

	// Thirty vblanks, one at a time.
	bool wait_ok = true;
	int wait_errno = 0;
	double wait_begin = now_ms();
	for (int i = 0; i < 30 && wait_ok; ++i) {
		drm_wait_vblank vblank{};
		vblank.request.type = static_cast<drm_vblank_seq_type>(
			_DRM_VBLANK_RELATIVE |
			((crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK));
		vblank.request.sequence = 1;
		if (ioctl_retry(fd, DRM_IOCTL_WAIT_VBLANK, &vblank) != 0) {
			wait_ok = false;
			wait_errno = errno;
		}
	}
	double wait_hz = 30.0 * 1000.0 / (now_ms() - wait_begin);
	report("wait-vblank", wait_ok && wait_hz > refresh_hz * 0.9 && wait_hz < refresh_hz * 1.1,
	       wait_ok ? "30 vblanks at " + std::to_string(wait_hz) + " Hz" : errno_text(wait_errno));

	// Two seconds of flips, one in flight at a time.
	std::vector<double> arrivals;
	std::vector<double> stamps;
	int flipped = 0;
	bool flip_ok = true;
	int flip_errno = 0;
	double begin = now_ms();
	while (now_ms() - begin < 2000.0 && flip_ok) {
		Buffer &next = buffers[(flipped + 1) % 2];
		drm_mode_crtc_page_flip flip{};
		flip.crtc_id = output.crtc;
		flip.fb_id = next.fb;
		flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
		flip.user_data = static_cast<uint64_t>(flipped);
		if (ioctl_retry(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
			flip_ok = false;
			flip_errno = errno;
			break;
		}
		pollfd waiter{fd, POLLIN, 0};
		if (poll(&waiter, 1, 1000) != 1) {
			flip_ok = false;
			flip_errno = ETIMEDOUT;
			break;
		}
		char buffer[256];
		ssize_t got = read(fd, buffer, sizeof buffer);
		if (got < static_cast<ssize_t>(sizeof(drm_event))) {
			flip_ok = false;
			flip_errno = EIO;
			break;
		}
		for (ssize_t at = 0; at + static_cast<ssize_t>(sizeof(drm_event)) <= got;) {
			auto *event = reinterpret_cast<drm_event *>(buffer + at);
			if (event->type == DRM_EVENT_FLIP_COMPLETE &&
			    event->length >= sizeof(drm_event_vblank)) {
				auto *vblank = reinterpret_cast<drm_event_vblank *>(event);
				stamps.push_back(static_cast<double>(vblank->tv_sec) * 1000.0 +
						 static_cast<double>(vblank->tv_usec) / 1000.0);
				arrivals.push_back(now_ms());
				++flipped;
			}
			if (event->length == 0)
				break;
			at += event->length;
		}
	}
	double flip_hz = flipped > 1 ? (flipped - 1) * 1000.0 / (arrivals.back() - arrivals.front()) : 0;
	std::vector<double> gaps;
	for (size_t i = 1; i < stamps.size(); ++i)
		gaps.push_back(stamps[i] - stamps[i - 1]);
	char summary[160];
	std::snprintf(summary, sizeof summary,
		      "%d flips at %.1f Hz, event gap median %.2f ms, min %.2f ms, max %.2f ms",
		      flipped, flip_hz, median(gaps),
		      gaps.empty() ? 0.0 : *std::min_element(gaps.begin(), gaps.end()),
		      gaps.empty() ? 0.0 : *std::max_element(gaps.begin(), gaps.end()));
	report("flip-paced", flip_ok && flip_hz > refresh_hz * 0.9 && flip_hz < refresh_hz * 1.1,
	       flip_ok ? summary : errno_text(flip_errno));
	std::printf("UML_DRM_VBLANK_PROBE: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

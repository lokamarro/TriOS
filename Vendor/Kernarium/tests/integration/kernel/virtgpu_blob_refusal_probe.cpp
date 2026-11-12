// SPDX-License-Identifier: GPL-2.0-or-later
/* A blob the host refuses never becomes a handle.
 *
 * The ioctl that creates a blob (DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB) used
 * to queue its command and return without reading the host's answer, so a blob
 * the host could not create still came back as a GEM handle that no host
 * resource stood behind.  The ioctl now waits for the answer and fails, with no
 * handle: ENOMEM when the host is out of memory and EINVAL for any other
 * refusal.  Asking for a blob that names no host object is refused as EINVAL
 * everywhere, so the rows below do not depend on the GPU memory budget; they
 * check both blob branches (a host3d blob and a guest blob that the host must
 * name), that a refusal leaves the descriptor usable, and that an ordinary
 * host3d blob still works.  A last row makes the host run out of memory as
 * gralloc does: the storage of a resource too large for the GPU budget is
 * created inside the blob's own command buffer, where the host cannot report
 * the refusal, and the blob that names it is answered out of memory, so the
 * ioctl fails with ENOMEM.  The exit status is the number of failed checks.
 *
 * The UAPI structures are declared here, as the header is not in every guest.
 */
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

struct GetParam {
	uint64_t param;
	uint64_t value;
};

struct ContextParam {
	uint64_t param;
	uint64_t value;
};

struct ContextInit {
	uint32_t num_params;
	uint32_t pad;
	uint64_t ctx_set_params;
};

struct CreateBlob {
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint32_t bo_handle;
	uint32_t res_handle;
	uint64_t size;
	uint32_t pad;
	uint32_t cmd_size;
	uint64_t cmd;
	uint64_t blob_id;
};

struct GemClose {
	uint32_t handle;
	uint32_t pad;
};

constexpr unsigned command_base = 0x40;
constexpr unsigned long ioctl_get_param = _IOWR('d', command_base + 0x03, GetParam);
constexpr unsigned long ioctl_create_blob = _IOWR('d', command_base + 0x0a, CreateBlob);
constexpr unsigned long ioctl_context_init = _IOWR('d', command_base + 0x0b, ContextInit);
constexpr unsigned long ioctl_gem_close = _IOW('d', 0x09, GemClose);

constexpr uint64_t param_3d_features = 1;
constexpr uint64_t param_resource_blob = 3;
constexpr uint64_t param_context_init = 6;
constexpr uint64_t param_supported_capsets = 7;
constexpr uint64_t context_param_capset_id = 1;

constexpr uint32_t blob_mem_host3d = 2;
constexpr uint32_t blob_mem_host3d_guest = 3;
constexpr uint32_t blob_flag_mappable = 1;
constexpr uint32_t blob_flag_shareable = 2;

constexpr uint64_t missing_blob_id = 0x7ffffff0;

/* A 2D render target 12288 pixels square: 576 MiB, which fits the aperture but
 * not the GPU budget of a 1 GiB guest (at most half its memory). */
constexpr uint32_t virgl_capset = 2;
constexpr uint32_t command_pipe_resource_create = 48;
constexpr uint32_t pipe_texture_2d = 2;
constexpr uint32_t virgl_format_r8g8b8a8_unorm = 67;
constexpr uint32_t virgl_bind_render_target = 1u << 1;
constexpr uint32_t virgl_bind_sampler_view = 1u << 3;
constexpr uint32_t too_large_side = 12288;
constexpr int refusals = 64;

int failures = 0;

void check(bool ok, const char *what, const char *detail = "")
{
	std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, *detail ? ": " : "",
		    detail);
	if (!ok)
		++failures;
}

__attribute__((format(printf, 3, 4)))
void checkf(bool ok, const char *what, const char *format, ...)
{
	char detail[96];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(detail, sizeof detail, format, arguments);
	va_end(arguments);
	check(ok, what, detail);
}

int open_render_node()
{
	for (int attempt = 0; attempt < 120; ++attempt) {
		const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		sleep(1);
	}
	return -1;
}

bool query(int fd, uint64_t param, uint64_t &value)
{
	GetParam request{param, reinterpret_cast<uintptr_t>(&value)};
	return ioctl(fd, ioctl_get_param, &request) == 0;
}

/* One blob creation; the outcome is the ioctl's result and errno. */
struct Outcome {
	int result;
	int error;
	uint32_t handle;
};

Outcome create(int fd, uint32_t mem, uint32_t flags, uint64_t size, uint64_t blob_id)
{
	CreateBlob request{};
	request.blob_mem = mem;
	request.blob_flags = flags;
	request.size = size;
	request.blob_id = blob_id;
	errno = 0;
	const int result = ioctl(fd, ioctl_create_blob, &request);
	return {result, result ? errno : 0, request.bo_handle};
}

void close_handle(int fd, uint32_t handle)
{
	GemClose request{handle, 0};
	ioctl(fd, ioctl_gem_close, &request);
}

bool refused(const Outcome &outcome)
{
	return outcome.result != 0 && (outcome.error == EINVAL || outcome.error == ENOMEM) &&
	       outcome.handle == 0;
}

void report(const char *what, const Outcome &outcome)
{
	checkf(refused(outcome), what, "ioctl %d, errno %d, handle %u", outcome.result,
	       outcome.error, outcome.handle);
	if (outcome.result == 0 && outcome.handle != 0)
		/* The phantom the ioctl used to hand out. */
		std::printf("     (a handle for a blob the host never made)\n");
}

}  // namespace

int main()
{
	const int fd = open_render_node();
	if (fd < 0) {
		check(false, "open the render node", "no /dev/dri/renderD128");
		return failures;
	}

	uint64_t three_d = 0, blob = 0, init = 0, capsets = 0;
	check(query(fd, param_3d_features, three_d) && three_d == 1, "the device has 3D");
	check(query(fd, param_resource_blob, blob) && blob == 1, "the device has blob resources");
	check(query(fd, param_context_init, init) && init == 1, "the device has context init");
	if (failures)
		return failures;
	check(query(fd, param_supported_capsets, capsets) && capsets != 0, "the device lists capsets");

	/* A context of the first capset the host offers, preferring Venus. */
	uint64_t capset = 0;
	for (uint64_t id : {4, 2, 1})
		if (capsets & (1ull << id)) {
			capset = id;
			break;
		}
	ContextParam parameter{context_param_capset_id, capset};
	ContextInit context{1, 0, reinterpret_cast<uintptr_t>(&parameter)};
	checkf(capset != 0 && ioctl(fd, ioctl_context_init, &context) == 0, "create a context",
	       "capset %llu, errno %d", static_cast<unsigned long long>(capset), errno);
	if (failures)
		return failures;

	/* The host cannot satisfy a blob that names no object of its own. */
	report("host3d blob of a missing host object",
	       create(fd, blob_mem_host3d, blob_flag_mappable | blob_flag_shareable, 4096,
		      missing_blob_id));
	report("guest blob that names a missing host object",
	       create(fd, blob_mem_host3d_guest, blob_flag_shareable, 4096, missing_blob_id));

	/* Refusals leave the device and the descriptor usable, again and again. */
	int accepted = 0;
	for (int i = 0; i < refusals; ++i) {
		const Outcome outcome =
			create(fd, blob_mem_host3d, blob_flag_mappable, 4096, missing_blob_id + 16 + i);
		if (!refused(outcome))
			++accepted;
		if (outcome.result == 0 && outcome.handle != 0)
			close_handle(fd, outcome.handle);
	}
	checkf(accepted == 0, "repeated refusals", "%d of %d were accepted", accepted, refusals);

	/* The budget refuses the storage that gralloc creates inside the blob's
	 * command buffer; the blob that names it is the host's chance to say why.
	 * It needs a context of the virgl capset, so it has a descriptor of its
	 * own. */
	const int virgl = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (virgl >= 0 && (capsets & (1ull << virgl_capset))) {
		ContextParam virgl_parameter{context_param_capset_id, virgl_capset};
		ContextInit virgl_context{1, 0, reinterpret_cast<uintptr_t>(&virgl_parameter)};
		check(ioctl(virgl, ioctl_context_init, &virgl_context) == 0, "create a virgl context");
		const uint32_t commands[] = {
			(11u << 16) | command_pipe_resource_create,
			pipe_texture_2d,
			virgl_format_r8g8b8a8_unorm,
			virgl_bind_render_target | virgl_bind_sampler_view,
			too_large_side,
			too_large_side,
			1,
			1,
			0,
			0,
			0,
			0x51,
		};
		CreateBlob request{};
		request.blob_mem = blob_mem_host3d;
		request.blob_flags = blob_flag_mappable | blob_flag_shareable;
		request.size = uint64_t{too_large_side} * too_large_side * 4;
		request.cmd_size = sizeof commands;
		request.cmd = reinterpret_cast<uintptr_t>(commands);
		request.blob_id = 0x51;
		errno = 0;
		const int result = ioctl(virgl, ioctl_create_blob, &request);
		checkf(result == -1 && errno == ENOMEM && request.bo_handle == 0,
		       "blob of a resource the budget refuses", "ioctl %d, errno %d, handle %u",
		       result, result ? errno : 0, request.bo_handle);
		if (result == 0 && request.bo_handle != 0)
			close_handle(virgl, request.bo_handle);
	} else {
		check(false, "the virgl capset is offered");
	}
	if (virgl >= 0)
		close(virgl);

	/* An ordinary host3d blob, the anonymous kind Venus uses for its shared
	 * memory, is created and gets a handle and a resource. */
	CreateBlob ordinary{};
	ordinary.blob_mem = blob_mem_host3d;
	ordinary.blob_flags = blob_flag_mappable;
	ordinary.size = 65536;
	const int result = ioctl(fd, ioctl_create_blob, &ordinary);
	checkf(result == 0 && ordinary.bo_handle != 0 && ordinary.res_handle != 0,
	       "an ordinary host3d blob", "ioctl %d, errno %d, handle %u, resource %u", result,
	       result ? errno : 0, ordinary.bo_handle, ordinary.res_handle);
	if (result == 0 && ordinary.bo_handle != 0)
		close_handle(fd, ordinary.bo_handle);

	std::printf("%d failure(s)\n", failures);
	return failures;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/* A Virtio-GPU context names the guest process that created it.
 *
 * The host ends the application that holds the most graphics memory, so it has
 * to know which process a context belongs to. The guest kernel appends the
 * creating process to the debug name it sends with the context,
 * "<name>|pid=<tgid>|comm=<comm>", and the host credits the context's memory to
 * that process.  This probe makes three processes, each holding graphics
 * memory in a context of its own, and prints who they are:
 *
 *   gl     a Virgl context holding an 8 MiB texture, the context's name being
 *          its thread's;
 *   vk     a Venus context holding 48 MiB of Vulkan device memory, made by
 *          Mesa's Venus driver, which gives the context a name of its own;
 *   spoof  a Virgl context of a process whose command name imitates the
 *          suffix ("a|pid=1|comm=z"): the kernel replaces its '|'.
 *
 * Each line "OWNER <kind> <pid> <comm> <bytes>" is a claim that the runner
 * (run_guest_virtgpu_context_owner.py) checks against the holders the host
 * reports for the guest while the processes hold their memory.  The exit
 * status is the number of processes that could not make their context.
 *
 * The UAPI structures and the Vulkan entry points are declared here: neither
 * header is in every guest.
 */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint64_t mebibyte = 1u << 20;

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

struct ResourceCreate {
	uint32_t target;
	uint32_t format;
	uint32_t bind;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;
	uint32_t bo_handle;
	uint32_t res_handle;
	uint32_t size;
	uint32_t stride;
};

constexpr unsigned command_base = 0x40;
constexpr unsigned long ioctl_get_param = _IOWR('d', command_base + 0x03, GetParam);
constexpr unsigned long ioctl_resource_create =
	_IOWR('d', command_base + 0x04, ResourceCreate);
constexpr unsigned long ioctl_context_init = _IOWR('d', command_base + 0x0b, ContextInit);

constexpr uint64_t param_supported_capsets = 7;
constexpr uint64_t context_param_capset_id = 1;
constexpr uint32_t virgl_capset = 2;
constexpr uint32_t pipe_texture_2d = 2;
constexpr uint32_t virgl_format_r8g8b8a8_unorm = 67;
constexpr uint32_t virgl_bind_render_target = 1u << 1;
constexpr uint32_t virgl_bind_sampler_view = 1u << 3;

/* Failures that end a process before it holds its memory. */
int fail(const char *what)
{
	std::printf("FAIL %s: errno %d\n", what, errno);
	std::fflush(stdout);
	return 1;
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

/* The process the runner will look for: its command name as the kernel has
 * it, which is what the context carries. */
void own_name(const char *comm)
{
	prctl(PR_SET_NAME, comm, 0, 0, 0);
}

void say(const char *kind, const char *comm, uint64_t bytes)
{
	char actual[32] = {};
	prctl(PR_GET_NAME, actual, 0, 0, 0);
	std::printf("OWNER %s %d %s %llu\n", kind, static_cast<int>(getpid()),
		    *actual ? actual : comm, static_cast<unsigned long long>(bytes));
	std::fflush(stdout);
}

/* A Virgl context holding a texture of `side` x `side/2` RGBA pixels. */
int hold_virgl(const char *kind, const char *comm, uint32_t side)
{
	own_name(comm);
	const int fd = open_render_node();
	if (fd < 0)
		return fail("open the render node");
	uint64_t capsets = 0;
	GetParam query{param_supported_capsets, reinterpret_cast<uintptr_t>(&capsets)};
	if (ioctl(fd, ioctl_get_param, &query) != 0 || !(capsets & (1ull << virgl_capset))) {
		std::printf("FAIL the host offers no Virgl capset\n");
		return 1;
	}
	ContextParam parameter{context_param_capset_id, virgl_capset};
	ContextInit init{1, 0, reinterpret_cast<uintptr_t>(&parameter)};
	if (ioctl(fd, ioctl_context_init, &init) != 0)
		return fail("create a Virgl context");
	ResourceCreate texture{};
	texture.target = pipe_texture_2d;
	texture.format = virgl_format_r8g8b8a8_unorm;
	texture.bind = virgl_bind_render_target | virgl_bind_sampler_view;
	texture.width = side;
	texture.height = side / 2;
	texture.depth = 1;
	texture.array_size = 1;
	if (ioctl(fd, ioctl_resource_create, &texture) != 0)
		return fail("create a texture");
	say(kind, comm, uint64_t{side} * (side / 2) * 4);
	return -1;
}

/* The Vulkan structures that a device with one allocation needs. */
struct ApplicationInfo {
	uint32_t type;
	const void *next;
	const char *application_name;
	uint32_t application_version;
	const char *engine_name;
	uint32_t engine_version;
	uint32_t api_version;
};
struct InstanceCreateInfo {
	uint32_t type;
	const void *next;
	uint32_t flags;
	const ApplicationInfo *application;
	uint32_t layer_count;
	const char *const *layers;
	uint32_t extension_count;
	const char *const *extensions;
};
struct QueueCreateInfo {
	uint32_t type;
	const void *next;
	uint32_t flags;
	uint32_t family;
	uint32_t count;
	const float *priorities;
};
struct DeviceCreateInfo {
	uint32_t type;
	const void *next;
	uint32_t flags;
	uint32_t queue_count;
	const QueueCreateInfo *queues;
	uint32_t layer_count;
	const char *const *layers;
	uint32_t extension_count;
	const char *const *extensions;
	const void *features;
};
struct MemoryAllocateInfo {
	uint32_t type;
	const void *next;
	uint64_t size;
	uint32_t memory_type;
};
struct MemoryType {
	uint32_t flags;
	uint32_t heap;
};
struct MemoryHeap {
	uint64_t size;
	uint32_t flags;
};
struct MemoryProperties {
	uint32_t type_count;
	MemoryType types[32];
	uint32_t heap_count;
	MemoryHeap heaps[16];
};

using Handle = void *;
using CreateInstance = int32_t (*)(const InstanceCreateInfo *, const void *, Handle *);
using EnumerateDevices = int32_t (*)(Handle, uint32_t *, Handle *);
using GetMemoryProperties = void (*)(Handle, MemoryProperties *);
using CreateDevice = int32_t (*)(Handle, const DeviceCreateInfo *, const void *, Handle *);
using AllocateMemory = int32_t (*)(Handle, const MemoryAllocateInfo *, const void *, uint64_t *);

constexpr uint32_t memory_device_local = 1;

/* Mesa's Venus driver, whatever else the guest has: the loader is told to use
 * the first ICD whose file names virtio. */
bool choose_venus_driver()
{
	DIR *directory = opendir("/usr/share/vulkan/icd.d");
	if (!directory)
		return false;
	char path[512] = {};
	while (const dirent *entry = readdir(directory)) {
		if (std::strstr(entry->d_name, "virtio") && std::strstr(entry->d_name, ".json")) {
			std::snprintf(path, sizeof path, "/usr/share/vulkan/icd.d/%s", entry->d_name);
			break;
		}
	}
	closedir(directory);
	if (!*path)
		return false;
	setenv("VK_ICD_FILENAMES", path, 1);
	setenv("VK_DRIVER_FILES", path, 1);
	return true;
}

/* A Venus context holding `bytes` of device-local Vulkan memory. */
int hold_venus(const char *kind, const char *comm, uint64_t bytes)
{
	own_name(comm);
	if (!choose_venus_driver()) {
		std::printf("FAIL the guest has no Venus Vulkan driver\n");
		return 1;
	}
	void *library = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!library) {
		std::printf("FAIL libvulkan.so.1: %s\n", dlerror());
		return 1;
	}
	auto create_instance = reinterpret_cast<CreateInstance>(dlsym(library, "vkCreateInstance"));
	auto enumerate = reinterpret_cast<EnumerateDevices>(dlsym(library, "vkEnumeratePhysicalDevices"));
	auto memory_properties =
		reinterpret_cast<GetMemoryProperties>(dlsym(library, "vkGetPhysicalDeviceMemoryProperties"));
	auto create_device = reinterpret_cast<CreateDevice>(dlsym(library, "vkCreateDevice"));
	auto allocate = reinterpret_cast<AllocateMemory>(dlsym(library, "vkAllocateMemory"));
	if (!create_instance || !enumerate || !memory_properties || !create_device || !allocate) {
		std::printf("FAIL the Vulkan loader lacks an entry point\n");
		return 1;
	}
	ApplicationInfo application{0, nullptr, "context-owner", 1, nullptr, 0, (1u << 22) | (1u << 12)};
	InstanceCreateInfo instance_info{1, nullptr, 0, &application, 0, nullptr, 0, nullptr};
	Handle instance = nullptr;
	if (int32_t result = create_instance(&instance_info, nullptr, &instance)) {
		std::printf("FAIL vkCreateInstance: %d\n", result);
		return 1;
	}
	uint32_t count = 1;
	Handle physical = nullptr;
	int32_t listed = enumerate(instance, &count, &physical);
	if ((listed != 0 && listed != 5 /* VK_INCOMPLETE */) || count == 0) {
		std::printf("FAIL vkEnumeratePhysicalDevices: %d, %u\n", listed, count);
		return 1;
	}
	MemoryProperties properties{};
	memory_properties(physical, &properties);
	uint32_t memory_type = properties.type_count;
	for (uint32_t i = 0; i < properties.type_count; ++i)
		if (properties.types[i].flags & memory_device_local) {
			memory_type = i;
			break;
		}
	if (memory_type == properties.type_count) {
		std::printf("FAIL no device-local memory type\n");
		return 1;
	}
	const float priority = 1.0f;
	QueueCreateInfo queue{2, nullptr, 0, 0, 1, &priority};
	DeviceCreateInfo device_info{3, nullptr, 0, 1, &queue, 0, nullptr, 0, nullptr, nullptr};
	Handle device = nullptr;
	if (int32_t result = create_device(physical, &device_info, nullptr, &device)) {
		std::printf("FAIL vkCreateDevice: %d\n", result);
		return 1;
	}
	MemoryAllocateInfo allocation{5, nullptr, bytes, memory_type};
	uint64_t memory = 0;
	if (int32_t result = allocate(device, &allocation, nullptr, &memory)) {
		std::printf("FAIL vkAllocateMemory: %d\n", result);
		return 1;
	}
	say(kind, comm, bytes);
	return -1;
}

struct Child {
	const char *kind;
	pid_t pid;
	int ready;
	int hold;
	bool ready_seen;
};

/* A process that makes its context, says so, and holds it until the pipe it
 * reads is closed. */
template <typename Make>
pid_t start(Child &child, Make make)
{
	int ready[2], hold[2];
	if (pipe(ready) != 0 || pipe(hold) != 0)
		return -1;
	const pid_t pid = fork();
	if (pid == 0) {
		close(ready[0]);
		close(hold[1]);
		const int status = make();
		if (status >= 0)
			_exit(status);
		(void)!write(ready[1], "r", 1);
		pollfd waiting{hold[0], POLLIN, 0};
		poll(&waiting, 1, 120 * 1000);
		_exit(0);
	}
	close(ready[1]);
	close(hold[0]);
	child.pid = pid;
	child.ready = ready[0];
	child.hold = hold[1];
	return pid;
}

}  // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	Child children[] = {{"gl", 0, -1, -1, false}, {"vk", 0, -1, -1, false}, {"spoof", 0, -1, -1, false}};
	int failures = 0;
	if (start(children[0], [] { return hold_virgl("gl", "glowner", 2048); }) < 0 ||
	    start(children[1], [] { return hold_venus("vk", "vkowner", 48 * mebibyte); }) < 0 ||
	    start(children[2], [] { return hold_virgl("spoof", "a|pid=1|comm=z", 1448); }) < 0) {
		std::printf("FAIL fork\n");
		return 1;
	}

	for (Child &child : children) {
		pollfd waiting{child.ready, POLLIN, 0};
		char byte;
		child.ready_seen =
			poll(&waiting, 1, 180 * 1000) > 0 && read(child.ready, &byte, 1) == 1;
		if (!child.ready_seen) {
			std::printf("FAIL the %s process did not make its context\n", child.kind);
			++failures;
		}
	}

	/* The host reports the holders while the processes hold their memory. */
	sleep(6);

	/* A process sees the end of its pipe only when no other process holds the
	 * write end, as the children made later inherited the earlier ones'. */
	for (Child &child : children)
		close(child.hold);
	for (Child &child : children) {
		int status = 0;
		waitpid(child.pid, &status, 0);
		if (child.ready_seen && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
			std::printf("FAIL the %s process ended with status %d\n", child.kind, status);
			++failures;
		}
	}
	std::printf("%d failure(s)\n", failures);
	return failures;
}

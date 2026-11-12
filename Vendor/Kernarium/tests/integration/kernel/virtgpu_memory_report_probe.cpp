// SPDX-License-Identifier: GPL-2.0-or-later
/* The host's graphics memory report, as the guest's reclaimer reads it.
 *
 * A host GPU that gives the guest a fixed memory budget tells the guest how
 * near the budget is and which guest processes hold it.  The guest kernel keeps
 * the answer in the root-only file gpu_memory_report on the DRM device
 * (kernel/common/README.md), which Android's lmkd reads: it arms the file,
 * waits for it to change with poll(), and ends the app that holds the most.
 * This probe is that reader.  It checks, one line per check as "PASS <what>" or
 * "FAIL <what>: <detail>":
 *
 *   - the file appears, is mode 0600 and holds no snapshot before it is asked;
 *   - "refresh" returns at once and the answer wakes poll() (POLLPRI);
 *   - "arm" and "disarm" are taken, and a malformed write is EINVAL;
 *   - a guest that holds nothing is woken by nothing;
 *   - a process that fills the budget raises PRESSURE, and the snapshot names
 *     it by process ID and command name with the memory it holds;
 *   - the arguments of "arm" filter the holders (a minimum of bytes, a
 *     minimum of idle time) and leave the state alone;
 *   - the state returns to calm when the process ends;
 *   - an allocation the host refuses raises REFUSED and counts;
 *   - a disarmed file is woken by nothing, under pressure too.
 *
 * The runner (run_guest_virtgpu_memory_report.py) also checks the launcher's
 * --stats line for "guest-reclaim=on" while the file is armed.  The exit status
 * is the number of checks that failed.
 *
 * The Vulkan entry points are declared here: no header is in every guest.
 */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint64_t mebibyte = 1u << 20;
constexpr uint64_t margin = 96 * mebibyte;
constexpr unsigned state_pressure = 1u << 0;
constexpr unsigned state_refused = 1u << 1;

uint64_t now_ms()
{
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000;
}

int failures = 0;

void check(bool ok, const char *what, const char *detail = "")
{
	if (ok) {
		std::printf("PASS %s\n", what);
	} else {
		std::printf("FAIL %s: %s\n", what, detail);
		++failures;
	}
	std::fflush(stdout);
}

/* ---- The report ---- */

struct Holder {
	unsigned pid;
	uint64_t bytes;
	unsigned idle_ms;
	unsigned contexts;
	char comm[32];
};

struct Report {
	bool valid;
	uint64_t seq, age_ms, refused, budget, used, held;
	unsigned state, armed, generation;
	int count;
	Holder holders[32];
	size_t length;
	char text[8192];

	const Holder *find(unsigned pid) const
	{
		for (int i = 0; i < count; ++i)
			if (holders[i].pid == pid)
				return &holders[i];
		return nullptr;
	}
};

/* The first line is key=value pairs, then one line per holder:
 * "<pid> <bytes> <idle_ms> <contexts> <comm>". */
bool parse(Report &report)
{
	report.valid = false;
	report.count = 0;
	char *line = report.text;
	char *end = std::strchr(line, '\n');
	if (!end)
		return false;
	*end = '\0';
	unsigned long long seq, age, refused, budget, used, held;
	if (std::sscanf(line,
			"seq=%llu age_ms=%llu state=%u refused=%llu budget=%llu used=%llu held=%llu "
			"armed=%u generation=%u",
			&seq, &age, &report.state, &refused, &budget, &used, &held, &report.armed,
			&report.generation) != 9)
		return false;
	report.seq = seq;
	report.age_ms = age;
	report.refused = refused;
	report.budget = budget;
	report.used = used;
	report.held = held;
	line = end + 1;
	while (*line && report.count < 32) {
		end = std::strchr(line, '\n');
		if (!end)
			return false;
		*end = '\0';
		Holder &holder = report.holders[report.count];
		unsigned long long bytes;
		int comm_at = 0;
		if (std::sscanf(line, "%u %llu %u %u %n", &holder.pid, &bytes, &holder.idle_ms,
				&holder.contexts, &comm_at) != 4 ||
		    comm_at == 0 || std::strlen(line + comm_at) >= sizeof holder.comm)
			return false;
		holder.bytes = bytes;
		std::strcpy(holder.comm, line + comm_at);
		++report.count;
		line = end + 1;
	}
	report.valid = true;
	return true;
}

/* Reads the file whole; a read restarts the file's change detection, so the
 * next poll() reports only what happens after it. */
bool read_report(int fd, Report &report)
{
	const ssize_t length = pread(fd, report.text, sizeof report.text - 1, 0);
	if (length <= 0)
		return false;
	report.length = size_t(length);
	report.text[length] = '\0';
	/* parse() writes into the text; keep a copy for messages. */
	char copy[sizeof report.text];
	std::memcpy(copy, report.text, size_t(length) + 1);
	const bool ok = parse(report);
	std::memcpy(report.text, copy, size_t(length) + 1);
	return ok;
}

/* What the file has shown since the start of a wait. */
struct Seen {
	unsigned state;
	uint64_t refused;
	int snapshots;
};

/* Waits for a snapshot that `accept` takes, for at most `timeout_ms`, reading
 * the file each time poll() reports a change.  A reader that was not woken
 * would find nothing, so the wait is poll()'s alone. */
template <typename Accept>
bool wait_for(int fd, Report &report, uint64_t timeout_ms, Seen *seen, Accept accept)
{
	const uint64_t deadline = now_ms() + timeout_ms;
	for (;;) {
		const uint64_t now = now_ms();
		if (now >= deadline)
			return false;
		pollfd waiting{fd, POLLPRI | POLLERR, 0};
		if (poll(&waiting, 1, int(deadline - now)) <= 0)
			return false;
		if (!read_report(fd, report))
			return false;
		if (seen) {
			seen->state |= report.state;
			if (report.refused > seen->refused)
				seen->refused = report.refused;
			++seen->snapshots;
		}
		if (accept(report))
			return true;
	}
}

/* Whether poll() reports a change within `timeout_ms`. */
bool woken_within(int fd, uint64_t timeout_ms)
{
	pollfd waiting{fd, POLLPRI | POLLERR, 0};
	return poll(&waiting, 1, int(timeout_ms)) > 0;
}

int write_command(int fd, const char *text)
{
	return write(fd, text, std::strlen(text)) < 0 ? -errno : 0;
}

/* The report on the guest's first DRM device that has one. */
int open_report()
{
	for (int attempt = 0; attempt < 120; ++attempt) {
		for (int card = 0; card < 4; ++card) {
			char path[96];
			std::snprintf(path, sizeof path, "/sys/class/drm/card%d/gpu_memory_report", card);
			const int fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				std::printf("report %s\n", path);
				return fd;
			}
		}
		sleep(1);
	}
	return -1;
}

/* ---- The process that holds device memory ---- */

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

struct Venus {
	Handle device;
	AllocateMemory allocate;
	uint32_t memory_type;
};

/* A Venus device, the context that holds what it allocates. */
bool venus_open(Venus &venus)
{
	if (!choose_venus_driver()) {
		std::printf("FAIL the guest has no Venus Vulkan driver\n");
		return false;
	}
	void *library = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!library) {
		std::printf("FAIL libvulkan.so.1: %s\n", dlerror());
		return false;
	}
	auto create_instance = reinterpret_cast<CreateInstance>(dlsym(library, "vkCreateInstance"));
	auto enumerate = reinterpret_cast<EnumerateDevices>(dlsym(library, "vkEnumeratePhysicalDevices"));
	auto memory_properties =
		reinterpret_cast<GetMemoryProperties>(dlsym(library, "vkGetPhysicalDeviceMemoryProperties"));
	auto create_device = reinterpret_cast<CreateDevice>(dlsym(library, "vkCreateDevice"));
	venus.allocate = reinterpret_cast<AllocateMemory>(dlsym(library, "vkAllocateMemory"));
	if (!create_instance || !enumerate || !memory_properties || !create_device || !venus.allocate) {
		std::printf("FAIL the Vulkan loader lacks an entry point\n");
		return false;
	}
	ApplicationInfo application{0, nullptr, "memory-report", 1, nullptr, 0, (1u << 22) | (1u << 12)};
	InstanceCreateInfo instance_info{1, nullptr, 0, &application, 0, nullptr, 0, nullptr};
	Handle instance = nullptr;
	if (int32_t result = create_instance(&instance_info, nullptr, &instance)) {
		std::printf("FAIL vkCreateInstance: %d\n", result);
		return false;
	}
	uint32_t count = 1;
	Handle physical = nullptr;
	int32_t listed = enumerate(instance, &count, &physical);
	if ((listed != 0 && listed != 5 /* VK_INCOMPLETE */) || count == 0) {
		std::printf("FAIL vkEnumeratePhysicalDevices: %d, %u\n", listed, count);
		return false;
	}
	MemoryProperties properties{};
	memory_properties(physical, &properties);
	venus.memory_type = properties.type_count;
	for (uint32_t i = 0; i < properties.type_count; ++i)
		if (properties.types[i].flags & memory_device_local) {
			venus.memory_type = i;
			break;
		}
	if (venus.memory_type == properties.type_count) {
		std::printf("FAIL no device-local memory type\n");
		return false;
	}
	const float priority = 1.0f;
	QueueCreateInfo queue{2, nullptr, 0, 0, 1, &priority};
	DeviceCreateInfo device_info{3, nullptr, 0, 1, &queue, 0, nullptr, 0, nullptr, nullptr};
	if (int32_t result = create_device(physical, &device_info, nullptr, &venus.device)) {
		std::printf("FAIL vkCreateDevice: %d\n", result);
		return false;
	}
	return true;
}

int32_t venus_allocate(Venus &venus, uint64_t bytes)
{
	MemoryAllocateInfo allocation{5, nullptr, bytes, venus.memory_type};
	uint64_t memory = 0;
	return venus.allocate(venus.device, &allocation, nullptr, &memory);
}

/* A holder is a process named `comm` with one Venus context.  It says "a" once
 * it holds a first allocation, which tells the parent the host's overhead for
 * a context, then allocates the size each 8 bytes the parent writes ("b" when
 * done), and holds all of it until the pipe ends.  A size that does not fit is
 * the point of the refusal check: the allocation is asynchronous, so the
 * process may live on or fail later, and the parent does not judge it. */
constexpr uint64_t first_allocation = 16 * mebibyte;

int holder_main(const char *comm, int ready, int command)
{
	prctl(PR_SET_NAME, comm, 0, 0, 0);
	Venus venus{};
	if (!venus_open(venus))
		return 1;
	if (int32_t result = venus_allocate(venus, first_allocation)) {
		std::printf("FAIL the first vkAllocateMemory: %d\n", result);
		return 1;
	}
	(void)!write(ready, "a", 1);
	for (;;) {
		pollfd waiting{command, POLLIN, 0};
		if (poll(&waiting, 1, 180 * 1000) <= 0)
			return 0;
		uint64_t bytes = 0;
		if (read(command, &bytes, sizeof bytes) != ssize_t(sizeof bytes))
			return 0;
		venus_allocate(venus, bytes);
		(void)!write(ready, "b", 1);
	}
}

struct Child {
	pid_t pid;
	int ready;
	int command;
};

Child start_holder(const char *comm)
{
	int ready[2], command[2];
	Child child{-1, -1, -1};
	if (pipe(ready) != 0 || pipe(command) != 0)
		return child;
	const pid_t pid = fork();
	if (pid == 0) {
		close(ready[0]);
		close(command[1]);
		_exit(holder_main(comm, ready[1], command[0]));
	}
	close(ready[1]);
	close(command[0]);
	child.pid = pid;
	child.ready = ready[0];
	child.command = command[1];
	return child;
}

bool child_says(const Child &child, char what, uint64_t timeout_ms)
{
	pollfd waiting{child.ready, POLLIN, 0};
	char byte = 0;
	return poll(&waiting, 1, int(timeout_ms)) > 0 && read(child.ready, &byte, 1) == 1 &&
	       byte == what;
}

void ask_to_hold(const Child &child, uint64_t bytes)
{
	(void)!write(child.command, &bytes, sizeof bytes);
}

/* Ends the holder and waits for it: its context and memory go with it. */
void end_holder(Child &child)
{
	if (child.pid < 0)
		return;
	close(child.command);
	close(child.ready);
	for (int attempt = 0; attempt < 100; ++attempt) {
		int status = 0;
		if (waitpid(child.pid, &status, WNOHANG) == child.pid)
			break;
		if (attempt == 50)
			kill(child.pid, SIGKILL);
		usleep(100 * 1000);
	}
	child.pid = -1;
}

}  // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	static Report report;
	static Report before;

	const int fd = open_report();
	check(fd >= 0, "the report file appears on the DRM device");
	if (fd < 0) {
		std::printf("%d failure(s)\n", failures);
		return failures;
	}

	struct stat info {};
	check(fstat(fd, &info) == 0 && (info.st_mode & 07777) == 0600 && info.st_uid == 0,
	      "the report is readable and writable by root alone");

	const bool first = read_report(fd, report);
	check(first && report.seq == 0 && report.armed == 0 && report.count == 0,
	      "an unasked report is empty and not armed");
	if (!first)
		std::printf("text: %s\n", report.text);

	/* A refresh returns at once; the answer arrives and wakes poll(). */
	uint64_t started = now_ms();
	const int refreshed = write_command(fd, "refresh");
	check(refreshed == 0 && now_ms() - started < 1000, "refresh returns without waiting for the host");
	check(wait_for(fd, report, 5000, nullptr, [](const Report &r) { return r.seq >= 1; }) &&
		      report.budget > 0 && report.armed == 0 && !(report.state & state_pressure) &&
		      report.length <= 4095,
	      "the answer to a refresh wakes poll() with the host's budget", report.text);
	std::printf("budget %llu MiB, used %llu MiB, held %llu MiB, age %llu ms\n",
		    (unsigned long long)(report.budget / mebibyte),
		    (unsigned long long)(report.used / mebibyte),
		    (unsigned long long)(report.held / mebibyte), (unsigned long long)report.age_ms);

	started = now_ms();
	read_report(fd, report);
	check(now_ms() - started < 200, "a read formats the stored answer without asking the host");

	/* Malformed writes change nothing. */
	check(write_command(fd, "bogus") == -EINVAL && write_command(fd, "arm 1") == -EINVAL &&
		      write_command(fd, "arm x 0") == -EINVAL &&
		      write_command(fd, "arm 0 0 0") == -EINVAL &&
		      write_command(fd, "disarm now") == -EINVAL,
	      "a malformed write is EINVAL");

	check(write_command(fd, "arm 0 0") == 0 &&
		      wait_for(fd, report, 5000, nullptr, [](const Report &r) { return r.armed == 1; }),
	      "arm is taken and the answer wakes poll()");

	read_report(fd, report);
	check(!woken_within(fd, 3000), "an armed guest that holds nothing is woken by nothing");

	/* The reclaimer's case: a process fills the budget. */
	Child holder = start_holder("memhold");
	check(holder.pid > 0 && child_says(holder, 'a', 180 * 1000),
	      "a process holds a first device allocation");
	write_command(fd, "refresh");
	check(wait_for(fd, report, 5000, nullptr,
		       [&](const Report &r) { return r.find(unsigned(holder.pid)) != nullptr; }),
	      "the snapshot names the process that holds device memory", report.text);
	const uint64_t budget = report.budget;
	const uint64_t overhead = report.used > report.held ? report.used - report.held : 0;
	const uint64_t held_now = report.held;
	/* The pressure condition is usage + 96 MiB >= budget, and the usage is
	 * what is held plus the overhead above it; 8 MiB over it fills the pool
	 * to a few tens of MiB short of a refusal. Memory the host's drivers
	 * cache is freed before the figure counts, but a holder's is not. */
	const uint64_t target = budget + 8 * mebibyte > margin + overhead + held_now
				? budget + 8 * mebibyte - margin - overhead - held_now
				: 8 * mebibyte;
	const uint64_t request = (target + mebibyte - 1) / mebibyte * mebibyte;
	std::printf("overhead %llu MiB, asking for %llu MiB more\n",
		    (unsigned long long)(overhead / mebibyte), (unsigned long long)(request / mebibyte));

	read_report(fd, report);
	started = now_ms();
	ask_to_hold(holder, request);
	check(child_says(holder, 'b', 60 * 1000), "the process holds the rest");
	Seen seen{};
	const bool pressured = wait_for(fd, report, 15000, &seen, [&](const Report &r) {
		return (r.state & state_pressure) != 0 && r.find(unsigned(holder.pid)) != nullptr;
	});
	check(pressured, "a full budget raises PRESSURE and the snapshot names the holder",
	      report.text);
	if (pressured) {
		const Holder *entry = report.find(unsigned(holder.pid));
		std::printf("pressure after %llu ms: %llu MiB held, used %llu of %llu MiB, %u context(s)\n",
			    (unsigned long long)(now_ms() - started),
			    (unsigned long long)(entry->bytes / mebibyte),
			    (unsigned long long)(report.used / mebibyte),
			    (unsigned long long)(report.budget / mebibyte), entry->contexts);
		check(std::strcmp(entry->comm, "memhold") == 0, "the holder carries its command name",
		      entry->comm);
		check(entry->bytes >= first_allocation + request - mebibyte && entry->contexts >= 1,
		      "the holder carries the memory it holds");
		check(report.armed == 1 && report.held >= entry->bytes && report.budget == budget,
		      "the header agrees with the holder");
	}

	/* The arguments of arm filter the holders, not the state. */
	write_command(fd, "arm 4294967296 0");
	check(wait_for(fd, report, 8000, nullptr,
		       [&](const Report &r) { return r.armed == 1 && r.count == 0; }) &&
		      (report.state & state_pressure),
	      "a minimum of bytes drops the holder and keeps the pressure", report.text);
	write_command(fd, "arm 0 3600000");
	check(wait_for(fd, report, 8000, nullptr,
		       [&](const Report &r) { return r.armed == 1 && r.count == 0; }) &&
		      (report.state & state_pressure),
	      "a minimum of idle time drops the holder and keeps the pressure", report.text);
	write_command(fd, "arm 0 0");
	check(wait_for(fd, report, 8000, nullptr,
		       [&](const Report &r) { return r.find(unsigned(holder.pid)) != nullptr; }),
	      "arm 0 0 lists the holder again", report.text);

	/* The process ends: the pool empties and the state is calm again. */
	read_report(fd, report);
	started = now_ms();
	end_holder(holder);
	const bool calm = wait_for(fd, report, 20000, nullptr, [&](const Report &r) {
		return !(r.state & state_pressure) && r.find(unsigned(holder.pid)) == nullptr;
	});
	check(calm, "the pressure ends when the holder ends", report.text);
	if (calm)
		std::printf("calm after %llu ms\n", (unsigned long long)(now_ms() - started));

	/* An allocation the pool refuses. */
	Child refuser = start_holder("memrefuse");
	check(refuser.pid > 0 && child_says(refuser, 'a', 180 * 1000),
	      "a second process holds a first device allocation");
	read_report(fd, before);
	Seen refusals{};
	refusals.refused = before.refused;
	ask_to_hold(refuser, before.budget + 64 * mebibyte);
	wait_for(fd, report, 15000, &refusals, [&](const Report &r) {
		return r.refused > before.refused && (refusals.state & state_refused) != 0;
	});
	check(refusals.refused > before.refused && (refusals.state & state_refused) != 0,
	      "a refused allocation raises REFUSED and counts", report.text);
	end_holder(refuser);

	/* Let the pool settle before the last check. */
	wait_for(fd, report, 12000, nullptr,
		 [](const Report &r) { return !(r.state & (state_pressure | state_refused)); });

	/* A disarmed file is woken by nothing. */
	check(write_command(fd, "disarm") == 0 &&
		      wait_for(fd, report, 5000, nullptr, [](const Report &r) { return r.armed == 0; }),
	      "disarm is taken and the answer wakes poll()");
	read_report(fd, report);
	Child quiet = start_holder("memquiet");
	if (quiet.pid > 0 && child_says(quiet, 'a', 180 * 1000)) {
		ask_to_hold(quiet, request);
		child_says(quiet, 'b', 60 * 1000);
		read_report(fd, report);
		check(!woken_within(fd, 5000), "a disarmed file is woken by nothing under pressure");
	} else {
		check(false, "a third process holds device memory");
	}
	write_command(fd, "refresh");
	check(wait_for(fd, report, 5000, nullptr, [&](const Report &r) {
		return r.armed == 0 && r.find(unsigned(quiet.pid)) != nullptr;
	}),
	      "a refresh still lists the holder when disarmed", report.text);
	end_holder(quiet);

	std::printf("%d failure(s)\n", failures);
	return failures;
}

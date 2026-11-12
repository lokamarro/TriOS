// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A process audit for tests that restart a guest in place.
 *
 * A test runner inserts this library into the launcher it starts
 * (DYLD_INSERT_LIBRARIES) and names a directory in UML_PROCESS_AUDIT_DIRECTORY.
 * The process then answers requests the runner leaves there, from a thread of
 * its own, with what only the process can say about itself: its descriptors,
 * threads, Mach ports, signal dispositions and stdio state, its next pthread
 * key and heap, and whether a pattern is anywhere in its memory. The library
 * does nothing in any process that is not a launcher, or without the
 * directory, and nothing in the product loads it.
 *
 *   request    "dump <label>"              writes <label>.txt
 *              "scan <label> <hex>"        writes <label>.scan: where the
 *                                          pattern is in the process, the
 *                                          native kernel window (guest RAM and
 *                                          the kernel) counted apart from the
 *                                          rest
 *
 * With UML_PROCESS_AUDIT_HAND_BACK set, the process also dumps itself as
 * handback-<n>.txt each time a restarted guest has been handed back to the
 * process: the library announces it with dprintf() on stderr ("Darwin: guest
 * handed back:") after the last step and before the next kernel exists, and
 * the audit interposes dprintf() to see that call. (Interposing write() would
 * not see it: the C library writes through its own internal entry.)
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr char thread_name[] = "uml-process-audit";
/* Where the macOS product puts the native kernel window, which holds guest
 * RAM, and how far from there a window can reach. */
constexpr std::uintptr_t window_begin = DARWIN_UML_MACOS_NATIVE_ADDRESS;
constexpr std::uintptr_t window_end = window_begin + (1ULL << 40);
constexpr std::string_view hand_back_line = "Darwin: guest handed back:";
/* The VM tags of the heaps the memory layer maps for itself and gives back
 * whole (memory/domain_heap.hpp): domain heaps and thread scratch. */
constexpr unsigned heap_tag = 241;
constexpr unsigned scratch_tag = 242;

/* Set by the constructor below. A path object with a constructor of its own
 * would be initialized by the dynamic initializers, which may run after it. */
const std::filesystem::path *directory = nullptr;
std::atomic<bool> hand_back_audit{};
std::atomic<unsigned> hand_backs{};

/* The status flags fcntl(F_SETFL) can set. Darwin also reports a
 * kernel-internal "was written" bit that no call clears. */
constexpr int settable_flags =
	O_NONBLOCK | O_APPEND | O_ASYNC | O_SYNC | O_DSYNC;

std::string descriptor_kind(std::uint32_t type)
{
	switch (type) {
	case PROX_FDTYPE_VNODE: return "vnode";
	case PROX_FDTYPE_SOCKET: return "socket";
	case PROX_FDTYPE_PSHM: return "pshm";
	case PROX_FDTYPE_PSEM: return "psem";
	case PROX_FDTYPE_KQUEUE: return "kqueue";
	case PROX_FDTYPE_PIPE: return "pipe";
	case PROX_FDTYPE_FSEVENTS: return "fsevents";
	default: return "type" + std::to_string(type);
	}
}

void write_descriptors(std::ostream &out)
{
	const pid_t pid = getpid();
	const int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
	if (bytes <= 0) {
		out << "error descriptors\n";
		return;
	}
	std::vector<proc_fdinfo> list(
		static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16U);
	const int used = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, list.data(),
		static_cast<int>(list.size() * sizeof(proc_fdinfo)));
	const auto count = static_cast<std::size_t>(std::max(used, 0)) /
		sizeof(proc_fdinfo);
	std::map<std::string, std::size_t> kinds;
	for (std::size_t index = 0; index != count; ++index) {
		const auto &entry = list[index];
		std::string kind = descriptor_kind(entry.proc_fdtype);
		if (entry.proc_fdtype == PROX_FDTYPE_VNODE) {
			vnode_fdinfowithpath info{};
			if (proc_pidfdinfo(pid, entry.proc_fd, PROC_PIDFDVNODEPATHINFO,
				    &info, sizeof(info)) == sizeof(info))
				kind += std::string(" ") + info.pvip.vip_path;
		} else if (entry.proc_fdtype == PROX_FDTYPE_SOCKET) {
			socket_fdinfo info{};
			if (proc_pidfdinfo(pid, entry.proc_fd, PROC_PIDFDSOCKETINFO,
				    &info, sizeof(info)) == sizeof(info)) {
				const auto &socket = info.psi;
				kind += socket.soi_family == AF_UNIX ? "/unix" :
					socket.soi_family == AF_INET ? "/inet" :
					socket.soi_family == AF_INET6 ? "/inet6" : "/other";
				if (socket.soi_family == AF_UNIX)
					kind += std::string(" ") +
						socket.soi_proto.pri_un.unsi_addr.ua_sun.sun_path;
			}
		}
		++kinds[kind];
	}
	out << "descriptors " << count << '\n';
	for (const auto &[kind, number] : kinds)
		out << "descriptor " << number << ' ' << kind << '\n';
}

void write_threads(std::ostream &out)
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		out << "error threads\n";
		return;
	}
	std::map<std::string, std::size_t> classes;
	std::size_t total = 0;
	for (mach_msg_type_number_t index = 0; index != count; ++index) {
		thread_identifier_info_data_t identity{};
		mach_msg_type_number_t identity_count = THREAD_IDENTIFIER_INFO_COUNT;
		std::string name;
		bool dispatch = false;
		if (thread_info(threads[index], THREAD_IDENTIFIER_INFO,
			    reinterpret_cast<thread_info_t>(&identity),
			    &identity_count) == KERN_SUCCESS) {
			dispatch = identity.dispatch_qaddr != 0;
			proc_threadinfo info{};
			if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO,
				    identity.thread_id, &info, sizeof(info)) ==
			    sizeof(info))
				name = info.pth_name;
		}
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
		/* The audit's own thread is not the process's. */
		if (name == thread_name)
			continue;
		/* A name class ignores digits, so "cpu3" and "cpu7" are one class
		 * and a boot with another number of them still differs. */
		std::string cls;
		for (const char character :
		     name.empty() ? std::string("(unnamed)") : name)
			if (character < '0' || character > '9')
				cls.push_back(character);
		++classes[cls + (dispatch ? " [dispatch]" : "")];
		++total;
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	out << "threads " << total << '\n';
	for (const auto &[cls, number] : classes)
		out << "thread " << number << ' ' << cls << '\n';
}

void write_ports(std::ostream &out)
{
	mach_port_name_array_t names = nullptr;
	mach_msg_type_number_t name_count = 0;
	mach_port_type_array_t types = nullptr;
	mach_msg_type_number_t type_count = 0;
	if (mach_port_names(mach_task_self(), &names, &name_count, &types,
		    &type_count) != KERN_SUCCESS) {
		out << "error ports\n";
		return;
	}
	/* By the rights held and the kernel object each names. The names
	 * themselves, and the array that lists them, are not the process's. */
	std::map<std::string, std::size_t> kinds;
	for (mach_msg_type_number_t index = 0; index != type_count; ++index) {
		ipc_info_object_type_t object_type{};
		mach_vm_address_t object_address = 0;
		const kern_return_t result = mach_port_kobject(mach_task_self(),
			names[index], &object_type, &object_address);
		char key[64];
		std::snprintf(key, sizeof(key), "rights=%#x kobject=%d", types[index],
			result == KERN_SUCCESS ? static_cast<int>(object_type) : -1);
		++kinds[key];
	}
	out << "ports " << name_count << '\n';
	for (const auto &[key, number] : kinds)
		out << "port " << number << ' ' << key << '\n';
	(void)vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(names),
		name_count * sizeof(mach_port_name_t));
	(void)vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(types),
		type_count * sizeof(mach_port_type_t));
}

void write_signals(std::ostream &out)
{
	for (int signal = 1; signal < NSIG; ++signal) {
		struct sigaction action {};
		if (sigaction(signal, nullptr, &action) != 0)
			continue;
		const auto handler = (action.sa_flags & SA_SIGINFO) != 0 ?
			reinterpret_cast<std::uintptr_t>(action.sa_sigaction) :
			reinterpret_cast<std::uintptr_t>(action.sa_handler);
		/* A handler is the host's or the kernel's; where the kernel's is does
		 * not matter, only that the same signals have one. */
		Dl_info info{};
		const char *where = handler == 0 ? "SIG_DFL" :
			handler == 1 ? "SIG_IGN" :
			dladdr(reinterpret_cast<void *>(handler), &info) != 0 ?
				"host" : "kernel";
		out << "sigaction " << signal << ' ' << where << " flags="
		    << action.sa_flags << " mask=" << action.sa_mask << '\n';
	}
	for (const int which : {ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF}) {
		itimerval timer{};
		(void)getitimer(which, &timer);
		out << "itimer " << which << " value=" << timer.it_value.tv_sec
		    << '.' << timer.it_value.tv_usec << " interval="
		    << timer.it_interval.tv_sec << '.' << timer.it_interval.tv_usec
		    << '\n';
	}
}

void write_process(std::ostream &out)
{
	/* The next key a pthread_key_create would hand out: a boot that leaves
	 * one behind moves it. */
	pthread_key_t key{};
	if (pthread_key_create(&key, nullptr) == 0) {
		out << "pthread_next_key " << key << '\n';
		(void)pthread_key_delete(key);
	}
	for (int descriptor = 0; descriptor <= 2; ++descriptor) {
		out << "stdio " << descriptor << " flags="
		    << (fcntl(descriptor, F_GETFL) & settable_flags) << '\n';
		termios terminal{};
		if (tcgetattr(descriptor, &terminal) == 0)
			out << "termios " << descriptor << " iflag=" << terminal.c_iflag
			    << " oflag=" << terminal.c_oflag << " cflag="
			    << terminal.c_cflag << " lflag=" << terminal.c_lflag << '\n';
	}
	char cwd[PATH_MAX] = {};
	out << "cwd " << (getcwd(cwd, sizeof(cwd)) != nullptr ? cwd : "?") << '\n';
	const mode_t mask = umask(022);
	umask(mask);
	out << "umask " << mask << '\n';
}

/* The bytes of dirty pages the process holds in regions of one VM tag. */
std::size_t dirty_bytes_of_tags(std::initializer_list<unsigned> tags)
{
	std::size_t bytes = 0;
	mach_vm_address_t address = 0;
	for (;;) {
		mach_vm_size_t size = 0;
		natural_t depth = 64;
		vm_region_submap_info_data_64_t info{};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
			    reinterpret_cast<vm_region_recurse_info_t>(&info),
			    &count) != KERN_SUCCESS)
			break;
		if (info.is_submap) {
			++depth;
			continue;
		}
		if (std::find(tags.begin(), tags.end(), info.user_tag) != tags.end())
			bytes += static_cast<std::size_t>(info.pages_dirtied +
				info.pages_swapped_out) * vm_page_size;
		address += size;
	}
	return bytes;
}

void write_memory(std::ostream &out)
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
		out << "phys_footprint " << info.phys_footprint << '\n';
	/* The default zone: what a boot allocated and freed is in use or it is
	 * not, whatever the allocator still holds. */
	malloc_statistics_t statistics{};
	malloc_zone_statistics(malloc_default_zone(), &statistics);
	out << "malloc_in_use " << statistics.size_in_use << '\n';
	out << "malloc_allocated " << statistics.size_allocated << '\n';
	/* The heaps the memory layer keeps outside the zone: what a domain's
	 * engine metadata holds while it lives, and nothing once its run is
	 * handed back, but the scratch chunk each thread that mapped pages keeps
	 * until it ends. */
	out << "heap_dirty " << dirty_bytes_of_tags({heap_tag}) << '\n';
	out << "scratch_dirty " << dirty_bytes_of_tags({scratch_tag}) << '\n';
}

/* The kernel's executable pages, in the native window, and how many of them
 * are dirty. The loader aliases them from the embedded kernel section, which
 * are clean file pages; a copy in anonymous memory is dirty. */
void write_kernel_text(std::ostream &out)
{
	std::size_t pages = 0, dirty = 0;
	mach_vm_address_t address = window_begin;
	while (address < window_end) {
		mach_vm_size_t size = 0;
		natural_t depth = 64;
		vm_region_submap_info_data_64_t info{};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
			    reinterpret_cast<vm_region_recurse_info_t>(&info),
			    &count) != KERN_SUCCESS)
			break;
		if (info.is_submap) {
			++depth;
			continue;
		}
		if (address >= window_end)
			break;
		if ((info.protection & VM_PROT_EXECUTE) != 0) {
			pages += size / vm_page_size;
			dirty += info.pages_dirtied;
		}
		address += size;
	}
	out << "kernel_text_pages " << pages << '\n'
	    << "kernel_text_dirty_pages " << dirty << '\n';
}

/* Writes `text` as `path`, atomically, after it is all composed: the file
 * being written is a descriptor of the process too. */
void publish(const std::filesystem::path &path, const std::string &text)
{
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream out(temporary);
		out << text;
	}
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
}

void write_dump(const std::filesystem::path &path, const std::string &label)
{
	std::ostringstream out;
	out << "label " << label << "\npid " << getpid() << '\n';
	write_memory(out);
	write_kernel_text(out);
	write_threads(out);
	write_ports(out);
	write_descriptors(out);
	write_signals(out);
	write_process(out);
	out << "end\n";
	publish(path, out.str());
}

/* Every page of the process that has a page behind it, read through
 * mach_vm_read_overwrite so that one that cannot be read is skipped rather
 * than faulted. The pattern and the buffer pages are read into are excluded. */
void write_scan(const std::filesystem::path &path, const std::string &label,
		const std::vector<unsigned char> &pattern)
{
	const auto page_size = static_cast<std::size_t>(vm_page_size);
	const auto own_begin = reinterpret_cast<std::uintptr_t>(pattern.data()) &
		~static_cast<std::uintptr_t>(page_size - 1U);
	const auto own_end = reinterpret_cast<std::uintptr_t>(pattern.data()) +
		pattern.size();
	std::vector<unsigned char> page(page_size);
	const auto page_begin = reinterpret_cast<std::uintptr_t>(page.data()) &
		~static_cast<std::uintptr_t>(page_size - 1U);
	const auto page_end = reinterpret_cast<std::uintptr_t>(page.data()) +
		page.size();
	std::size_t window_pages = 0, window_matches = 0;
	std::size_t host_pages = 0, host_matches = 0, unreadable = 0;
	std::ostringstream places;
	mach_vm_address_t address = 0;
	for (;;) {
		mach_vm_size_t size = 0;
		natural_t depth = 64;
		vm_region_submap_info_data_64_t info{};
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
			    reinterpret_cast<vm_region_recurse_info_t>(&info),
			    &count) != KERN_SUCCESS)
			break;
		if (info.is_submap) {
			++depth;
			continue;
		}
		const bool in_ram = address >= window_begin && address < window_end;
		if ((info.protection & VM_PROT_READ) != 0 &&
		    info.pages_resident + info.pages_swapped_out != 0) {
			const std::size_t pages = size / page_size;
			std::vector<char> present(pages);
			const bool known = mincore(reinterpret_cast<void *>(address),
				size, present.data()) == 0;
			for (std::size_t index = 0; index != pages; ++index) {
				if (!known || (present[index] &
					    (MINCORE_INCORE | MINCORE_PAGED_OUT)) == 0)
					continue;
				const mach_vm_address_t at = address + index * page_size;
				if ((at >= own_begin && at < own_end) ||
				    (at + page_size > page_begin && at < page_end))
					continue;
				mach_vm_size_t read = 0;
				if (mach_vm_read_overwrite(mach_task_self(), at, page_size,
					    reinterpret_cast<mach_vm_address_t>(page.data()),
					    &read) != KERN_SUCCESS || read != page_size) {
					++unreadable;
					continue;
				}
				++(in_ram ? window_pages : host_pages);
				if (memmem(page.data(), page.size(), pattern.data(),
					    pattern.size()) == nullptr)
					continue;
				++(in_ram ? window_matches : host_matches);
				if (!in_ram && host_matches <= 24U)
					places << "host_match address=0x" << std::hex << at
					       << std::dec << " region_size=" << size
					       << " tag=" << info.user_tag << " protection="
					       << info.protection << '\n';
			}
		}
		address += size;
	}
	std::ostringstream out;
	out << "scan " << label << " pattern_bytes=" << pattern.size() << '\n'
	    << "window_pages " << window_pages << '\n'
	    << "window_matches " << window_matches << '\n'
	    << "host_pages " << host_pages << '\n'
	    << "host_matches " << host_matches << '\n'
	    << "unreadable_pages " << unreadable << '\n'
	    << places.str() << "end\n";
	publish(path, out.str());
}

std::vector<unsigned char> decode_hex(const std::string &hex)
{
	std::vector<unsigned char> bytes;
	for (std::size_t index = 0; index + 1U < hex.size(); index += 2U)
		bytes.push_back(static_cast<unsigned char>(
			std::stoul(hex.substr(index, 2U), nullptr, 16)));
	return bytes;
}

void serve()
{
	pthread_setname_np(thread_name);
	const auto request = *directory / "request";
	for (;;) {
		std::error_code error;
		if (!std::filesystem::exists(request, error)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		std::string verb, label, hex;
		{
			std::ifstream in(request);
			in >> verb >> label >> hex;
		}
		std::filesystem::remove(request, error);
		if (label.empty())
			continue;
		try {
			if (verb == "dump")
				write_dump(*directory / (label + ".txt"), label);
			else if (verb == "scan")
				write_scan(*directory / (label + ".scan"), label,
					   decode_hex(hex));
		} catch (...) {
		}
	}
}

__attribute__((constructor)) void start()
{
	const char *const target = getprogname();
	const char *const where = std::getenv("UML_PROCESS_AUDIT_DIRECTORY");
	if (where == nullptr || target == nullptr ||
	    std::strncmp(target, "uml-darwin", 10) != 0)
		return;
	directory = new std::filesystem::path(where);
	hand_back_audit = std::getenv("UML_PROCESS_AUDIT_HAND_BACK") != nullptr;
	/* The launcher blocks the guest timer on the threads it makes, and so
	 * does this one: ITIMER_REAL must reach the designated CPU only. */
	sigset_t all{};
	sigfillset(&all);
	sigset_t previous{};
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	try {
		std::thread{serve}.detach();
	} catch (...) {
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

} // namespace

extern "C" {

/* The library announces the end of a hand-back with dprintf() on stderr; the
 * audit dumps the process at that call, on the thread that makes it. */
int uml_process_audit_dprintf(int descriptor, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int written = vdprintf(descriptor, format, arguments);
	va_end(arguments);
	if (descriptor == STDERR_FILENO && hand_back_audit.load() &&
	    format != nullptr &&
	    std::strncmp(format, hand_back_line.data(),
			 hand_back_line.size()) == 0) {
		try {
			write_dump(*directory / ("handback-" +
				std::to_string(++hand_backs) + ".txt"),
				"handback-" + std::to_string(hand_backs));
		} catch (...) {
		}
	}
	return written;
}

struct Interposer {
	const void *replacement;
	const void *replacee;
};

__attribute__((used, section("__DATA,__interpose")))
const Interposer interposers[] = {
	{reinterpret_cast<const void *>(uml_process_audit_dprintf),
	 reinterpret_cast<const void *>(dprintf)},
};

}

// SPDX-License-Identifier: GPL-2.0-or-later
// What a product's kernel configuration gives its guest, for the settings the
// Android CTS and the desktop disagree on. usage: kernel_config_probe
// [linux|android], linux by default.
//   both:    IPv4 multicast (/proc/net/igmp, a group join, a looped datagram),
//            no RDS socket family, no listener on TCP port 16385, the
//            stack protector and hardened usercopy in /proc/config.gz,
//            64 KiB virtually mapped task stacks with the end-of-stack
//            check, a macvlan link that rtnetlink creates over a dummy link, the
//            accept_ra_rt_table sysctl (0 until written; a prefix route
//            lands in the device's own table once it is -1000, where the
//            kernel has IPv6 multiple tables), an XFRM SPI allocated twice,
//            the Virtio vsock driver registered and no Virtio balloon driver,
//            and a vsock stream on the loopback CID that carries a byte
//   linux:   SysV IPC works, the NFS and SMB clients, XFS, Btrfs and SCTP are
//            present, a piped
//            core_pattern runs its helper, /sys/power has no wake_lock,
//            there is no 802.11 stack (no nl80211 family, no phy), and one
//            ESP SPI is refused for a second destination address
//   android: no SysV IPC (ENOSYS, no /proc/sysvipc), no NFS or SMB client, no
//            XFS or Btrfs, no SCTP,
//            /sys/power/wake_lock takes and releases a wakelock, the
//            static usermode helper runs no piped core_pattern helper, and
//            the Incremental file system mounts, serves a filled block and
//            fails a read of a missing block (the Linux guest has none), and
//            virtual Wi-Fi works: cfg80211's nl80211 family and a phy, and a
//            virt_wifi link over the macvlan that connects to the access point
//            VirtWifi through nl80211, raises its carrier and drops it again,
//            remains on a channel the radio did not always list (2412 MHz)
//            and sends an Action frame there, and passes a multicast group it
//            joins on to its macvlan; one ESP SPI is allocated for two
//            destination addresses
// Prints one line per check. The exit status is the failure count.
//
// The probe is also the usermode helper it tests: "kernel_config_probe --umh
// MARKER" drains its standard input and creates MARKER.
#include <arpa/inet.h>
#include <fcntl.h>
#include <ftw.h>
#include <ifaddrs.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/vm_sockets.h>
#include <linux/xfrm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void report(const char *name, bool ok, const std::string &detail)
{
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
	if (!ok)
		++failures;
}

std::string slurp(const char *path)
{
	std::ifstream in(path);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

std::string errno_text(int e)
{
	return std::to_string(e) + " " + std::strerror(e);
}

// A bare test shell has no network. Bring loopback up with its address, as a
// root shell can, so a guest without another interface can still join a group
// on it. A guest that has the address up already is left as it is.
void prepare_loopback()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return;
	ifreq request{};
	std::strncpy(request.ifr_name, "lo", sizeof request.ifr_name - 1);
	if (ioctl(fd, SIOCGIFFLAGS, &request) == 0 && (request.ifr_flags & IFF_UP) == 0) {
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		std::memcpy(&request.ifr_addr, &address, sizeof address);
		ioctl(fd, SIOCSIFADDR, &request);
		address.sin_addr.s_addr = htonl(0xff000000U);
		std::memcpy(&request.ifr_netmask, &address, sizeof address);
		ioctl(fd, SIOCSIFNETMASK, &request);
		ioctl(fd, SIOCGIFFLAGS, &request);
		request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
		ioctl(fd, SIOCSIFFLAGS, &request);
	}
	close(fd);
}

// The chosen interface: first up, non-loopback, multicast-capable IPv4
// interface; else loopback.
bool pick_interface(in_addr &address, std::string &name)
{
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return false;
	bool found = false;
	for (ifaddrs *i = list; i != nullptr && !found; i = i->ifa_next) {
		if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET)
			continue;
		unsigned flags = i->ifa_flags;
		if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0 ||
		    (flags & IFF_MULTICAST) == 0)
			continue;
		address = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr;
		name = i->ifa_name;
		found = true;
	}
	if (!found) {
		address.s_addr = htonl(INADDR_LOOPBACK);
		name = "lo";
	}
	freeifaddrs(list);
	return true;
}

void check_multicast()
{
	std::string igmp = slurp("/proc/net/igmp");
	report("igmp-proc", access("/proc/net/igmp", F_OK) == 0 && !igmp.empty(),
	       "/proc/net/igmp is " + std::string(igmp.empty() ? "missing" : "present"));

	prepare_loopback();
	in_addr local{};
	std::string ifname;
	if (!pick_interface(local, ifname)) {
		report("multicast-iface", false, "getifaddrs failed");
		return;
	}
	char local_text[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &local, local_text, sizeof local_text);
	std::printf("INFO interface %s %s\n", ifname.c_str(), local_text);

	int rx = socket(AF_INET, SOCK_DGRAM, 0);
	int tx = socket(AF_INET, SOCK_DGRAM, 0);
	if (rx < 0 || tx < 0) {
		report("multicast-socket", false, errno_text(errno));
		return;
	}
	int yes = 1;
	setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	sockaddr_in bind_address{};
	bind_address.sin_family = AF_INET;
	bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_address.sin_port = htons(40123);
	if (bind(rx, reinterpret_cast<sockaddr *>(&bind_address), sizeof bind_address) != 0) {
		report("multicast-bind", false, errno_text(errno));
		return;
	}
	ip_mreq membership{};
	inet_pton(AF_INET, "239.255.42.99", &membership.imr_multiaddr);
	membership.imr_interface = local;
	int joined = setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership);
	report("multicast-join", joined == 0,
	       joined == 0 ? "joined 239.255.42.99" : errno_text(errno));
	if (joined != 0)
		return;

	// The group shows in /proc/net/igmp as the address in network byte order,
	// printed as a host-order hex word: 239.255.42.99 is 632AFFEF on a
	// little-endian host.
	igmp = slurp("/proc/net/igmp");
	report("multicast-listed", igmp.find("632AFFEF") != std::string::npos,
	       "group 239.255.42.99 in /proc/net/igmp");

	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof local);
	unsigned char loop = 1;
	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
	unsigned char ttl = 1;
	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	sockaddr_in group{};
	group.sin_family = AF_INET;
	group.sin_addr = membership.imr_multiaddr;
	group.sin_port = htons(40123);
	const char payload[] = "kconfig-multicast";
	ssize_t sent = sendto(tx, payload, sizeof payload, 0,
			      reinterpret_cast<sockaddr *>(&group), sizeof group);
	if (sent != static_cast<ssize_t>(sizeof payload)) {
		report("multicast-send", false, errno_text(errno));
		return;
	}
	timeval timeout{3, 0};
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	char buffer[64] = {};
	ssize_t got = recv(rx, buffer, sizeof buffer, 0);
	report("multicast-loop", got == static_cast<ssize_t>(sizeof payload) &&
					 std::strcmp(buffer, payload) == 0,
	       got < 0 ? errno_text(errno) : std::string("received ") + buffer);
	setsockopt(rx, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership, sizeof membership);
	close(rx);
	close(tx);
}

// A TCP socket in the LISTEN state (0A) on port 16385 (0x4001), in either
// family's table.
void check_no_rds_listener()
{
	bool found = false;
	for (const char *path : {"/proc/net/tcp", "/proc/net/tcp6"}) {
		std::istringstream lines(slurp(path));
		std::string line;
		while (std::getline(lines, line)) {
			std::istringstream fields(line);
			std::string index, local, remote, state;
			fields >> index >> local >> remote >> state;
			size_t colon = local.rfind(':');
			if (colon == std::string::npos)
				continue;
			if (state == "0A" && local.substr(colon + 1) == "4001") {
				found = true;
				std::printf("INFO listener %s %s\n", path, local.c_str());
			}
		}
	}
	report("no-port-16385-listener", !found, found ? "listening" : "none");
}

// Whether /proc/net/protocols lists a protocol: a policy can refuse a socket
// of a family the kernel has, so the error alone does not show its absence.
bool protocol_registered(const char *name)
{
	std::istringstream lines(slurp("/proc/net/protocols"));
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream fields(line);
		std::string first;
		fields >> first;
		if (first == name)
			return true;
	}
	return false;
}

void check_rds()
{
	int fd = socket(AF_RDS, SOCK_SEQPACKET, 0);
	int e = errno;
	if (fd >= 0)
		close(fd);
	bool registered = protocol_registered("RDS");
	report("no-rds", fd < 0 && !registered && e != 0,
	       std::string(registered ? "RDS registered, " : "RDS not registered, ") +
		       (fd >= 0 ? std::string("AF_RDS socket created") : errno_text(e)));
}

bool sysv_ok(long result, int &e)
{
	e = errno;
	return result >= 0;
}

void check_sysv(bool wanted)
{
	int e = 0;
	long msg = syscall(SYS_msgget, IPC_PRIVATE, IPC_CREAT | 0600);
	bool msg_ok = sysv_ok(msg, e);
	int msg_errno = e;
	if (msg_ok)
		syscall(SYS_msgctl, static_cast<int>(msg), IPC_RMID, nullptr);
	long sem = syscall(SYS_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
	bool sem_ok = sysv_ok(sem, e);
	int sem_errno = e;
	if (sem_ok)
		syscall(SYS_semctl, static_cast<int>(sem), 0, IPC_RMID, 0);
	long shm = syscall(SYS_shmget, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	bool shm_ok = sysv_ok(shm, e);
	int shm_errno = e;
	if (shm_ok)
		syscall(SYS_shmctl, static_cast<int>(shm), IPC_RMID, nullptr);
	bool proc = access("/proc/sysvipc", F_OK) == 0;
	std::string detail = "msgget " + (msg_ok ? std::string("ok") : errno_text(msg_errno)) +
			     ", semget " + (sem_ok ? std::string("ok") : errno_text(sem_errno)) +
			     ", shmget " + (shm_ok ? std::string("ok") : errno_text(shm_errno)) +
			     ", /proc/sysvipc " + (proc ? "present" : "absent");
	if (wanted) {
		report("sysv-ipc", msg_ok && sem_ok && shm_ok && proc, detail);
	} else {
		report("no-sysv-ipc", !msg_ok && msg_errno == ENOSYS && !sem_ok &&
					      sem_errno == ENOSYS && !shm_ok &&
					      shm_errno == ENOSYS && !proc,
		       detail);
	}
}

void check_nfs(bool wanted)
{
	std::string filesystems = slurp("/proc/filesystems");
	bool nfs = filesystems.find("\tnfs\n") != std::string::npos;
	bool nfs4 = filesystems.find("\tnfs4\n") != std::string::npos;
	std::string detail = std::string("nfs ") + (nfs ? "present" : "absent") + ", nfs4 " +
			     (nfs4 ? "present" : "absent");
	if (wanted)
		report("nfs-client", nfs && nfs4, detail);
	else
		report("no-nfs-client", !nfs && !nfs4, detail);
}

// XFS, Btrfs and the SMB client: the desktop product keeps them, Android
// mounts none of them and leaves them out.
void check_other_filesystems(bool wanted)
{
	std::string filesystems = slurp("/proc/filesystems");
	bool xfs = filesystems.find("\txfs\n") != std::string::npos;
	bool btrfs = filesystems.find("\tbtrfs\n") != std::string::npos;
	bool smb = filesystems.find("\tcifs\n") != std::string::npos ||
		   filesystems.find("\tsmb3\n") != std::string::npos;
	std::string detail = std::string("xfs ") + (xfs ? "present" : "absent") + ", btrfs " +
			     (btrfs ? "present" : "absent") + ", smb client " +
			     (smb ? "present" : "absent");
	if (wanted)
		report("xfs-btrfs-smb", xfs && btrfs && smb, detail);
	else
		report("no-xfs-btrfs-smb", !xfs && !btrfs && !smb, detail);
}

void check_sctp(bool wanted)
{
	int fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
	int e = errno;
	if (fd >= 0)
		close(fd);
	bool registered = protocol_registered("SCTP");
	std::string detail = std::string(registered ? "SCTP registered, " : "SCTP not registered, ") +
			     (fd >= 0 ? std::string("socket created") : errno_text(e));
	if (wanted)
		report("sctp", fd >= 0 && registered, detail);
	else
		report("no-sctp", fd < 0 && !registered, detail);
}

bool write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	ssize_t length = static_cast<ssize_t>(std::strlen(text));
	bool ok = write(fd, text, static_cast<size_t>(length)) == length;
	close(fd);
	return ok;
}

// Android's SystemSuspend ends a suspend that is under way by writing
// "userspace-abort" to /sys/power/wake_lock, and Sxmo suspends a guest on its
// own whenever the file exists. /sys/power/state must exist for the absence
// to mean anything.
void check_wake_lock(bool wanted)
{
	bool power = access("/sys/power/state", F_OK) == 0;
	bool present = access("/sys/power/wake_lock", F_OK) == 0;
	if (!wanted) {
		report("no-wake-lock", power && !present,
		       std::string("/sys/power/state ") + (power ? "present" : "absent") +
			       ", /sys/power/wake_lock " + (present ? "present" : "absent"));
		return;
	}
	const char *name = "kconfig-probe";
	bool taken = present && write_text("/sys/power/wake_lock", name);
	bool listed = taken && slurp("/sys/power/wake_lock").find(name) != std::string::npos;
	bool released = taken && write_text("/sys/power/wake_unlock", name) &&
			slurp("/sys/power/wake_lock").find(name) == std::string::npos;
	report("wake-lock", power && listed && released,
	       std::string("wake_lock ") + (present ? "present" : "absent") + ", taken " +
		       (taken ? "yes" : "no") + ", listed " + (listed ? "yes" : "no") +
		       ", released " + (released ? "yes" : "no"));
}

// The resolved kernel configuration, as /proc/config.gz holds it. zcat is run
// without a shell, which an Android guest does not have at /bin/sh.
std::string kernel_config()
{
	int fds[2];
	if (pipe(fds) != 0)
		return {};
	pid_t child = fork();
	if (child < 0) {
		close(fds[0]);
		close(fds[1]);
		return {};
	}
	if (child == 0) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, 2);
		execlp("zcat", "zcat", "/proc/config.gz", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	std::string text;
	char chunk[4096];
	ssize_t got;
	while ((got = read(fds[0], chunk, sizeof chunk)) > 0)
		text.append(chunk, static_cast<size_t>(got));
	close(fds[0]);
	int status = 0;
	waitpid(child, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? text : std::string();
}

bool config_has(const std::string &config, const std::string &line)
{
	return config.find("\n" + line + "\n") != std::string::npos ||
	       config.compare(0, line.size() + 1, line + "\n") == 0;
}

// The kernel hardening the Android CTS requires: the strong stack protector,
// hardened usercopy, and the static usermode helper with an empty path. The
// Linux product keeps its distribution's usermode helpers (systemd-coredump,
// request-key, bridge-stp), so its helper path stays free.
void check_hardening_config(bool android)
{
	std::string config = kernel_config();
	if (config.empty()) {
		report("hardening-config", false, "/proc/config.gz is unreadable");
		return;
	}
	bool protector = config_has(config, "CONFIG_STACKPROTECTOR_STRONG=y");
	bool usercopy = config_has(config, "CONFIG_HARDENED_USERCOPY=y");
	bool helper = config_has(config, "CONFIG_STATIC_USERMODEHELPER=y");
	bool empty_path = config_has(config, "CONFIG_STATIC_USERMODEHELPER_PATH=\"\"");
	std::string detail = std::string("strong stack protector ") + (protector ? "on" : "off") +
			     ", hardened usercopy " + (usercopy ? "on" : "off") +
			     ", static usermode helper " + (helper ? "on" : "off") +
			     (helper ? (empty_path ? " with an empty path" : " with a path") : "");
	report("hardening-config",
	       protector && usercopy && (android ? helper && empty_path : !helper), detail);
}

// Every task's kernel stack is 64 KiB of virtually mapped pages, so an overrun
// reaches an unmapped guard group, and the scheduler checks the stack's last
// word. Overflow behaviour itself is documented in the kernel README.
void check_stack_config()
{
	std::string config = kernel_config();
	if (config.empty()) {
		report("stack-config", false, "/proc/config.gz is unreadable");
		return;
	}
	bool order = config_has(config, "CONFIG_KERNEL_STACK_ORDER=4");
	bool vmap = config_has(config, "CONFIG_VMAP_STACK=y");
	bool end_check = config_has(config, "CONFIG_SCHED_STACK_END_CHECK=y");
	std::string detail = std::string("stack order 4 ") + (order ? "yes" : "no") + ", vmapped " +
			     (vmap ? "yes" : "no") + ", end-of-stack check " +
			     (end_check ? "yes" : "no");
	report("stack-config", order && vmap && end_check, detail);
}

// The Incremental file system of the Android common kernel, whose only user is
// Android's package manager. The interface below is the subset of
// <linux/incrementalfs.h> the check needs; the Linux guest's headers lack it.
struct incfs_uuid {
	unsigned char bytes[16];
} __attribute__((aligned(8)));

struct incfs_new_file_args {
	incfs_uuid file_id;
	unsigned long long size;
	unsigned short mode;
	unsigned short reserved1;
	unsigned int reserved2;
	unsigned long long directory_path;
	unsigned long long file_name;
	unsigned long long file_attr;
	unsigned int file_attr_len;
	unsigned int reserved4;
	unsigned long long signature_info;
	unsigned long long signature_size;
	unsigned long long reserved6;
};

struct incfs_fill_block {
	unsigned int block_index;
	unsigned int data_len;
	unsigned long long data;
	unsigned char compression;
	unsigned char flags;
	unsigned short reserved1;
	unsigned int reserved2;
	unsigned long long reserved3;
};

struct incfs_fill_blocks {
	unsigned long long count;
	unsigned long long fill_blocks;
};

struct incfs_permit_fill {
	unsigned int file_descriptor;
};

constexpr unsigned long incfs_ioc_create_file = _IOWR('g', 30, incfs_new_file_args);
constexpr unsigned long incfs_ioc_fill_blocks = _IOR('g', 32, incfs_fill_blocks);
constexpr unsigned long incfs_ioc_permit_fill = _IOW('g', 33, incfs_permit_fill);

int remove_entry(const char *path, const struct stat *, int, struct FTW *)
{
	return remove(path);
}

void check_incfs(bool android)
{
	bool listed = slurp("/proc/filesystems").find("\tincremental-fs\n") != std::string::npos;
	if (!android) {
		report("no-incfs", !listed,
		       std::string("incremental-fs ") + (listed ? "present" : "absent"));
		return;
	}
	report("incfs-listed", listed, std::string("incremental-fs ") + (listed ? "present" : "absent"));
	if (!listed)
		return;

	// Android's IncFs library reads these before it enables the feature.
	std::string features;
	bool supported = true;
	for (const char *name : {"corefs", "v2", "zstd"}) {
		std::string value =
			slurp((std::string("/sys/fs/incremental-fs/features/") + name).c_str());
		supported = supported && value == "supported\n";
		features += std::string(features.empty() ? "" : ", ") + name + " " +
			    (value == "supported\n" ? "supported" : "missing");
	}
	report("incfs-features", supported, features);

	std::string root = "/data/local/tmp/kconfig-incfs-" + std::to_string(getpid());
	std::string backing = root + "/backing";
	std::string mount_point = root + "/mnt";
	if (mkdir(root.c_str(), 0700) != 0 || mkdir(backing.c_str(), 0700) != 0 ||
	    mkdir(mount_point.c_str(), 0700) != 0) {
		report("incfs-mount", false, "cannot create " + root + ": " + errno_text(errno));
		nftw(root.c_str(), remove_entry, 8, FTW_DEPTH | FTW_PHYS);
		return;
	}
	int mounted = mount(backing.c_str(), mount_point.c_str(), "incremental-fs",
			    MS_NOSUID | MS_NODEV | MS_NOATIME,
			    "read_timeout_ms=1000,readahead=0,rlog_pages=0,rlog_wakeup_cnt=1");
	report("incfs-mount", mounted == 0,
	       mounted == 0 ? "mounted " + mount_point : errno_text(errno));
	if (mounted != 0) {
		nftw(root.c_str(), remove_entry, 8, FTW_DEPTH | FTW_PHYS);
		return;
	}

	// One file of two blocks, filled with a pattern, and one of a block that
	// is never filled.
	int control = open((mount_point + "/.pending_reads").c_str(), O_RDONLY | O_CLOEXEC);
	const char *filled_name = "filled";
	const char *missing_name = "missing";
	incfs_new_file_args filled{};
	filled.file_id.bytes[0] = 1;
	filled.size = 8192;
	filled.mode = S_IFREG | 0555;
	// No directory path puts the file in the root of the mount.
	filled.file_name = reinterpret_cast<unsigned long long>(filled_name);
	incfs_new_file_args missing = filled;
	missing.file_id.bytes[0] = 2;
	missing.size = 4096;
	missing.file_name = reinterpret_cast<unsigned long long>(missing_name);
	int created_filled = control >= 0 ? ioctl(control, incfs_ioc_create_file, &filled) : -1;
	int created_filled_errno = errno;
	int created_missing = control >= 0 ? ioctl(control, incfs_ioc_create_file, &missing) : -1;
	report("incfs-create", created_filled == 0 && created_missing == 0,
	       control < 0 ? "cannot open .pending_reads: " + errno_text(errno)
			   : (created_filled == 0 && created_missing == 0
				      ? std::string("created two files")
				      : errno_text(created_filled != 0 ? created_filled_errno : errno)));

	std::string filled_path = mount_point + "/filled";
	int file = open(filled_path.c_str(), O_RDONLY | O_CLOEXEC);
	char pattern[8192];
	for (size_t i = 0; i < sizeof pattern; ++i)
		pattern[i] = static_cast<char>((i * 7 + i / 4096) & 0xff);
	long long filled_blocks = -1;
	if (created_filled == 0 && file >= 0) {
		incfs_permit_fill permit{static_cast<unsigned int>(file)};
		incfs_fill_block blocks[2] = {};
		for (unsigned i = 0; i < 2; ++i) {
			blocks[i].block_index = i;
			blocks[i].data_len = 4096;
			blocks[i].data = reinterpret_cast<unsigned long long>(pattern + 4096 * i);
		}
		incfs_fill_blocks fill{2, reinterpret_cast<unsigned long long>(blocks)};
		filled_blocks = ioctl(control, incfs_ioc_permit_fill, &permit) != 0
					? -1
					: ioctl(file, incfs_ioc_fill_blocks, &fill);
	}
	int fill_errno = errno;
	report("incfs-fill", filled_blocks == 2,
	       filled_blocks == 2 ? std::string("filled 2 blocks") : errno_text(fill_errno));

	char readback[8192] = {};
	ssize_t got = file >= 0 ? pread(file, readback, sizeof readback, 0) : -1;
	int read_errno = errno;
	report("incfs-read", got == static_cast<ssize_t>(sizeof readback) &&
				     std::memcmp(readback, pattern, sizeof pattern) == 0,
	       got < 0 ? errno_text(read_errno) : "read " + std::to_string(got) + " bytes");

	// A block that was never sent fails the read after read_timeout_ms, and
	// leaves the page uncached so that the next read asks again.
	int hole = open((mount_point + "/missing").c_str(), O_RDONLY | O_CLOEXEC);
	timeval before{};
	timeval after{};
	gettimeofday(&before, nullptr);
	char block[4096];
	ssize_t hole_got = hole >= 0 ? pread(hole, block, sizeof block, 0) : -1;
	int hole_errno = errno;
	gettimeofday(&after, nullptr);
	long elapsed_ms = static_cast<long>((after.tv_sec - before.tv_sec) * 1000 +
					    (after.tv_usec - before.tv_usec) / 1000);
	report("incfs-timeout", hole_got < 0 && elapsed_ms < 10000,
	       hole_got < 0 ? "read failed with " + errno_text(hole_errno) + " after " +
				      std::to_string(elapsed_ms) + " ms"
			    : "read returned " + std::to_string(hole_got) + " bytes");

	if (hole >= 0)
		close(hole);
	if (file >= 0)
		close(file);
	if (control >= 0)
		close(control);
	int unmounted = umount(mount_point.c_str());
	report("incfs-umount", unmounted == 0, unmounted == 0 ? "unmounted" : errno_text(errno));
	nftw(root.c_str(), remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

// A piped core_pattern runs the probe itself as the helper (--umh), so the
// result needs no shell or other file. A static usermode helper with an empty
// path starts no helper at all, whatever core_pattern names, and the kernel
// says the dump is disabled.
void check_usermode_helper(bool android)
{
	char self[4096];
	ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
	if (length <= 0) {
		report("usermode-helper", false, "cannot find the probe binary");
		return;
	}
	self[length] = '\0';
	std::string marker = std::string(android ? "/data/local/tmp" : "/tmp") +
			     "/kconfig-umh-marker";
	unlink(marker.c_str());
	std::string saved = slurp("/proc/sys/kernel/core_pattern");
	while (!saved.empty() && saved.back() == '\n')
		saved.pop_back();
	std::string pattern = std::string("|") + self + " --umh " + marker;
	if (!write_text("/proc/sys/kernel/core_pattern", pattern.c_str())) {
		int e = errno;
		// An unprivileged shell cannot set it; a root one must.
		report("usermode-helper", geteuid() != 0,
		       std::string("core_pattern is not writable: ") + errno_text(e) +
			       (geteuid() != 0 ? " (not root, skipped)" : ""));
		return;
	}
	pid_t child = fork();
	if (child == 0) {
		rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
		setrlimit(RLIMIT_CORE, &unlimited);
		prctl(PR_SET_DUMPABLE, 1);
		signal(SIGSEGV, SIG_DFL);
		kill(getpid(), SIGSEGV);
		_exit(0);
	}
	int status = 0;
	if (child > 0)
		waitpid(child, &status, 0);
	bool dumped = child > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
	// The kernel reaps the task once the dump ends, but the helper is a
	// separate process that may still be creating the marker.
	bool ran = false;
	for (int i = 0; i < 50 && !ran; ++i) {
		ran = access(marker.c_str(), F_OK) == 0;
		if (!ran)
			usleep(100000);
	}
	unlink(marker.c_str());
	write_text("/proc/sys/kernel/core_pattern", saved.c_str());
	std::string detail = std::string("the child ") + (dumped ? "crashed" : "did not crash") +
			     ", the helper " + (ran ? "ran" : "did not run");
	if (android)
		report("static-usermode-helper", dumped && !ran, detail);
	else
		report("usermode-helper", dumped && ran, detail);
}

// A request for one of the kernel's netlink interfaces, built the way iproute2
// builds one: the header, a fixed body, then attributes that may nest.
class NetlinkRequest {
public:
	NetlinkRequest(unsigned short type, unsigned short flags, const void *body,
		       size_t body_size)
	{
		std::memset(buffer_, 0, sizeof buffer_);
		nlmsghdr *header = reinterpret_cast<nlmsghdr *>(buffer_);
		header->nlmsg_type = type;
		header->nlmsg_flags = static_cast<unsigned short>(flags | NLM_F_REQUEST);
		header->nlmsg_seq = 1;
		length_ = NLMSG_HDRLEN;
		std::memcpy(buffer_ + length_, body, body_size);
		length_ += NLMSG_ALIGN(body_size);
	}

	void put(unsigned short type, const void *data, size_t size)
	{
		nlattr *attribute = reinterpret_cast<nlattr *>(buffer_ + length_);
		attribute->nla_type = type;
		attribute->nla_len = static_cast<unsigned short>(NLA_HDRLEN + size);
		if (size != 0)
			std::memcpy(buffer_ + length_ + NLA_HDRLEN, data, size);
		length_ += NLA_ALIGN(NLA_HDRLEN + size);
	}

	void put_string(unsigned short type, const char *text)
	{
		put(type, text, std::strlen(text) + 1);
	}

	void put_u32(unsigned short type, unsigned int value) { put(type, &value, sizeof value); }

	// The offset to hand to close() after the nested attributes.
	size_t nest(unsigned short type)
	{
		size_t at = length_;
		put(static_cast<unsigned short>(type | NLA_F_NESTED), nullptr, 0);
		return at;
	}

	void close(size_t at)
	{
		reinterpret_cast<nlattr *>(buffer_ + at)->nla_len =
			static_cast<unsigned short>(length_ - at);
	}

	const char *data()
	{
		reinterpret_cast<nlmsghdr *>(buffer_)->nlmsg_len = static_cast<unsigned int>(length_);
		return buffer_;
	}

	size_t size() const { return length_; }

private:
	alignas(8) char buffer_[1024];
	size_t length_ = 0;
};

// Sends the request on a netlink socket and returns the errno of the
// acknowledgement (0 for success) or of the failing call. A reply that is not
// an acknowledgement is handed back in *reply.
int netlink_call(int protocol, NetlinkRequest &request, std::string *reply = nullptr)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
	if (fd < 0)
		return errno;
	timeval timeout{5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	sockaddr_nl kernel{};
	kernel.nl_family = AF_NETLINK;
	int result = 0;
	const char *message = request.data();
	if (sendto(fd, message, request.size(), 0, reinterpret_cast<sockaddr *>(&kernel),
		   sizeof kernel) < 0) {
		result = errno;
	} else {
		alignas(8) char buffer[8192];
		ssize_t got = recv(fd, buffer, sizeof buffer, 0);
		const nlmsghdr *header = reinterpret_cast<const nlmsghdr *>(buffer);
		if (got < 0)
			result = errno;
		else if (static_cast<size_t>(got) < NLMSG_HDRLEN ||
			 header->nlmsg_len > static_cast<size_t>(got))
			result = EBADMSG;
		else if (header->nlmsg_type == NLMSG_ERROR)
			result = -reinterpret_cast<const nlmsgerr *>(NLMSG_DATA(header))->error;
		else if (reply != nullptr)
			reply->assign(buffer, static_cast<size_t>(got));
	}
	close(fd);
	return result;
}

// The id of a generic netlink family, or minus the errno that refused it:
// ENOENT for a family the kernel does not have.
int generic_family(const char *name)
{
	genlmsghdr body{};
	body.cmd = CTRL_CMD_GETFAMILY;
	body.version = 1;
	NetlinkRequest request(GENL_ID_CTRL, 0, &body, sizeof body);
	request.put_string(CTRL_ATTR_FAMILY_NAME, name);
	std::string reply;
	int error = netlink_call(NETLINK_GENERIC, request, &reply);
	if (error != 0)
		return -error;
	for (size_t at = NLMSG_HDRLEN + GENL_HDRLEN; at + NLA_HDRLEN <= reply.size();) {
		nlattr attribute;
		std::memcpy(&attribute, reply.data() + at, sizeof attribute);
		if (attribute.nla_len < NLA_HDRLEN || at + attribute.nla_len > reply.size())
			break;
		if ((attribute.nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID &&
		    attribute.nla_len >= NLA_HDRLEN + sizeof(unsigned short)) {
			unsigned short id;
			std::memcpy(&id, reply.data() + at + NLA_HDRLEN, sizeof id);
			return id;
		}
		at += NLA_ALIGN(attribute.nla_len);
	}
	return -EBADMSG;
}

// A link of the given kind, over another one when lower is not zero, with a
// fixed address when mac is not null. A macvlan is private, as the product's
// Wi-Fi interface needs it.
int add_link(const char *name, const char *kind, unsigned lower, const unsigned char *mac)
{
	ifinfomsg body{};
	NetlinkRequest request(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK, &body,
			       sizeof body);
	request.put_string(IFLA_IFNAME, name);
	if (lower != 0)
		request.put_u32(IFLA_LINK, lower);
	if (mac != nullptr)
		request.put(IFLA_ADDRESS, mac, 6);
	size_t info = request.nest(IFLA_LINKINFO);
	request.put_string(IFLA_INFO_KIND, kind);
	if (std::strcmp(kind, "macvlan") == 0) {
		size_t data = request.nest(IFLA_INFO_DATA);
		request.put_u32(IFLA_MACVLAN_MODE, MACVLAN_MODE_PRIVATE);
		request.close(data);
	}
	request.close(info);
	return netlink_call(NETLINK_ROUTE, request);
}

int set_link_up(const char *name)
{
	ifinfomsg body{};
	body.ifi_index = static_cast<int>(if_nametoindex(name));
	body.ifi_flags = IFF_UP;
	body.ifi_change = IFF_UP;
	NetlinkRequest request(RTM_NEWLINK, NLM_F_ACK, &body, sizeof body);
	return netlink_call(NETLINK_ROUTE, request);
}

// Removing a link removes the links stacked on it.
int delete_link(const char *name)
{
	ifinfomsg body{};
	NetlinkRequest request(RTM_DELLINK, NLM_F_ACK, &body, sizeof body);
	request.put_string(IFLA_IFNAME, name);
	return netlink_call(NETLINK_ROUTE, request);
}

// NL80211_CMD_CONNECT and NL80211_CMD_DISCONNECT with the attributes they
// need, from <linux/nl80211.h>, which not every build host has.
constexpr unsigned char nl80211_cmd_connect = 46;
constexpr unsigned char nl80211_cmd_disconnect = 48;
constexpr unsigned short nl80211_attr_ifindex = 3;
constexpr unsigned short nl80211_attr_ssid = 52;
constexpr unsigned short nl80211_attr_reason_code = 54;

int nl80211_command(int family, unsigned char command, unsigned ifindex, const char *ssid)
{
	genlmsghdr body{};
	body.cmd = command;
	body.version = 1;
	NetlinkRequest request(static_cast<unsigned short>(family), NLM_F_ACK, &body, sizeof body);
	request.put_u32(nl80211_attr_ifindex, ifindex);
	if (command == nl80211_cmd_connect) {
		request.put(nl80211_attr_ssid, ssid, std::strlen(ssid));
	} else {
		unsigned short leaving = 3; // WLAN_REASON_DEAUTH_LEAVING
		request.put(nl80211_attr_reason_code, &leaving, sizeof leaving);
	}
	return netlink_call(NETLINK_GENERIC, request);
}

// The carrier of a link, polled until it is the wanted value or the time is
// up; virt_wifi connects two seconds after it is asked to.
bool wait_for_carrier(const char *name, char wanted, int seconds)
{
	std::string path = std::string("/sys/class/net/") + name + "/carrier";
	for (int i = 0; i < seconds * 10; ++i) {
		std::string value = slurp(path.c_str());
		if (!value.empty() && value[0] == wanted)
			return true;
		usleep(100000);
	}
	return false;
}

// Links the checks create, removed before and after: the dummy link carries
// the others, so deleting it removes all three.
constexpr const char *probe_lower = "kcfgd0";
constexpr const char *probe_macvlan = "kcfgm0";
constexpr const char *probe_wifi = "kcfgw0";

// The generic netlink commands and attributes of off-channel operation, from
// <linux/nl80211.h>, which not every build host has.
constexpr unsigned char nl80211_cmd_remain_on_channel = 55;
constexpr unsigned char nl80211_cmd_cancel_remain_on_channel = 56;
constexpr unsigned char nl80211_cmd_frame = 59;
constexpr unsigned short nl80211_attr_wiphy_freq = 38;
constexpr unsigned short nl80211_attr_frame = 51;
constexpr unsigned short nl80211_attr_duration = 87;
constexpr unsigned short nl80211_attr_cookie = 88;
constexpr unsigned short nl80211_attr_offchannel_tx_ok = 108;

bool reply_cookie(const std::string &reply, unsigned long long &cookie)
{
	for (size_t at = NLMSG_HDRLEN + GENL_HDRLEN; at + NLA_HDRLEN <= reply.size();) {
		nlattr attribute;
		std::memcpy(&attribute, reply.data() + at, sizeof attribute);
		if (attribute.nla_len < NLA_HDRLEN || at + attribute.nla_len > reply.size())
			break;
		if ((attribute.nla_type & NLA_TYPE_MASK) == nl80211_attr_cookie &&
		    attribute.nla_len >= NLA_HDRLEN + sizeof cookie) {
			std::memcpy(&cookie, reply.data() + at + NLA_HDRLEN, sizeof cookie);
			return true;
		}
		at += NLA_ALIGN(attribute.nla_len);
	}
	return false;
}

// Remain on a channel for a time, or send an Action frame there, and return
// the errno; the cookie that names the request is left in *cookie.
int nl80211_off_channel(int family, unsigned char command, unsigned ifindex, unsigned freq,
			unsigned long long *cookie)
{
	genlmsghdr body{};
	body.cmd = command;
	body.version = 1;
	NetlinkRequest request(static_cast<unsigned short>(family), NLM_F_ACK, &body, sizeof body);
	request.put_u32(nl80211_attr_ifindex, ifindex);
	if (command == nl80211_cmd_cancel_remain_on_channel) {
		request.put(nl80211_attr_cookie, cookie, sizeof *cookie);
		return netlink_call(NETLINK_GENERIC, request);
	}
	request.put_u32(nl80211_attr_wiphy_freq, freq);
	request.put_u32(nl80211_attr_duration, 2000);
	if (command == nl80211_cmd_frame) {
		// A public Action frame (vendor specific, WFA's OUI and the DPP type) to
		// nobody, from the interface's own address.
		unsigned char frame[24 + 8] = {0xd0, 0x00};
		std::memset(frame + 4, 0xff, 6);
		unsigned char mac[6] = {};
		std::string address = slurp((std::string("/sys/class/net/") + probe_wifi + "/address").c_str());
		unsigned int parts[6] = {};
		if (std::sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &parts[0], &parts[1], &parts[2],
				&parts[3], &parts[4], &parts[5]) == 6)
			for (int i = 0; i < 6; ++i)
				mac[i] = static_cast<unsigned char>(parts[i]);
		std::memcpy(frame + 10, mac, 6);
		std::memset(frame + 16, 0xff, 6);
		const unsigned char action[8] = {0x04, 0x09, 0x50, 0x6f, 0x9a, 0x1a, 0x00, 0x00};
		std::memcpy(frame + 24, action, sizeof action);
		request.put(nl80211_attr_frame, frame, sizeof frame);
		request.put(nl80211_attr_offchannel_tx_ok, nullptr, 0);
	}
	std::string reply;
	int error = netlink_call(NETLINK_GENERIC, request, &reply);
	if (error == 0 && !reply_cookie(reply, *cookie))
		return EBADMSG;
	return error;
}

// The radio of a phone's Wi-Fi chip, so that a supplicant can listen and send
// where a peer was told to be, though nobody answers. 2412 MHz was not one of
// the radio's two channels (2432 and 5240), and the request is refused for a
// channel it does not list.
void check_wifi_off_channel(int family, unsigned index)
{
	unsigned long long cookie = 0;
	int roc = nl80211_off_channel(family, nl80211_cmd_remain_on_channel, index, 2412, &cookie);
	report("virt-wifi-remain-on-channel", roc == 0 && cookie != 0,
	       roc != 0 ? errno_text(roc) : "remaining on 2412 MHz, cookie " + std::to_string(cookie));
	if (roc == 0) {
		int cancelled = nl80211_off_channel(family, nl80211_cmd_cancel_remain_on_channel, index, 0, &cookie);
		int again = nl80211_off_channel(family, nl80211_cmd_cancel_remain_on_channel, index, 0, &cookie);
		report("virt-wifi-cancel-roc", cancelled == 0 && again == ENOENT,
		       "cancelled: " + (cancelled == 0 ? std::string("yes") : errno_text(cancelled)) +
			       ", cancelled again: " + (again == 0 ? std::string("yes") : errno_text(again)));
	}
	unsigned long long frame_cookie = 0;
	int sent = nl80211_off_channel(family, nl80211_cmd_frame, index, 2412, &frame_cookie);
	report("virt-wifi-mgmt-tx", sent == 0 && frame_cookie != 0,
	       sent != 0 ? errno_text(sent) : "Action frame accepted on 2412 MHz, cookie " + std::to_string(frame_cookie));
}

// Whether /proc/net/dev_mcast lists the link-layer address under the link.
bool lists_multicast(const char *name, const char *address)
{
	std::istringstream lines(slurp("/proc/net/dev_mcast"));
	std::string line;
	while (std::getline(lines, line)) {
		char link[64];
		char listed[64];
		if (std::sscanf(line.c_str(), "%*d %63s %*d %*d %63s", link, listed) == 2 &&
		    std::strcmp(link, name) == 0 && std::strcmp(listed, address) == 0)
			return true;
	}
	return false;
}

// A macvlan delivers a multicast frame only to the groups its own filter
// lists, and the product's virt_wifi receives through one, so a group joined on
// the virt_wifi link has to reach the macvlan's list, and leave it again.
void check_wifi_multicast(unsigned index)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	ip_mreqn membership{};
	inet_pton(AF_INET, "239.255.77.1", &membership.imr_multiaddr);
	membership.imr_ifindex = static_cast<int>(index);
	int joined = fd < 0 ? -1 : setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership);
	if (joined != 0) {
		report("virt-wifi-multicast", false, "join: " + errno_text(errno));
		if (fd >= 0)
			close(fd);
		return;
	}
	// 239.255.77.1 maps to 01:00:5e:7f:4d:01.
	const char *address = "01005e7f4d01";
	bool on_wifi = lists_multicast(probe_wifi, address);
	bool on_macvlan = false;
	for (int i = 0; i < 30 && !on_macvlan; ++i) {
		on_macvlan = lists_multicast(probe_macvlan, address);
		if (!on_macvlan)
			usleep(100000);
	}
	close(fd);
	bool gone = false;
	for (int i = 0; i < 30 && !gone; ++i) {
		gone = !lists_multicast(probe_macvlan, address);
		if (!gone)
			usleep(100000);
	}
	report("virt-wifi-multicast", on_wifi && on_macvlan && gone,
	       std::string("group 239.255.77.1 listed on ") + probe_wifi + ": " + (on_wifi ? "yes" : "no") +
		       ", on " + probe_macvlan + ": " + (on_macvlan ? "yes" : "no") +
		       ", gone from it after leaving: " + (gone ? "yes" : "no"));
}

// The stack the Android product builds on its NIC, built on a dummy link so
// that the guest's own interfaces stay as they are: a macvlan with its own
// address, and on it a virt_wifi interface when the kernel has one.
int add_probe_stack(bool wifi, int &failed_step)
{
	static const unsigned char address[6] = {0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};
	failed_step = 1;
	int error = add_link(probe_lower, "dummy", 0, nullptr);
	if (error != 0)
		return error;
	failed_step = 2;
	error = add_link(probe_macvlan, "macvlan", if_nametoindex(probe_lower), address);
	if (error != 0 || !wifi)
		return error;
	failed_step = 3;
	return add_link(probe_wifi, "virt_wifi", if_nametoindex(probe_macvlan), nullptr);
}

// A macvlan over a dummy link: every product has the driver.
void check_macvlan()
{
	delete_link(probe_lower);
	int step = 0;
	int error = add_probe_stack(false, step);
	bool listed = error == 0 && if_nametoindex(probe_macvlan) != 0;
	std::string detail = error == 0 ? std::string(probe_macvlan) + " created over " + probe_lower
					: "step " + std::to_string(step) + ": " + errno_text(error);
	delete_link(probe_lower);
	if (error == EPERM && geteuid() != 0)
		report("macvlan", true, detail + " (not root, skipped)");
	else
		report("macvlan", listed, detail);
}

// The Android product's virtual Wi-Fi: cfg80211 with its nl80211 family and
// virt_wifi's phy, a virt_wifi interface on the macvlan that connects to the
// access point VirtWifi and drops its carrier when it disconnects. The Linux
// product has no 802.11 stack, so nothing answers on nl80211.
void check_wifi(bool android)
{
	std::string config = kernel_config();
	bool cfg = config_has(config, "CONFIG_CFG80211=y");
	bool virt = config_has(config, "CONFIG_VIRT_WIFI=y");
	bool macvlan = config_has(config, "CONFIG_MACVLAN=y");
	std::string detail = std::string("cfg80211 ") + (cfg ? "on" : "off") + ", virt_wifi " +
			     (virt ? "on" : "off") + ", macvlan " + (macvlan ? "on" : "off");
	if (config.empty())
		report("wifi-config", false, "/proc/config.gz is unreadable");
	else
		report("wifi-config", macvlan && cfg == android && virt == android, detail);

	int family = generic_family("nl80211");
	bool phy = access("/sys/class/ieee80211", F_OK) == 0;
	if (!android) {
		report("no-nl80211", family == -ENOENT && !phy,
		       std::string("nl80211 family ") +
			       (family > 0 ? "present" : errno_text(-family)) + ", /sys/class/ieee80211 " +
			       (phy ? "present" : "absent"));
		return;
	}
	report("nl80211", family > 0 && phy,
	       "nl80211 family " + (family > 0 ? std::to_string(family) : errno_text(-family)) +
		       ", /sys/class/ieee80211 " + (phy ? "present" : "absent"));
	if (family <= 0)
		return;

	delete_link(probe_lower);
	int step = 0;
	int error = add_probe_stack(true, step);
	if (error != 0) {
		bool skipped = error == EPERM && geteuid() != 0;
		report("virt-wifi-link", skipped, "step " + std::to_string(step) + ": " +
							  errno_text(error) +
							  (skipped ? " (not root, skipped)" : ""));
		delete_link(probe_lower);
		return;
	}
	unsigned index = if_nametoindex(probe_wifi);
	std::string phy_path = std::string("/sys/class/net/") + probe_wifi + "/phy80211";
	report("virt-wifi-link", index != 0 && access(phy_path.c_str(), F_OK) == 0,
	       std::string(probe_wifi) + " over " + probe_macvlan + " over " + probe_lower);

	// Both links under it must be up for the station to be.
	int up = set_link_up(probe_lower);
	if (up == 0)
		up = set_link_up(probe_macvlan);
	if (up == 0)
		up = set_link_up(probe_wifi);
	int connected = up == 0 ? nl80211_command(family, nl80211_cmd_connect, index, "VirtWifi") : up;
	bool carrier = connected == 0 && wait_for_carrier(probe_wifi, '1', 10);
	report("virt-wifi-connect", carrier,
	       connected != 0 ? errno_text(connected)
			      : std::string(carrier ? "carrier on" : "no carrier") + " after connecting to VirtWifi");
	if (connected == 0) {
		check_wifi_off_channel(family, index);
		check_wifi_multicast(index);
		int left = nl80211_command(family, nl80211_cmd_disconnect, index, "");
		bool dropped = left == 0 && wait_for_carrier(probe_wifi, '0', 10);
		report("virt-wifi-disconnect", dropped,
		       left != 0 ? errno_text(left)
				 : std::string(dropped ? "carrier off" : "carrier still on") +
					   " after disconnecting");
	}
	delete_link(probe_lower);
	report("virt-wifi-delete", if_nametoindex(probe_wifi) == 0 && if_nametoindex(probe_lower) == 0,
	       "the three links are gone");
}

// XFRM_MSG_ALLOCSPI for an ESP SPI on an IPv4 destination: 0, or the errno. The
// allocated state is an acquisition, as the IKE daemon's is.
int xfrm_alloc_spi(unsigned spi, const char *destination)
{
	xfrm_userspi_info body{};
	body.info.family = AF_INET;
	body.info.id.proto = IPPROTO_ESP;
	inet_pton(AF_INET, destination, &body.info.id.daddr.a4);
	body.min = spi;
	body.max = spi;
	NetlinkRequest request(XFRM_MSG_ALLOCSPI, NLM_F_ACK, &body, sizeof body);
	return netlink_call(NETLINK_XFRM, request);
}

int xfrm_delete_sa(unsigned spi, const char *destination)
{
	xfrm_usersa_id body{};
	body.family = AF_INET;
	body.proto = IPPROTO_ESP;
	inet_pton(AF_INET, destination, &body.daddr.a4);
	body.spi = htonl(spi);
	NetlinkRequest request(XFRM_MSG_DELSA, NLM_F_ACK, &body, sizeof body);
	return netlink_call(NETLINK_XFRM, request);
}

// Android's IKE daemon, and the CTS's IPsec tunnel tests, reserve one SPI for
// the local and the remote outer address of a tunnel. Linux 6.17 made an SPI
// unique per protocol for the whole namespace, which refuses the second
// reservation; the Android product restores the check per destination
// (CONFIG_XFRM_SPI_PER_DESTINATION) and the Linux product keeps the stricter one.
void check_xfrm_spi(bool android)
{
	const unsigned spi = 0x51000001;
	int first = xfrm_alloc_spi(spi, "192.0.2.1");
	if (first == EPERM && geteuid() != 0) {
		report("xfrm-spi", true, "not root, skipped");
		return;
	}
	int second = xfrm_alloc_spi(spi, "192.0.2.2");
	xfrm_delete_sa(spi, "192.0.2.1");
	xfrm_delete_sa(spi, "192.0.2.2");
	std::string detail = "SPI for 192.0.2.1: " + (first == 0 ? std::string("allocated") : errno_text(first)) +
			     ", the same SPI for 192.0.2.2: " +
			     (second == 0 ? std::string("allocated") : errno_text(second));
	report("xfrm-spi", first == 0 && (second == 0) == android, detail);
}

// The tables that hold a route to prefix/length through the link, from an
// RTM_GETROUTE dump of the IPv6 routes of every table: 0, or minus the errno.
int route6_tables(unsigned ifindex, const in6_addr &prefix, unsigned length, std::vector<int> &tables)
{
	tables.clear();
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	timeval timeout{5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	rtmsg body{};
	body.rtm_family = AF_INET6;
	NetlinkRequest request(RTM_GETROUTE, NLM_F_DUMP, &body, sizeof body);
	sockaddr_nl kernel{};
	kernel.nl_family = AF_NETLINK;
	const char *message = request.data();
	if (sendto(fd, message, request.size(), 0, reinterpret_cast<sockaddr *>(&kernel),
		   sizeof kernel) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	int found = 0;
	bool done = false;
	while (!done) {
		alignas(8) char buffer[16384];
		ssize_t got = recv(fd, buffer, sizeof buffer, 0);
		if (got <= 0) {
			found = got < 0 ? -errno : -EBADMSG;
			break;
		}
		for (const nlmsghdr *header = reinterpret_cast<const nlmsghdr *>(buffer);
		     NLMSG_OK(header, static_cast<unsigned>(got)); header = NLMSG_NEXT(header, got)) {
			if (header->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (header->nlmsg_type == NLMSG_ERROR) {
				found = -reinterpret_cast<const nlmsgerr *>(NLMSG_DATA(header))->error;
				done = true;
				break;
			}
			if (header->nlmsg_type != RTM_NEWROUTE)
				continue;
			const rtmsg *route = reinterpret_cast<const rtmsg *>(NLMSG_DATA(header));
			if (route->rtm_dst_len != length)
				continue;
			int table = route->rtm_table;
			unsigned oif = 0;
			bool same = false;
			int remaining = static_cast<int>(RTM_PAYLOAD(header));
			for (const rtattr *attribute = RTM_RTA(route); RTA_OK(attribute, remaining);
			     attribute = RTA_NEXT(attribute, remaining)) {
				if (attribute->rta_type == RTA_DST && RTA_PAYLOAD(attribute) == sizeof prefix)
					same = std::memcmp(RTA_DATA(attribute), &prefix, sizeof prefix) == 0;
				else if (attribute->rta_type == RTA_OIF)
					std::memcpy(&oif, RTA_DATA(attribute), sizeof oif);
				else if (attribute->rta_type == RTA_TABLE)
					std::memcpy(&table, RTA_DATA(attribute), sizeof(unsigned));
			}
			if (same && oif == ifindex)
				tables.push_back(table);
		}
	}
	close(fd);
	return found;
}

std::string tables_text(const std::vector<int> &tables, int error)
{
	if (error != 0)
		return "(" + errno_text(error < 0 ? -error : error) + ")";
	std::string text = "[";
	for (size_t i = 0; i < tables.size(); ++i)
		text += (i != 0 ? "," : "") + std::to_string(tables[i]);
	return text + "]";
}

int address6(unsigned short type, unsigned ifindex, const in6_addr &address, unsigned length)
{
	ifaddrmsg body{};
	body.ifa_family = AF_INET6;
	body.ifa_prefixlen = static_cast<unsigned char>(length);
	body.ifa_scope = RT_SCOPE_UNIVERSE;
	body.ifa_index = ifindex;
	NetlinkRequest request(type, type == RTM_NEWADDR ? NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK : NLM_F_ACK,
			       &body, sizeof body);
	request.put(IFA_LOCAL, &address, sizeof address);
	return netlink_call(NETLINK_ROUTE, request);
}

// The Android common kernel's accept_ra_rt_table sysctl, in every product. It
// is 0, mainline's behaviour, until something writes it (Android's netd writes
// -1000 for default and so for every new link), and then the prefix route of
// an address, as the route a router advertisement installs, goes to the table
// ifindex + 1000 instead of main. A kernel without IPv6 multiple tables (the
// Linux configuration) has only main, so the route stays there.
void check_ra_rt_table()
{
	std::string value = slurp("/proc/sys/net/ipv6/conf/all/accept_ra_rt_table");
	report("ra-rt-table-default", value == "0\n",
	       value.empty() ? "the sysctl is missing" : "all.accept_ra_rt_table is " + value.substr(0, value.size() - 1));

	delete_link(probe_lower);
	int error = add_link(probe_lower, "dummy", 0, nullptr);
	if (error == 0)
		error = set_link_up(probe_lower);
	if (error != 0) {
		bool skipped = error == EPERM && geteuid() != 0;
		report("ra-rt-table-route", skipped, errno_text(error) + (skipped ? " (not root, skipped)" : ""));
		delete_link(probe_lower);
		return;
	}
	unsigned index = if_nametoindex(probe_lower);
	in6_addr address{};
	in6_addr prefix{};
	inet_pton(AF_INET6, "2001:db8:cafe::1", &address);
	inet_pton(AF_INET6, "2001:db8:cafe::", &prefix);
	std::string path = std::string("/proc/sys/net/ipv6/conf/") + probe_lower + "/accept_ra_rt_table";
	// A new link takes the value of "default", which netd sets to -1000.
	bool zeroed = write_text(path.c_str(), "0") && slurp(path.c_str()) == "0\n";

	std::vector<int> by_default;
	std::vector<int> after_delete;
	std::vector<int> in_device_table;
	int added = address6(RTM_NEWADDR, index, address, 64);
	int read_default = added == 0 ? route6_tables(index, prefix, 64, by_default) : -added;
	int deleted = address6(RTM_DELADDR, index, address, 64);
	int read_deleted = deleted == 0 ? route6_tables(index, prefix, 64, after_delete) : -deleted;

	bool written = write_text(path.c_str(), "-1000");
	std::string readback = slurp(path.c_str());
	added = written ? address6(RTM_NEWADDR, index, address, 64) : EACCES;
	int read_device = added == 0 ? route6_tables(index, prefix, 64, in_device_table) : -added;
	bool tables = config_has(kernel_config(), "CONFIG_IPV6_MULTIPLE_TABLES=y");
	int expected = tables ? static_cast<int>(index) + 1000 : static_cast<int>(RT_TABLE_MAIN);
	bool ok = zeroed && read_default == 0 && by_default == std::vector<int>{RT_TABLE_MAIN} && read_deleted == 0 &&
		  after_delete.empty() && readback == "-1000\n" && read_device == 0 &&
		  in_device_table == std::vector<int>{expected};
	report("ra-rt-table-route", ok,
	       std::string(zeroed ? "" : "(the sysctl does not take 0) ") + "prefix route in tables " +
		       tables_text(by_default, read_default) + " with 0 written, " +
		       tables_text(after_delete, read_deleted) + " once the address is gone, and " +
		       tables_text(in_device_table, read_device) + " with " + (readback.empty() ? "?" : readback.substr(0, readback.size() - 1)) +
		       " written (expected [" + std::to_string(expected) + "], " +
		       (tables ? "IPv6 multiple tables on" : "IPv6 multiple tables off") + ")");
	delete_link(probe_lower);
}

// The Virtio vsock transport is built in, so its driver is registered
// although the guest has no device, and no balloon driver exists. A stream on
// the loopback CID still connects, is accepted and carries a byte: with the
// transport registered, only the host's CID 2 moved to it. An Android guest
// may refuse the socket by policy; that is skipped.
void check_virtio_drivers()
{
	std::string config = kernel_config();
	if (config.empty()) {
		report("virtio-drivers", false, "/proc/config.gz is unreadable");
		return;
	}
	bool vsock_config = config_has(config, "CONFIG_VIRTIO_VSOCKETS=y");
	bool balloon_config = config.find("CONFIG_VIRTIO_BALLOON=") != std::string::npos;
	bool bus = access("/sys/bus/virtio/drivers", F_OK) == 0;
	bool vsock_driver = access("/sys/bus/virtio/drivers/vmw_vsock_virtio_transport", F_OK) == 0;
	bool balloon_driver = access("/sys/bus/virtio/drivers/virtio_balloon", F_OK) == 0;
	report("virtio-drivers", vsock_config && !balloon_config && bus && vsock_driver && !balloon_driver,
	       std::string("vsock config ") + (vsock_config ? "y" : "no") + ", vsock driver " +
		       (vsock_driver ? "registered" : "absent") + ", balloon config " +
		       (balloon_config ? "set" : "unset") + ", balloon driver " +
		       (balloon_driver ? "registered" : "absent") +
		       (bus ? "" : " (no /sys/bus/virtio/drivers)"));
}

void check_vsock_loopback()
{
	int server = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (server < 0) {
		int error = errno;
		bool refused = error == EACCES || error == EPERM;
		report("vsock-loopback", refused, errno_text(error) + (refused ? " (refused by policy, skipped)" : ""));
		return;
	}
	sockaddr_vm address{};
	address.svm_family = AF_VSOCK;
	address.svm_cid = VMADDR_CID_LOCAL;
	address.svm_port = VMADDR_PORT_ANY;
	socklen_t length = sizeof address;
	bool listening = bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0 &&
			 getsockname(server, reinterpret_cast<sockaddr *>(&address), &length) == 0 &&
			 listen(server, 1) == 0;
	int client = listening ? socket(AF_VSOCK, SOCK_STREAM, 0) : -1;
	timeval timeout{5, 0};
	if (client >= 0)
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	int error = listening ? 0 : errno;
	if (client >= 0 && connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
		error = errno;
		close(client);
		client = -1;
	}
	int peer = client >= 0 ? accept(server, nullptr, nullptr) : -1;
	if (client >= 0 && peer < 0)
		error = errno;
	if (peer >= 0)
		setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	char sent = 'k';
	char received = 0;
	bool carried = peer >= 0 && write(client, &sent, 1) == 1 && read(peer, &received, 1) == 1 &&
		       received == sent;
	report("vsock-loopback", carried,
	       carried ? "one byte through CID 1 port " + std::to_string(address.svm_port)
		       : "no stream through CID 1: " + errno_text(error));
	if (peer >= 0)
		close(peer);
	if (client >= 0)
		close(client);
	close(server);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--umh") == 0) {
		char chunk[4096];
		while (read(0, chunk, sizeof chunk) > 0) {
		}
		int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0)
			close(fd);
		return 0;
	}
	const char *product = argc == 2 ? argv[1] : "linux";
	if (argc > 2 || (std::strcmp(product, "android") != 0 && std::strcmp(product, "linux") != 0)) {
		std::fprintf(stderr, "usage: kernel_config_probe [linux|android]\n");
		return 125;
	}
	bool android = std::strcmp(product, "android") == 0;
	check_multicast();
	check_rds();
	check_no_rds_listener();
	check_sysv(!android);
	check_nfs(!android);
	check_other_filesystems(!android);
	check_sctp(!android);
	check_wake_lock(android);
	check_hardening_config(android);
	check_stack_config();
	check_virtio_drivers();
	check_vsock_loopback();
	check_usermode_helper(android);
	check_incfs(android);
	check_macvlan();
	check_wifi(android);
	check_xfrm_spi(android);
	check_ra_rt_table();
	std::printf("%s: %d failure(s)\n", product, failures);
	return failures;
}

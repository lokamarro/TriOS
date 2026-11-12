// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Splits the guest's one network device into the management NIC and the
 * virtual Wi-Fi the framework sees, the way Cuttlefish does with its
 * rename_netiface and setup_wifi tools.
 *
 * eth0 is the host's user-mode network. adb reaches the guest through the
 * host's forward to 10.0.2.15, so eth0 keeps that address statically and the
 * framework never manages it: the product declares no Ethernet. The tool
 *
 *  1. addresses eth0 with 10.0.2.15/24 and sends the host's DHCP server one
 *     DHCPREQUEST for that address from eth0's MAC, which makes the server
 *     reserve it before any other MAC exists;
 *  2. routes everything that leaves from 10.0.2.15 through a private table
 *     that holds only the link route, so the replies adbd sends keep leaving
 *     eth0 whichever network the framework makes the default;
 *  3. stacks a private macvlan with a MAC of its own on eth0 and a virt_wifi
 *     device named wlan0 on that, so the framework sees a Wi-Fi station whose
 *     DHCP lease is another address of the same network (10.0.2.16).
 *
 * init has set arp_ignore and arp_announce already, so eth0 and the macvlan
 * answer ARP for their own addresses only.
 *
 * The tool fails closed: without the lease or the source rule wlan0 would
 * either take over the address adb is reached on or send adb's replies out
 * of the wrong device, so it creates no Wi-Fi device in either case.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/fib_rules.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <memory>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include "common/libs/net/netlink_client.h"
#include "common/libs/net/netlink_request.h"
#include "common/libs/net/network_interface.h"
#include "common/libs/net/network_interface_manager.h"

namespace {

using Mac = std::array<unsigned char, 6>;

constexpr char kLowerName[] = "eth0";
constexpr char kMacvlanName[] = "buried_eth0";
constexpr char kWifiName[] = "wlan0";

// The host's user-mode network: the guest's address, the DHCP server that
// is also the gateway, and the broadcast address of the /24.
constexpr char kGuestAddress[] = "10.0.2.15";
constexpr char kBroadcastAddress[] = "10.0.2.255";
constexpr char kNetworkAddress[] = "10.0.2.0";
constexpr char kServerAddress[] = "10.0.2.2";
constexpr int kPrefixLength = 24;

// The private table of the source rule. netd numbers its own tables from
// 1000 plus the interface index and 97 to 99, and the kernel's are 253 to
// 255.
constexpr uint32_t kRouteTable = 5555;

// The MAC of the macvlan, and so of wlan0. It is locally administered and
// differs from eth0's in the first octets, so the host's DHCP server assigns
// it the next free address.
constexpr Mac kWifiMac = {0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};

constexpr int kInterfaceWaitMs = 15000;
constexpr int kLeaseAttempts = 5;
constexpr int kLeaseAttemptMs = 1000;

constexpr uint16_t kBootpServerPort = 67;
constexpr uint16_t kBootpClientPort = 68;
// The host's DHCP server reads a fixed-size BOOTP record: the 236-byte
// header, the cookie and 308 bytes of options.
constexpr size_t kBootpSize = 548;
constexpr size_t kBootpHeaderSize = 236;
constexpr uint8_t kDhcpRequest = 3;
constexpr uint8_t kDhcpAck = 5;
constexpr uint8_t kDhcpNak = 6;

int64_t NowMs() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t{now.tv_sec} * 1000 + now.tv_nsec / 1000000;
}

bool WaitForInterface(const char* name) {
  const int64_t deadline = NowMs() + kInterfaceWaitMs;
  while (if_nametoindex(name) == 0) {
    if (NowMs() >= deadline) {
      LOG(ERROR) << name << " did not appear within " << kInterfaceWaitMs
                 << " ms";
      return false;
    }
    usleep(100 * 1000);
  }
  return true;
}

bool ReadHardwareAddress(const char* name, Mac* mac) {
  android::base::unique_fd sock(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (sock < 0) {
    PLOG(ERROR) << "socket";
    return false;
  }
  ifreq request = {};
  strlcpy(request.ifr_name, name, sizeof(request.ifr_name));
  if (ioctl(sock.get(), SIOCGIFHWADDR, &request) < 0) {
    PLOG(ERROR) << "SIOCGIFHWADDR " << name;
    return false;
  }
  if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
    LOG(ERROR) << name << " is not an Ethernet device";
    return false;
  }
  memcpy(mac->data(), request.ifr_hwaddr.sa_data, mac->size());
  return true;
}

bool AddressLower(uint32_t* index) {
  auto manager = cuttlefish::NetworkInterfaceManager::New(
      cuttlefish::NetlinkClientFactory::Default());
  if (!manager) {
    LOG(ERROR) << "cannot open a netlink socket";
    return false;
  }
  auto lower = manager->Open(kLowerName, kLowerName);
  if (!lower) {
    LOG(ERROR) << "cannot open " << kLowerName;
    return false;
  }
  lower->SetOperational(true)
      .SetAddress(kGuestAddress)
      .SetBroadcastAddress(kBroadcastAddress)
      .SetPrefixLength(kPrefixLength);
  if (!manager->ApplyChanges(*lower)) {
    LOG(ERROR) << "cannot address " << kLowerName;
    return false;
  }
  *index = static_cast<uint32_t>(lower->Index());
  return true;
}

// Builds the DHCPREQUEST that asks for kGuestAddress as |mac|.
std::array<uint8_t, kBootpSize> BuildRequest(uint32_t xid, const Mac& mac) {
  std::array<uint8_t, kBootpSize> packet = {};
  packet[0] = 1;  // BOOTREQUEST
  packet[1] = ARPHRD_ETHER;
  packet[2] = static_cast<uint8_t>(mac.size());
  const uint32_t xid_net = htonl(xid);
  memcpy(&packet[4], &xid_net, sizeof(xid_net));
  memcpy(&packet[28], mac.data(), mac.size());

  uint8_t* option = &packet[kBootpHeaderSize];
  *option++ = 99;  // the magic cookie
  *option++ = 130;
  *option++ = 83;
  *option++ = 99;
  *option++ = 53;  // message type
  *option++ = 1;
  *option++ = kDhcpRequest;
  *option++ = 50;  // requested address
  *option++ = 4;
  const in_addr_t address = inet_addr(kGuestAddress);
  memcpy(option, &address, sizeof(address));
  option += sizeof(address);
  *option++ = 61;  // client identifier
  *option++ = 7;
  *option++ = ARPHRD_ETHER;
  memcpy(option, mac.data(), mac.size());
  option += mac.size();
  *option++ = 255;  // end
  return packet;
}

enum class Reply { kNone, kAck, kRefused };

// Classifies a datagram as the answer to the request with |xid| from |mac|.
Reply ParseReply(const uint8_t* data, size_t size, uint32_t xid,
                 const Mac& mac) {
  if (size < kBootpHeaderSize + 4 || data[0] != 2) return Reply::kNone;
  uint32_t reply_xid;
  memcpy(&reply_xid, &data[4], sizeof(reply_xid));
  if (ntohl(reply_xid) != xid || memcmp(&data[28], mac.data(), mac.size())) {
    return Reply::kNone;
  }
  if (data[kBootpHeaderSize] != 99 || data[kBootpHeaderSize + 1] != 130 ||
      data[kBootpHeaderSize + 2] != 83 || data[kBootpHeaderSize + 3] != 99) {
    return Reply::kNone;
  }
  uint8_t type = 0;
  for (size_t i = kBootpHeaderSize + 4; i < size;) {
    const uint8_t code = data[i++];
    if (code == 0) continue;
    if (code == 255 || i >= size) break;
    const uint8_t length = data[i++];
    if (i + length > size) break;
    if (code == 53 && length == 1) type = data[i];
    i += length;
  }
  if (type == kDhcpNak) return Reply::kRefused;
  if (type != kDhcpAck) return Reply::kNone;
  const in_addr_t want = inet_addr(kGuestAddress);
  in_addr_t offered;
  memcpy(&offered, &data[16], sizeof(offered));
  return offered == want ? Reply::kAck : Reply::kRefused;
}

// Sends the host's DHCP server one DHCPREQUEST for kGuestAddress from |mac|
// and waits for its ACK, so the server's table holds the address for |mac|
// before any other MAC asks.
bool ClaimLease(const Mac& mac) {
  android::base::unique_fd sock(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (sock < 0) {
    PLOG(ERROR) << "socket";
    return false;
  }
  const int on = 1;
  setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(kBootpClientPort);
  if (bind(sock.get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) <
      0) {
    PLOG(ERROR) << "bind " << kBootpClientPort;
    return false;
  }
  sockaddr_in server = {};
  server.sin_family = AF_INET;
  server.sin_port = htons(kBootpServerPort);
  server.sin_addr.s_addr = inet_addr(kServerAddress);

  const uint32_t xid = arc4random();
  const auto request = BuildRequest(xid, mac);
  for (int attempt = 0; attempt < kLeaseAttempts; ++attempt) {
    if (sendto(sock.get(), request.data(), request.size(), 0,
               reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
      // The link or the route may not be ready yet; the next attempt retries.
      PLOG(WARNING) << "sendto " << kServerAddress;
    }
    const int64_t deadline = NowMs() + kLeaseAttemptMs;
    for (int64_t left = kLeaseAttemptMs; left > 0; left = deadline - NowMs()) {
      pollfd wait = {sock.get(), POLLIN, 0};
      if (poll(&wait, 1, static_cast<int>(left)) <= 0) break;
      uint8_t reply[1500];
      const ssize_t size = recv(sock.get(), reply, sizeof(reply), 0);
      if (size < 0) continue;
      switch (ParseReply(reply, static_cast<size_t>(size), xid, mac)) {
        case Reply::kAck:
          return true;
        case Reply::kRefused:
          LOG(ERROR) << "the DHCP server refused " << kGuestAddress;
          return false;
        case Reply::kNone:
          break;
      }
    }
  }
  LOG(ERROR) << "the DHCP server did not answer for " << kGuestAddress;
  return false;
}

bool AddSourceRule(cuttlefish::NetlinkClient& netlink) {
  cuttlefish::NetlinkRequest request(RTM_NEWRULE, NLM_F_CREATE | NLM_F_EXCL);
  fib_rule_hdr header = {};
  header.family = AF_INET;
  header.src_len = 32;
  header.action = FR_ACT_TO_TBL;
  request.Append(header);
  request.AddInt<uint32_t>(FRA_PRIORITY, 0);
  request.AddInt<uint32_t>(FRA_SRC, inet_addr(kGuestAddress));
  request.AddInt<uint32_t>(FRA_TABLE, kRouteTable);
  return netlink.Send(request);
}

bool AddLinkRoute(cuttlefish::NetlinkClient& netlink, uint32_t lower_index) {
  cuttlefish::NetlinkRequest request(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL);
  rtmsg route = {};
  route.rtm_family = AF_INET;
  route.rtm_dst_len = kPrefixLength;
  route.rtm_protocol = RTPROT_BOOT;
  route.rtm_scope = RT_SCOPE_LINK;
  route.rtm_type = RTN_UNICAST;
  request.Append(route);
  request.AddInt<uint32_t>(RTA_DST, inet_addr(kNetworkAddress));
  request.AddInt<uint32_t>(RTA_OIF, lower_index);
  request.AddInt<uint32_t>(RTA_TABLE, kRouteTable);
  return netlink.Send(request);
}

// The creation requests leave ifi_change at 0. A flag mask would replace the new link's flags with
// ifi_flags, and with none given that clears IFF_MULTICAST, which ether_setup() sets. BringUp sets
// IFF_UP.
bool AddMacvlan(cuttlefish::NetlinkClient& netlink, uint32_t lower_index) {
  cuttlefish::NetlinkRequest request(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
  ifinfomsg info = {};
  request.Append(info);
  request.AddString(IFLA_IFNAME, kMacvlanName);
  request.AddInt<uint32_t>(IFLA_LINK, lower_index);
  request.AddMacAddress(kWifiMac);
  request.PushList(IFLA_LINKINFO);
  request.AddString(IFLA_INFO_KIND, "macvlan");
  request.PushList(IFLA_INFO_DATA);
  request.AddInt<uint32_t>(IFLA_MACVLAN_MODE, MACVLAN_MODE_PRIVATE);
  request.PopList();
  request.PopList();
  return netlink.Send(request);
}

bool AddVirtWifi(cuttlefish::NetlinkClient& netlink, uint32_t lower_index) {
  cuttlefish::NetlinkRequest request(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
  ifinfomsg info = {};
  request.Append(info);
  request.AddString(IFLA_IFNAME, kWifiName);
  request.AddInt<uint32_t>(IFLA_LINK, lower_index);
  request.PushList(IFLA_LINKINFO);
  request.AddString(IFLA_INFO_KIND, "virt_wifi");
  request.PushList(IFLA_INFO_DATA);
  request.PopList();
  request.PopList();
  return netlink.Send(request);
}

bool BringUp(cuttlefish::NetlinkClient& netlink, uint32_t index) {
  cuttlefish::NetlinkRequest request(RTM_SETLINK, 0);
  request.AddIfInfo(static_cast<int32_t>(index), true);
  return netlink.Send(request);
}

int Run() {
  if (!WaitForInterface(kLowerName)) return 1;
  Mac lower_mac;
  if (!ReadHardwareAddress(kLowerName, &lower_mac)) return 1;

  uint32_t lower_index;
  if (!AddressLower(&lower_index)) return 1;
  if (!ClaimLease(lower_mac)) return 1;
  LOG(INFO) << kLowerName << " holds " << kGuestAddress;

  auto netlink =
      cuttlefish::NetlinkClientFactory::Default()->New(NETLINK_ROUTE);
  if (!netlink) {
    LOG(ERROR) << "cannot open a netlink socket";
    return 1;
  }
  if (!AddSourceRule(*netlink)) {
    LOG(ERROR) << "cannot add the source rule for " << kGuestAddress;
    return 1;
  }
  if (!AddLinkRoute(*netlink, lower_index)) {
    LOG(ERROR) << "cannot add the link route in table " << kRouteTable;
    return 1;
  }
  if (!AddMacvlan(*netlink, lower_index)) {
    LOG(ERROR) << "cannot add " << kMacvlanName;
    return 1;
  }
  const uint32_t macvlan_index = if_nametoindex(kMacvlanName);
  if (macvlan_index == 0) {
    LOG(ERROR) << kMacvlanName << " is missing after it was added";
    return 1;
  }
  if (!AddVirtWifi(*netlink, macvlan_index)) {
    LOG(ERROR) << "cannot add " << kWifiName;
    return 1;
  }
  if (!BringUp(*netlink, macvlan_index)) {
    LOG(ERROR) << "cannot bring up " << kMacvlanName;
    return 1;
  }
  LOG(INFO) << kWifiName << " is ready on " << kMacvlanName;
  return 0;
}

}  // namespace

int main(int /*argc*/, char** argv) {
  android::base::InitLogging(argv, android::base::KernelLogger);
  return Run();
}

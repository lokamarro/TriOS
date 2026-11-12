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

#include <arpa/inet.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <recovery_ui/ethernet_device.h>
#include <recovery_ui/ethernet_ui.h>

namespace {

// The host's user-mode network hands the guest this address behind a gateway
// on the same subnet, and forwards a loopback port to the TCP port adbd
// listens on. Recovery runs no DHCP client, so the interface is addressed here.
constexpr char kInterface[] = "eth0";
constexpr char kAddress[] = "10.0.2.15";
constexpr char kNetmask[] = "255.255.255.0";

bool SetInterfaceAddress(int socket_fd, unsigned long request, const char* address) {
  struct ifreq ifr = {};
  strlcpy(ifr.ifr_name, kInterface, IFNAMSIZ);
  auto* in = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
  in->sin_family = AF_INET;
  if (inet_pton(AF_INET, address, &in->sin_addr) != 1 || ioctl(socket_fd, request, &ifr) < 0) {
    PLOG(ERROR) << "Failed to set " << kInterface << " address " << address;
    return false;
  }
  return true;
}

// Recovery on this machine has no USB cable and no bootloader. Its only transport is the
// Virtio-net interface, so the screen stays lit while it waits for input.
class DarwinRecoveryUI : public EthernetRecoveryUI {
 public:
  bool IsUsbConnected() override {
    return true;
  }
};

class DarwinDevice : public EthernetDevice {
 public:
  DarwinDevice() : EthernetDevice(new DarwinRecoveryUI, kInterface) {}

  void InitDevice() override {
    android::base::unique_fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (fd < 0) {
      PLOG(ERROR) << "Failed to open socket";
    } else if (SetInterfaceAddress(fd, SIOCSIFADDR, kAddress)) {
      SetInterfaceAddress(fd, SIOCSIFNETMASK, kNetmask);
    }
    EthernetDevice::InitDevice();
  }
};

}  // namespace

Device* make_device() {
  Device* device = new DarwinDevice;
  // The machine has no bootloader and no fastbootd to enter, and its ADB follows the host's boot
  // policy rather than the menu's Enable ADB switch.
  device->RemoveMenuItemForAction(Device::REBOOT_BOOTLOADER);
  device->RemoveMenuItemForAction(Device::ENTER_FASTBOOT);
  device->RemoveMenuItemForAction(Device::ENABLE_ADB);
  return device;
}

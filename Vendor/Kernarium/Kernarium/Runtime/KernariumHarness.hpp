// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace kernarium::harness {

class ArtifactRelay;

/*
 * What an automation run adds to a Linux boot. Only the automation runtime
 * (KERNARIUM_AUTOMATION_HARNESS) builds this; the release runtime accepts no
 * host payload. Kernarium/README.md owns the contract.
 *
 * KERNARIUM_BOOT_PAYLOAD names a file in the application's Documents
 * directory: a newc initramfs segment, written on the host by uml-initramfs,
 * that the runtime appends after its own segment, so its files may replace
 * the runtime's /boot.env. KERNARIUM_SYSTEMD_UNIT names the systemd target
 * the guest boots instead of graphical.target. A payload also attaches the
 * artifact relay, a virtio-serial port whose frames reach standard output.
 * KERNARIUM_PAYLOAD_REMOVE=1 deletes the payload file once the runtime holds
 * its bytes; a launch that reuses one copied payload leaves it unset.
 */
class AutomationBoot final {
public:
    /* Null when neither setting is present. Throws std::invalid_argument
     * naming the setting that is malformed or unreadable. */
    [[nodiscard]] static std::unique_ptr<AutomationBoot> fromEnvironment(
        std::mutex& consoleStdoutMutex);

    ~AutomationBoot();
    AutomationBoot(const AutomationBoot&) = delete;
    AutomationBoot& operator=(const AutomationBoot&) = delete;

    [[nodiscard]] std::span<const std::byte> payload() const noexcept
    {
        return payload_;
    }

    /* Empty when the guest boots the runtime's own target. */
    [[nodiscard]] const std::string& systemdUnit() const noexcept
    {
        return systemdUnit_;
    }

    /* The relay's virtio_uml.device argument, or empty without a payload.
     * A relay serves one guest: its connection name can be taken once, so a
     * guest that restarts is given a new relay (the old one ends with the
     * guest it served). */
    [[nodiscard]] std::string relayDeviceArgument();

private:
    AutomationBoot();

    std::vector<std::byte> payload_;
    std::string systemdUnit_;
    std::mutex* stdoutMutex_ {};
    std::unique_ptr<ArtifactRelay> relay_;
    bool relayTaken_ {};
};

} // namespace kernarium::harness

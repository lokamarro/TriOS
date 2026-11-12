// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How many vCPUs the applications default to and offer at most: every
/// physical core of the device, and never more than the count the memory
/// budget is measured for. It is the vCPU count a new guest takes, the top of
/// the settings and the bound of `KERNARIUM_VCPU_COUNT`; a stored count above
/// it comes down to it on load.
///
/// The rule is `application_vcpu_limit` in `runtime/guest_memory_budget.hpp`,
/// which also holds the measured count itself, and the loader answers it
/// without a runtime, as it does for the guest-RAM bound. No application
/// states the number.
nonisolated enum GuestProcessorLimit {
    static func vCPUs(physicalProcessorCount: Int) -> Int {
        Int(kernarium_runtime_loader_application_vcpu_limit(
            UInt32(clamping: max(physicalProcessorCount, 1))
        ))
    }
}

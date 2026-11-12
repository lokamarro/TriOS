// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum GuestRotationPolicy: Equatable, Sendable {
    case guest, host

    /// Preserve the guest's rotation preferences across a host-owned period,
    /// including a host crash or a guest reboot. Never source this file as code.
    public var command: String {
        let state = #"""
        state=/data/local/tmp/andarium-rotation-state
        set -e
        wait_rotation() {
            remaining=50
            while [ "$(wm user-rotation)" != "$1" ]; do
                [ "$remaining" -gt 0 ] || return 1
                remaining=$((remaining - 1))
                sleep 0.05
            done
        }
        """# + "\n"
        let validate = #"""
        case "$automatic" in null|0|1) ;; *) exit 1 ;; esac
        case "$angle" in null|0|1|2|3) ;; *) exit 1 ;; esac
        case "$fixed" in default|disabled|enabled|enabled_if_no_auto_rotation) ;; *) exit 1 ;; esac
        """#
        switch self {
        case .host:
            return state + #"""
            if [ ! -f "$state" ]; then
                automatic=$(settings get system accelerometer_rotation)
                angle=$(settings get system user_rotation)
                fixed=$(wm fixed-to-user-rotation)
            """# + "\n" + validate + "\n" + #"""
                umask 077
                printf '%s\n%s\n%s\n' "$automatic" "$angle" "$fixed" > "$state.new"
                mv "$state.new" "$state"
            fi
            wm fixed-to-user-rotation enabled
            wm user-rotation lock 0
            wait_rotation 'lock 0'
            """#
        case .guest:
            return state + #"""
            if [ -f "$state" ]; then
                { read -r automatic; read -r angle; read -r fixed; } < "$state"
            """# + "\n" + validate + "\n" + #"""
                wm fixed-to-user-rotation "$fixed"
                target=$angle
                [ "$target" != null ] || target=0
                wm user-rotation lock "$target"
                wait_rotation "lock $target"
                if [ "$automatic" = 1 ]; then
                    wm user-rotation free
                    wait_rotation free
                fi
                if [ "$angle" = null ]; then settings delete system user_rotation; fi
                if [ "$automatic" = null ]; then settings delete system accelerometer_rotation; fi
                rm "$state"
            fi
            """#
        }
    }
}

/// One guest command at a time, coalescing geometry changes while adb is busy.
/// A boot or restart invalidates every completion from the previous guest.
@MainActor
public final class GuestRotationController {
    public private(set) var applied: GuestRotationPolicy?
    private var desired = GuestRotationPolicy.guest
    private var failed: GuestRotationPolicy?
    private var epoch: UInt64 = 0
    private var task: Task<Void, Never>?
    private let apply: @MainActor (GuestRotationPolicy) async throws -> Void
    private let changed: @MainActor () -> Void
    private let failure: @MainActor (Error) -> Void

    public init(
        apply: @escaping @MainActor (GuestRotationPolicy) async throws -> Void,
        changed: @escaping @MainActor () -> Void,
        failure: @escaping @MainActor (Error) -> Void
    ) {
        self.apply = apply
        self.changed = changed
        self.failure = failure
    }

    public func update(policy: GuestRotationPolicy, ready: Bool) {
        if policy != desired { failed = nil }
        desired = policy
        guard ready else {
            epoch &+= 1
            task?.cancel()
            task = nil
            applied = nil
            failed = nil
            return
        }
        guard task == nil, applied != desired, failed != desired else { return }
        let current = epoch
        task = Task { [weak self] in
            guard let self else { return }
            while current == epoch, applied != desired {
                let next = desired
                do {
                    try await apply(next)
                    guard !Task.isCancelled, current == epoch else { return }
                    applied = next
                    changed()
                } catch {
                    guard !Task.isCancelled, current == epoch else { return }
                    // A shell command may have changed one setting before
                    // failing. The next policy must reconcile that state.
                    applied = nil
                    failed = next
                    failure(error)
                    if desired == next { break }
                }
            }
            if current == epoch { task = nil }
        }
    }
}

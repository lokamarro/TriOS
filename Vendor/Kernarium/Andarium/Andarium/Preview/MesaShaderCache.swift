// SPDX-License-Identifier: GPL-2.0-or-later

/// The preview embeds no Mesa, so there is no disk shader cache to place.
/// The device application's own `MesaShaderCache` is in `GuestHost`, which the
/// preview does not carry.
nonisolated enum MesaShaderCache {
    static func configureBeforeFirstUse() {}
}

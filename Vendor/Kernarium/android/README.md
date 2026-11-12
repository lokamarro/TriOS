# Android guest sources

Android guest inputs are organized by product line. The maintained lane is
[`lineage-24/`](lineage-24/README.md): LineageOS 24.0, which is Android 17.

This directory owns the patch series for the projects the product changes,
builder-local scripts, image/release support, and Android-version metadata. The
kernel is built from the mainline line, which owns its Android configuration
overlay and patches (see
[`../kernel/mainline-6.18/`](../kernel/mainline-6.18/README.md#android-product)).

Tracked scripts run locally on the dedicated Linux builder. They accept only
builder-local filesystem paths. Workstation-to-builder transport, endpoints,
accounts, and credentials are deliberately outside this repository.

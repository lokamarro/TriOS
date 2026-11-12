// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::device {

/*
 * A device's way to learn that the guest it serves is ending.
 *
 * The guest kernel says so through darwin_uml_guest_shutdown(), before it
 * shuts any driver down, and the runtime that receives the report cannot name
 * the devices: a device is below the runtime. The device registers a callback
 * here, as the socket paths register theirs for a terminating signal, and the
 * runtime calls every registered callback once per report that is new to its
 * run.
 *
 * A callback runs on the guest kernel's thread, in the middle of the
 * guest's shutdown. It must be quick and must not block, wait for the guest
 * or register or unregister a callback.
 */
class GuestEndRegistration final {
public:
	using Callback = void (*)(void *context) noexcept;

	/* Throws std::length_error when every slot is taken. */
	GuestEndRegistration(Callback callback, void *context);
	/* Unregisters. When this returns the callback is not running and is not
	 * called again. */
	~GuestEndRegistration();

	GuestEndRegistration(const GuestEndRegistration &) = delete;
	GuestEndRegistration &operator=(const GuestEndRegistration &) = delete;

private:
	int slot_{-1};
};

/* Calls every registered callback, in registration order. */
void notify_guest_end() noexcept;

} // namespace uml::device

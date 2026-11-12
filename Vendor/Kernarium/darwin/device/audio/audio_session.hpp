// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::audio::session {

enum class Client : std::uint8_t {
    playback,
    capture,
};

/* The process's one audio session, shared by the guest's playback and its
 * microphone. iOS routes audio by the session's category, so it is chosen
 * from who needs it: playback alone uses the playback category, and the
 * category becomes play-and-record, still mixing with other audio, only while
 * the microphone is being captured, and returns to playback when capture
 * leaves. The session is deactivated when no client is left.
 *
 * acquire() returns false if the system refuses the session; the client then
 * is not counted. macOS has no audio session, and both calls do nothing. */
[[nodiscard]] bool acquire(Client client) noexcept;
void release(Client client) noexcept;

/* The IO cycle, in seconds, that `client` asks for while it holds the session
 * (0 asks for none). The session runs at the shortest cycle its clients ask
 * for, rounded by the system to one the hardware supports, from the next
 * acquire() on. macOS configures each output engine itself, and the call does
 * nothing there. */
void preferIoCycle(Client client, double seconds) noexcept;

/* Whether another app plays audio that wants the session to itself (the
 * session's secondaryAudioShouldBeSilencedHint). It is the question a client
 * asks before it takes the session back without being told to: a client must
 * never take it from an app that is playing or recording, such as a call.
 * Always false on macOS. */
[[nodiscard]] bool otherAudioPlaying() noexcept;

} // namespace uml::audio::session

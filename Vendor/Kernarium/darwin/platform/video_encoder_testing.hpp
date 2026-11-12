// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_encoder.hpp"

#if UML_VIDEO_ENCODER_TESTING
namespace uml::platform::testing {

// Available only from the separately compiled unit-test variant of the Apple
// encoder. The next `count` coded outputs of `encoder` report that the
// VideoToolbox session was lost, as the system does when it revokes the media
// engine, so that the recovery path runs on hardware that does not fail on
// demand. `encoder` must come from `makeVideoEncoder()` of that variant.
void loseSession(VideoEncoder& encoder, unsigned count);

} // namespace uml::platform::testing
#endif

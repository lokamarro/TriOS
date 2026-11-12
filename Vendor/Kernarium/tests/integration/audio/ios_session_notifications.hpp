// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <AVFAudio/AVFAudio.h>

// What iOS 27 posts when the session becomes inactive: for the app's own
// deactivation, or for the system's, which carries an interruption context
// when another app caused it.
void postDeactivation(AVAudioSessionDeactivationSource source, bool interrupted);

// Another app takes the session: the system deactivates it with an
// interruption context.
void postInterruptionBegan();

// What iOS 27 posts when the interrupter lets go, with the system's advice on
// resuming.
void postInterruptionEnded(bool shouldResume);

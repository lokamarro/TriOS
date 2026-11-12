// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_AUTOMATION_EXCEPTION_BARRIER_H
#define KERNARIUM_AUTOMATION_EXCEPTION_BARRIER_H

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

/* Runs body and returns the Objective-C exception it raised, or nil. XCTest
 * raises for some misuse, such as typeKey given more than one character, and
 * to interrupt a test. Swift cannot catch either, and an exception left to
 * unwind the automation host's serve loop ends the host. */
NSException* _Nullable KernariumAutomationCatchException(
    NS_NOESCAPE void (^body)(void));

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

#endif

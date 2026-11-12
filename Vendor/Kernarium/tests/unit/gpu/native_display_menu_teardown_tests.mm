// SPDX-License-Identifier: GPL-2.0-or-later

/* A menu the user holds open runs its own event loop inside
 * -[NSApplication run], and that loop drains neither the main dispatch queue
 * nor -stop:. When the guest finished with a menu open, the emulator reported
 * that it had stopped and then stayed alive until someone dismissed the menu.
 * The native display must end the menu itself and return. Each case opens a
 * menu on the AppKit thread from the run callback, and a watchdog fails the
 * test if the display does not return. */

#include "native_display_application.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {

constexpr int skipped = 77;
constexpr auto trackingTimeout = std::chrono::seconds(10);
constexpr unsigned watchdogSeconds = 60;

std::atomic_int tracking {};

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "native-display-menu-teardown: %s\n", message);
        std::_Exit(1);
    }
}

NSMenu* makeMenu()
{
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Display"];
    [menu addItemWithTitle:@"Rotate Guest Display" action:nil keyEquivalent:@""];
    return menu;
}

void openOnMainThread(NSMenu* menu)
{
    CFRunLoopRef mainLoop = CFRunLoopGetMain();
    CFRunLoopPerformBlock(mainLoop, kCFRunLoopDefaultMode, ^{
        [menu popUpMenuPositioningItem:nil
                            atLocation:NSMakePoint(200.0, 200.0)
                                inView:nil];
    });
    CFRunLoopWakeUp(mainLoop);
}

bool waitForTracking(int count)
{
    return uml::testing::waitUntil(
        [&] { return tracking.load() >= count; },
        trackingTimeout,
        std::chrono::milliseconds(10));
}

/* The case that was seen: the guest finishes while a menu is open. */
void testMenuOpenWhenTheGuestFinishes()
{
    NSMenu* menu = makeMenu();
    int stopRequests = 0;
    const int result = uml::gpu::runNativeDisplayApplication(
        [&] {
            openOnMainThread(menu);
            return waitForTracking(1) ? 7 : 2;
        },
        [&] { ++stopRequests; });
    require(result == 7, "the menu never began tracking");
    require(stopRequests == 0, "the display asked a finished guest to stop");
}

/* A menu that opens only after the guest has finished, while the stop is
 * still queued, must not hold the display open either. */
void testMenuOpenedAfterTheGuestFinished()
{
    NSMenu* menu = makeMenu();
    const int before = tracking.load();
    const int result = uml::gpu::runNativeDisplayApplication(
        [&] {
            CFRunLoopRef mainLoop = CFRunLoopGetMain();
            CFRunLoopPerformBlock(mainLoop, kCFRunLoopDefaultMode, ^{
                usleep(300000);
                [menu popUpMenuPositioningItem:nil
                                    atLocation:NSMakePoint(200.0, 200.0)
                                        inView:nil];
            });
            CFRunLoopWakeUp(mainLoop);
            return 9;
        },
        [] {});
    require(result == 9, "the run result was lost");
    require(tracking.load() == before + 1, "the late menu never opened");
}

} // namespace

int main()
{
    @autoreleasepool {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (session == nullptr) {
            std::fprintf(stderr,
                "native-display-menu-teardown: no window server session\n");
            return skipped;
        }
        CFRelease(session);

        const uml::testing::Watchdog watchdog {
            "the native display with a menu open",
            std::chrono::seconds {watchdogSeconds}};

        [NSNotificationCenter.defaultCenter
            addObserverForName:NSMenuDidBeginTrackingNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        tracking.fetch_add(1);
                    }];

        testMenuOpenWhenTheGuestFinishes();
        testMenuOpenedAfterTheGuestFinished();
    }
    std::puts("native display returned with a menu open");
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later

#import <UIKit/UIKit.h>
#import <XCTest/XCTest.h>
#import <objc/runtime.h>

#include "host_battery.hpp"

#include <memory>

/* The iOS battery provider the guest's battery device reads. The guest asks
 * from the device's service thread while a vCPU waits for the answer, and the
 * main thread may itself be waiting for the guest, as it does while quiescing
 * on the way to the background. The answer must therefore never wait for the
 * main thread, and UIKit's battery notifications must keep it current. The
 * provider is linked from HostBattery.xcframework, the archive the Android
 * runtime links, so the tested object is the one that ships. */
@interface HostBatteryProviderTests : XCTestCase
@end

namespace {

using uml::platform::HostBatteryState;
using uml::platform::HostBatteryStatus;

float stubbedLevel = 0.0F;
UIDeviceBatteryState stubbedState = UIDeviceBatteryStateUnknown;

float stubBatteryLevel(id, SEL)
{
    return stubbedLevel;
}

UIDeviceBatteryState stubBatteryState(id, SEL)
{
    return stubbedState;
}

/* Replaces UIDevice's battery readings for the life of the object, so the
 * test controls what the provider reads whatever battery the host has: a Mac
 * running the iPad build reports none. */
class StubbedBattery final {
public:
    StubbedBattery()
        : level_(class_getInstanceMethod(
              object_getClass(UIDevice.currentDevice),
              @selector(batteryLevel)))
        , state_(class_getInstanceMethod(
              object_getClass(UIDevice.currentDevice),
              @selector(batteryState)))
        , previousLevel_(method_setImplementation(
              level_, reinterpret_cast<IMP>(stubBatteryLevel)))
        , previousState_(method_setImplementation(
              state_, reinterpret_cast<IMP>(stubBatteryState)))
    {
    }

    ~StubbedBattery()
    {
        method_setImplementation(level_, previousLevel_);
        method_setImplementation(state_, previousState_);
        // Every other provider in the process reads the real battery again.
        [NSNotificationCenter.defaultCenter
            postNotificationName:UIDeviceBatteryStateDidChangeNotification
                          object:UIDevice.currentDevice];
    }

    StubbedBattery(const StubbedBattery&) = delete;
    StubbedBattery& operator=(const StubbedBattery&) = delete;

private:
    Method level_;
    Method state_;
    IMP previousLevel_;
    IMP previousState_;
};

/* Runs the main run loop until `done` holds or `seconds` pass. */
bool runMainLoopUntil(NSTimeInterval seconds, bool (^done)(void))
{
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done()) {
        if ([deadline timeIntervalSinceNow] <= 0)
            return false;
        [NSRunLoop.mainRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return true;
}

} // namespace

@implementation HostBatteryProviderTests

/* The main thread blocks here, as a quiescing main thread blocks on the
 * guest, while another thread reads the battery. A provider that asks the
 * main thread for the answer never returns while it waits. */
- (void)testStateAnswersWhileTheMainThreadIsBlocked
{
    XCTAssertTrue(NSThread.isMainThread);
    auto provider = uml::platform::makeAppleHostBatteryProvider();
    auto* reader = provider.get();
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      (void)reader->state();
      dispatch_semaphore_signal(answered);
    });
    const long waited = dispatch_semaphore_wait(
        answered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    XCTAssertEqual(
        waited, 0L, @"the battery read waited for the blocked main thread");
    if (waited != 0) {
        // The reader still holds the provider; it finishes once the main
        // thread runs again, after this test has returned.
        (void)provider.release();
    }
}

/* UIKit's battery notifications arrive on the main thread; the state other
 * threads read follows each of them. */
- (void)testBatteryNotificationsUpdateTheState
{
    XCTAssertTrue(NSThread.isMainThread);
    StubbedBattery battery;
    stubbedLevel = 0.42F;
    stubbedState = UIDeviceBatteryStateUnplugged;
    XCTAssertEqual(UIDevice.currentDevice.batteryLevel, 0.42F);
    auto provider = uml::platform::makeAppleHostBatteryProvider();
    auto* reader = provider.get();

    // The provider reads the battery once the main thread runs.
    XCTAssertTrue(runMainLoopUntil(5, ^{
      const HostBatteryState state = reader->state();
      return state.percentage == 42 &&
          state.status == HostBatteryStatus::discharging;
    }), @"the provider never read the battery on the main thread");

    stubbedLevel = 0.77F;
    [NSNotificationCenter.defaultCenter
        postNotificationName:UIDeviceBatteryLevelDidChangeNotification
                      object:UIDevice.currentDevice];
    XCTAssertTrue(runMainLoopUntil(5, ^{
      return reader->state().percentage == 77;
    }), @"a battery level notification left the state behind");

    stubbedLevel = 1.0F;
    stubbedState = UIDeviceBatteryStateFull;
    [NSNotificationCenter.defaultCenter
        postNotificationName:UIDeviceBatteryStateDidChangeNotification
                      object:UIDevice.currentDevice];
    XCTAssertTrue(runMainLoopUntil(5, ^{
      const HostBatteryState state = reader->state();
      return state.percentage == 100 &&
          state.status == HostBatteryStatus::full && state.externalPower;
    }), @"a battery state notification left the state behind");
}

@end

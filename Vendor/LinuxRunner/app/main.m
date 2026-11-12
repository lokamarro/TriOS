//
//  main.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"
#import "ExceptionExfiltrator.h"
#ifdef LINUXRUNNER
#import "LinuxRunner/BootLog.h"
static void LRUncaughtException(NSException *exception) {
    LRBootEvent("uncaught Objective-C exception; retaining upstream termination behavior");
    iSHExceptionHandler(exception);
}
#endif

int main(int argc, char * argv[]) {
#ifdef LINUXRUNNER
    NSSetUncaughtExceptionHandler(LRUncaughtException);
#else
    NSSetUncaughtExceptionHandler(iSHExceptionHandler);
#endif
    @autoreleasepool {
#ifdef LINUXRUNNER
        LRStartupInitialize();
        LRBootEvent("application initialization: entering UIApplicationMain");
#endif
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}

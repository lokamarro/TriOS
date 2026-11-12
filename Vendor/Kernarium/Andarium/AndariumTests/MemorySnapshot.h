// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
/// Allocation and VM metadata for this process; no allocation contents.
FOUNDATION_EXPORT NSDictionary<NSString *, id> *andarium_test_memory_snapshot(void);
/// Releases unused GL driver caches, or returns -1 if the provider is unavailable.
FOUNDATION_EXPORT NSInteger andarium_test_release_gpu_caches(void);
NS_ASSUME_NONNULL_END

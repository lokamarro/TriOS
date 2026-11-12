#import <MetalKit/MetalKit.h>
#import "RFBProtocol.h"

NS_ASSUME_NONNULL_BEGIN
// Optional final framebuffer presentation only. Linux graphics remains software
// rendered, and the ARM64 execution backend remains the interpreter.
@interface LRMetalFramebufferView : MTKView
- (nullable instancetype)initWithFailure:(void (^)(NSString *reason))failure;
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height;
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage;
- (NSDictionary *)presentationStatistics;
- (void)clearFrame;
@end
NS_ASSUME_NONNULL_END

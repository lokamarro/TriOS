#import <Foundation/Foundation.h>
#import "RFBProtocol.h"
// Immutable compact handoff between decoder/receiver/presenter queues. Only
// changed rows cross queues; mutable full frames remain owned by their queue.
static NSData *LRFramebufferPatch(NSData *frame,uint16_t width,uint16_t height,LRFrameDamage damage) {
    size_t full,bytes;
    if(!lr_rfb_dimensions(width,height,&full)||frame.length!=full ||
       !lr_rfb_rectangle(width,height,damage.x,damage.y,damage.width,damage.height,&bytes))return nil;
    if(!damage.x&&!damage.y&&damage.width==width&&damage.height==height)return [frame copy];
    NSMutableData *patch=[NSMutableData dataWithLength:bytes];
    for(NSUInteger y=0;y<damage.height;y++)memcpy((uint8_t *)patch.mutableBytes+y*damage.width*4,
        (const uint8_t *)frame.bytes+((y+damage.y)*width+damage.x)*4,damage.width*4);
    return patch; // Ownership transfers; nobody subsequently mutates this data.
}
static BOOL LRApplyFramebufferPatch(NSMutableData *frame,NSData *patch,uint16_t width,uint16_t height,LRFrameDamage damage) {
    size_t full;
    if(!lr_rfb_dimensions(width,height,&full)||frame.length!=full)return NO;
    return lr_rfb_apply_raw(frame.mutableBytes,width,height,damage.x,damage.y,damage.width,damage.height,patch.bytes,patch.length);
}

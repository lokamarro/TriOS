// Non-ARC fixture objects exercise the lifetime of autoreleased callback work.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
static _Atomic unsigned live, peak;
@interface LRFrameLease : NSObject @end
@implementation LRFrameLease
- (id)init {if((self=[super init])){unsigned n=atomic_fetch_add(&live,1)+1,old=atomic_load(&peak);while(n>old&&!atomic_compare_exchange_weak(&peak,&old,n)){}}return self;}
- (void)dealloc {atomic_fetch_sub(&live,1);[super dealloc];}
@end
void LRMakeAutoreleasedFrameLease(void){[[[LRFrameLease alloc]init]autorelease];}
unsigned LRLiveFrameLeases(void){return atomic_load(&live);}
unsigned LRPeakFrameLeases(void){return atomic_load(&peak);}

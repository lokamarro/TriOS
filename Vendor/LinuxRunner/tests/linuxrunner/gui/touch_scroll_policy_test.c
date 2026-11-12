#include "app/LinuxRunner/TouchScrollPolicy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(LRChooseTwoFingerIntent(0,0,0,1)==LRTwoFingerUndecided);
 assert(LRChooseTwoFingerIntent(0,0,11,1)==LRTwoFingerUndecided);
 assert(LRChooseTwoFingerIntent(0,0,-12,1)==LRTwoFingerScroll);
 assert(LRChooseTwoFingerIntent(0,15,0,1.02)==LRTwoFingerScroll);
 assert(LRChooseTwoFingerIntent(0,0,0,1.08)==LRTwoFingerZoom);
 assert(LRChooseTwoFingerIntent(0,0,0,.9)==LRTwoFingerZoom);
 /* First intent stays locked while the other recognizer is still live. */
 assert(LRChooseTwoFingerIntent(LRTwoFingerScroll,0,30,1.5)==LRTwoFingerScroll);
 assert(LRChooseTwoFingerIntent(LRTwoFingerZoom,0,30,1)==LRTwoFingerZoom);
 /* A fresh gesture begins undecided; don't inherit the preceding zoom. */
 assert(LRChooseTwoFingerIntent(0,0,18,1)==LRTwoFingerScroll);
 assert(LRChooseTwoFingerIntent(0,NAN,0,1)==0);
 assert(LRChooseTwoFingerIntent(0,0,INFINITY,1)==0);
 assert(LRChooseTwoFingerIntent(0,0,0,NAN)==0);
 assert(LRChooseTwoFingerIntent(0,0,20,0)==0);
 puts("TOUCH_SCROLL_POLICY_PASS early_swipe pinch swipe_lock zoom_lock reset invalid_input; UIKit/device recognition not tested");
}

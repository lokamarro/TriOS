#ifndef LR_TOUCH_SCROLL_POLICY_H
#define LR_TOUCH_SCROLL_POLICY_H
#include <math.h>

typedef enum {
    LRTwoFingerUndecided, LRTwoFingerScroll, LRTwoFingerZoom
} LRTwoFingerIntent;

// Recognize motion while both recognizers are live, rather than waiting for
// a possible pinch to fail at touch-up. Lock the first intent for the gesture
// so pinching cannot also emit wheel events, or a swipe change the zoom.
static inline LRTwoFingerIntent LRChooseTwoFingerIntent(
        LRTwoFingerIntent prior, double x, double y, double scale) {
    if (prior != LRTwoFingerUndecided) return prior;
    if (!isfinite(x) || !isfinite(y) || !isfinite(scale) || scale <= 0)
        return LRTwoFingerUndecided;
    if (fabs(scale - 1) >= .04) return LRTwoFingerZoom;
    if (hypot(x, y) >= 12) return LRTwoFingerScroll;
    return LRTwoFingerUndecided;
}
#endif

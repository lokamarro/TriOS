#import <Vision/Vision.h>
#import <CoreML/CoreML.h>

// Host test instrumentation only. Keep the same setting/probe for both sides
// of a benchmark; this does not change any guest GL or compositing backend.
static BOOL LRConfigureProbeOCR(VNRequest *request) {
    if (!getenv("LR_PROBE_OCR_CPU")) return YES;
    if (@available(macOS 14.0, *)) {
        NSError *error = nil;
        NSDictionary *stages = [request supportedComputeStageDevicesAndReturnError:&error];
        if (!stages.count) {
            fprintf(stderr, "PROBE_OCR_DEVICE_ERROR %s\n", error.localizedDescription.UTF8String ?: "no supported stages");
            return NO;
        }
        for (VNComputeStage stage in stages) {
            BOOL selected = NO;
            for (id device in stages[stage]) {
                if ([device isKindOfClass:MLCPUComputeDevice.class]) {
                    [request setComputeDevice:device forComputeStage:stage];
                    selected = YES;
                    break;
                }
            }
            if (!selected) {
                fprintf(stderr, "PROBE_OCR_DEVICE_ERROR CPU unavailable for %s\n", stage.UTF8String);
                return NO;
            }
        }
        return YES;
    }
    fprintf(stderr, "PROBE_OCR_DEVICE_ERROR CPU selection requires macOS14 or newer\n");
    return NO;
}

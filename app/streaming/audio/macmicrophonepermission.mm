#include "macmicrophonepermission.h"
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

int plankMacMicrophonePermission()
{
    @autoreleasepool {
        const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (status == AVAuthorizationStatusAuthorized) return 1;
        if (status != AVAuthorizationStatusNotDetermined) return -1;
        return 0;
    }
}

void plankMacRequestMicrophonePermission(std::function<void()> completed)
{
    NSCAssert(NSThread.isMainThread, @"Microphone permission UI is main-thread only");
    if (plankMacMicrophonePermission() != 0) { completed(); return; }
    [NSApp activate];
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL allowed) {
        (void)allowed;
        dispatch_async(dispatch_get_main_queue(), ^{ completed(); });
    }];
}

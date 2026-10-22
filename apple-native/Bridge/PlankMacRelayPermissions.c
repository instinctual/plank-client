#include <IOKit/hidsystem/IOHIDLib.h>
#include "PlankMacRelay.h"
// Only called from the local Share action on the UI thread.
void plank_mac_relay_request_capture_permission(void) {
    if(IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)==kIOHIDAccessTypeUnknown)
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
}

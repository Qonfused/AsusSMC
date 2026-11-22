#ifndef AsusSMCMessage_h
#define AsusSMCMessage_h

#include <stdint.h>

#define AsusSMCEventVendor "com.hieplpvip"
#define AsusSMCEventCode 0x8102
#define AsusSMCBrightnessRoutingReady "BrightnessRoutingReady"
#define AsusSMCBrightnessRoutingReply "BrightnessRoutingReply"

enum AsusSMCMessageType {
    kDaemonKeyboardBacklight = 1,
    kDaemonAirplaneMode = 2,
    kDaemonSleep = 3,
    kDaemonTouchpad = 4,
    kDaemonScreenPadBrightness = 5,
    // x identifies the press. Reply with (x << 1) | pointerIsOnScreenPad.
    kDaemonChooseBrightnessDisplay = 6
};

struct AsusSMCMessage {
    int32_t type;
    // Brightness events carry the current level in x and its maximum in y.
    int32_t x;
    int32_t y;
};

#endif /* AsusSMCMessage_h */

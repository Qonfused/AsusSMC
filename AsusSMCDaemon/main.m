//
//  main.m
//  AsusSMCDaemon
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#import <CoreWLAN/CoreWLAN.h>
#import <CoreServices/CoreServices.h>
#import <IOKit/IOKitLib.h>
#import <sys/ioctl.h>
#import <sys/socket.h>
#import <sys/kern_event.h>
#import <dlfcn.h>
#import "../Global/AsusSMCMessage.h"
#import "BezelServices.h"
#import "OSD.h"

/*
 *    kAERestart        will cause system to restart
 *    kAEShutDown       will cause system to shutdown
 *    kAEReallyLogout   will cause system to logout
 *    kAESleep          will cause system to sleep
 */
extern OSStatus MDSendAppleEventToSystemProcess(AEEventID eventToSend);

// requires IOBluetooth.framework
void IOBluetoothPreferenceSetControllerPowerState(int);
int IOBluetoothPreferenceGetControllerPowerState(void);

static void *(*_BSDoGraphicWithMeterAndTimeout)(CGDirectDisplayID arg0, BSGraphic arg1, int arg2, float v, int timeout) = NULL;

bool _loadBezelServices(void) {
    // Load BezelServices framework
    void *handle = dlopen("/System/Library/PrivateFrameworks/BezelServices.framework/Versions/A/BezelServices", RTLD_GLOBAL);
    if (!handle) {
        NSLog(@"Error opening framework");
        return NO;
    } else {
        _BSDoGraphicWithMeterAndTimeout = dlsym(handle, "BSDoGraphicWithMeterAndTimeout");
        return _BSDoGraphicWithMeterAndTimeout != NULL;
    }
}

bool _loadOSDFramework(void) {
    return [[NSBundle bundleWithPath:@"/System/Library/PrivateFrameworks/OSD.framework"] load];
}

OSStatus MDSendAppleEventToSystemProcess(AEEventID eventToSendID) {
    AEAddressDesc targetDesc;
    static const ProcessSerialNumber kPSNOfSystemProcess = {0, kSystemProcess };
    AppleEvent eventReply = {typeNull, NULL};
    AppleEvent eventToSend = {typeNull, NULL};

    OSStatus status = AECreateDesc(typeProcessSerialNumber,
                                   &kPSNOfSystemProcess, sizeof(kPSNOfSystemProcess), &targetDesc);

    if (status != noErr) return status;

    status = AECreateAppleEvent(kCoreEventClass, eventToSendID,
                                &targetDesc, kAutoGenerateReturnID, kAnyTransactionID, &eventToSend);

    AEDisposeDesc(&targetDesc);

    if (status != noErr) return status;

    status = AESendMessage(&eventToSend, &eventReply,
                           kAENormalPriority, kAEDefaultTimeout);

    AEDisposeDesc(&eventToSend);
    if (status != noErr) return status;
    AEDisposeDesc(&eventReply);
    return status;
}

static CGDirectDisplayID displayID(NSScreen *screen) {
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

static CGDirectDisplayID screenPadDisplayID(void) {
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, NULL, &count) != kCGErrorSuccess || !count)
        return kCGNullDirectDisplay;

    CGDirectDisplayID *displays = calloc(count, sizeof(*displays));
    if (!displays)
        return kCGNullDirectDisplay;

    CGDirectDisplayID match = kCGNullDirectDisplay;
    if (CGGetActiveDisplayList(count, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            CGDirectDisplayID display = displays[i];
            uint32_t model = CGDisplayModelNumber(display);
            // BOE087F (UX481) and BOE085F (UX581/UX582) are ScreenPad panels.
            if (CGDisplayVendorNumber(display) != 0x09e5 ||
                (model != 0x087f && model != 0x085f))
                continue;
            if (match != kCGNullDirectDisplay) {
                match = kCGNullDirectDisplay;
                break;
            }
            match = display;
        }
    }
    free(displays);
    return match;
}

static bool pointerOnScreenPad(void) {
    CGEventRef sample = CGEventCreate(NULL);
    if (!sample)
        return false;
    CGPoint point = CGEventGetLocation(sample);
    CFRelease(sample);

    CGDirectDisplayID displays[2];
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(point, 2, displays, &count) != kCGErrorSuccess ||
        count != 1)
        return false;
    CGDirectDisplayID display = displays[0];
    return display && !CGDisplayIsInMirrorSet(display) &&
        !CGDisplayIsAsleep(display) && display == screenPadDisplayID();
}

static void chooseBrightnessDisplay(io_service_t service, int32_t request) {
    if (request <= 0)
        return;

    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    if (![session[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue])
        return;

    bool screenPad = false;
    if ([session[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue] &&
        ![session[@"CGSSessionScreenIsLocked"] boolValue] && pointerOnScreenPad()) {
        CFTypeRef enabled = IORegistryEntryCreateCFProperty(
            service, CFSTR("ScreenPadEnabled"), kCFAllocatorDefault, 0);
        screenPad = enabled == kCFBooleanTrue;
        if (enabled)
            CFRelease(enabled);
    }

    int64_t reply = ((int64_t)request << 1) | screenPad;
    IORegistryEntrySetCFProperty(service, CFSTR(AsusSMCBrightnessRoutingReply),
                                 (__bridge CFNumberRef)@(reply));
}

static void showBacklightMeter(CGDirectDisplayID display, int level, int max,
                               BSGraphic bezelImage, OSDGraphic osdImage) {
    if (!display || max <= 0 || level < 0 || level > max)
        return;

    if (_BSDoGraphicWithMeterAndTimeout) {
        _BSDoGraphicWithMeterAndTimeout(display, bezelImage, 0,
                                        (float)level / max, 1);
    } else {
        OSDManager *manager = [NSClassFromString(@"OSDManager") sharedManager];
        [manager showImage:osdImage onDisplayID:display
                  priority:OSDPriorityDefault msecUntilFade:1000
            filledChiclets:level totalChiclets:max locked:NO];
    }
}

void showKBoardBLightStatus(int level, int max) {
    BSGraphic bezel = level ? BSGraphicKeyboardBacklightMeter
                            : BSGraphicKeyboardBacklightDisabledMeter;
    OSDGraphic osd = level ? OSDGraphicKeyboardBacklightMeter
                           : OSDGraphicKeyboardBacklightDisabledMeter;
    showBacklightMeter(displayID(NSScreen.mainScreen), level, max, bezel, osd);
}

void goToSleep(void) {
    if (_BSDoGraphicWithMeterAndTimeout != NULL) // El Capitan and probably older systems
        MDSendAppleEventToSystemProcess(kAESleep);
    else {
        // Sierra+
        CGDirectDisplayID currentDisplayId = displayID(NSScreen.mainScreen);
        [[NSClassFromString(@"OSDManager") sharedManager] showImage:OSDGraphicSleep onDisplayID:currentDisplayId priority:OSDPriorityDefault msecUntilFade:1000];
    }
}

BOOL airplaneModeEnabled = NO, lastWifiState;
int lastBluetoothState;
void toggleAirplaneMode(void) {
    airplaneModeEnabled = !airplaneModeEnabled;

    CWInterface *currentInterface = [CWWiFiClient.sharedWiFiClient interface];
    NSError *err = nil;

    if (airplaneModeEnabled) {
        lastWifiState = currentInterface.powerOn;
        lastBluetoothState = IOBluetoothPreferenceGetControllerPowerState();
        [currentInterface setPower:NO error:&err];
        IOBluetoothPreferenceSetControllerPowerState(0);
    } else {
        [currentInterface setPower:lastWifiState error:&err];
        IOBluetoothPreferenceSetControllerPowerState(lastBluetoothState);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        printf("daemon started...\n");

        if (!_loadBezelServices()) {
            _loadOSDFramework();
        }

        int systemSocket = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);
        if (systemSocket < 0) {
            perror("socket(SYSPROTO_EVENT)");
            return EXIT_FAILURE;
        }

        struct kev_vendor_code vendorCode = {.vendor_string = AsusSMCEventVendor};
        if (ioctl(systemSocket, SIOCGKEVVENDOR, &vendorCode) < 0) {
            perror("SIOCGKEVVENDOR");
            return EXIT_FAILURE;
        }

        struct kev_request filter = {
            .vendor_code = vendorCode.vendor_code,
            .kev_class = KEV_ANY_CLASS,
            .kev_subclass = KEV_ANY_SUBCLASS
        };
        if (ioctl(systemSocket, SIOCSKEVFILT, &filter) < 0) {
            perror("SIOCSKEVFILT");
            return EXIT_FAILURE;
        }

        io_service_t service = IOServiceGetMatchingService(
            MACH_PORT_NULL, IOServiceMatching("AsusSMC"));
        if (!service)
            return EXIT_FAILURE;

        // Subscribe before advertising readiness so the first key can be routed.
        IORegistryEntrySetCFProperty(service, CFSTR(AsusSMCBrightnessRoutingReady),
                                     kCFBooleanTrue);

        char kextMsg[KEV_MSG_HEADER_SIZE + sizeof(struct AsusSMCMessage)];
        while (YES) {
            ssize_t bytesReceived = recv(systemSocket, kextMsg, sizeof(kextMsg), 0);
            if (bytesReceived < 0) {
                if (errno == EINTR)
                    continue;
                perror("recv(SYSPROTO_EVENT)");
                IOObjectRelease(service);
                return EXIT_FAILURE;
            }
            if (bytesReceived != sizeof(kextMsg))
                continue;

            const struct kern_event_msg *event = (const void *)kextMsg;
            if (event->event_code != AsusSMCEventCode ||
                event->total_size != sizeof(kextMsg))
                continue;

            const struct AsusSMCMessage *message = (const void *)event->event_data;
            @autoreleasepool {
                switch (message->type) {
                    case kDaemonKeyboardBacklight:
                        showKBoardBLightStatus(message->x, message->y);
                        break;
                    case kDaemonChooseBrightnessDisplay:
                        chooseBrightnessDisplay(service, message->x);
                        break;
                    case kDaemonScreenPadBrightness:
                        showBacklightMeter(screenPadDisplayID(),
                                           message->x, message->y,
                                           BSGraphicBacklightMeter,
                                           OSDGraphicBacklight);
                        break;
                    case kDaemonAirplaneMode:
                        toggleAirplaneMode();
                        break;
                    case kDaemonSleep:
                        goToSleep();
                        break;
                }
            }
        }
    }

    return 0;
}

//
//  AsusSMC.hpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#ifndef _AsusSMC_hpp
#define _AsusSMC_hpp

#include <sys/queue.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include "HIDReport.hpp"
#include "HIDUsageTables.h"
#include "VirtualAppleKeyboard.hpp"
#include "KernEventServer.hpp"
#include "KeyImplementations.hpp"

#define ASUS_WMI_METHODID_DSTS         0x53545344
#define ASUS_WMI_METHODID_DEVS         0x53564544
#define ASUS_WMI_METHODID_INIT         0x54494E49
#define ASUS_WMI_DEVID_ALS_ENABLE      0x00050001
#define ASUS_WMI_DEVID_KBD_BACKLIGHT   0x00050021
#define ASUS_WMI_DEVID_SCREENPAD       0x00050031
#define ASUS_WMI_DEVID_SCREENPAD_LIGHT 0x00050032
#define ASUS_WMI_DEVID_CPU_FAN_CTRL    0x00110013
#define ASUS_WMI_DEVID_GPU_FAN_CTRL    0x00110014
#define ASUS_WMI_DEVID_RSOC            0x00120057
#define ASUS_WMI_DSTS_PRESENCE_BIT     0x00010000
#define ASUS_WMI_MGMT_GUID             "97845ED0-4E6D-11DE-8A39-0800200C9A66"

#define kDeliverNotifications "RM,deliverNotifications"

class AsusSMC : public IOService {
    OSDeclareDefaultStructors(AsusSMC)

    VirtualSMCAPI::Plugin vsmcPlugin {
        xStringify(PRODUCT_NAME),
        parseModuleVersion(xStringify(MODULE_VERSION)),
        VirtualSMCAPI::Version,
    };

public:
    bool init(OSDictionary *dictionary) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    IOReturn message(uint32_t type, IOService *provider, void *argument) override;
    IOReturn setProperties(OSObject *properties) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    void systemWillShutdown(IOOptionBits specifier) override;

    void letSleep();
    void toggleAirplaneMode();
    void toggleTouchpad();
    void toggleALS(bool state);
    void toggleBatteryConservativeMode(bool state);
    void displayOff();

private:
    struct guid_block {
        char guid[16];
        union {
            char object_id[2];
            struct {
                uint8_t notify_id;
                uint8_t reserved;
            };
        };
        uint8_t instance_count;
        uint8_t flags;
    };

    struct wmi_args {
        uint32_t arg0;
        uint32_t arg1;
    } __packed;

    enum {
        kKeyboardSetTouchStatus = iokit_vendor_specific_msg(100), // set disable/enable touchpad (data is bool*)
        kKeyboardGetTouchStatus = iokit_vendor_specific_msg(101), // get disable/enable touchpad (data is bool*)
        kKeyboardKeyPressTime = iokit_vendor_specific_msg(110),   // notify of timestamp a non-modifier key was pressed (data is uint64_t*)
    };

    enum {
        FanModeAutomatic = 0,
        FanModeFullSpeed = 1
    };

    static constexpr uint32_t SensorUpdateTimeoutMS {1000};

    static constexpr uint8_t NOTIFY_BRIGHTNESS_UP_MIN = 0x10;
    static constexpr uint8_t NOTIFY_BRIGHTNESS_UP_MAX = 0x1F;

    static constexpr uint8_t NOTIFY_BRIGHTNESS_DOWN_MIN = 0x20;
    static constexpr uint8_t NOTIFY_BRIGHTNESS_DOWN_MAX = 0x2F;

    char wmi_method[5];
    int wmi_parse_guid(const char *in, char *out);
    int wmi_evaluate_method(uint32_t method_id, uint32_t arg0, uint32_t arg1);
    int wmi_get_devstate(uint32_t dev_id);
    bool wmi_dev_is_present(uint32_t dev_id);
    void parse_WDG();

    void initATKDevice();
    void initALSDevice();
    void initEC0Device();
    void initBattery();
    void readScreenPadState();
    int readScreenPadValue(uint32_t device);
    bool setScreenPadBrightness(uint8_t level);
    void setScreenPadPower(bool enabled);
    void cycleScreenPadBrightness();
    void stepScreenPadBrightness(bool brighter);
    void handleBrightnessKey(bool brighter);
    IOReturn routeBrightnessKey(bool brighter);
    IOReturn acceptBrightnessReply(OSDictionary *properties);
    void initVirtualKeyboard();

    void startATKDevice();
    void readKeyboardBacklightDirection();
    
    bool refreshALS(bool post);
    void initFans();
    bool readFanSpeed(uint32_t index, uint32_t &speed);
    void refreshFan();
    IOReturn applyFanMode(uint32_t mode);
    void updatePowerState(bool sleeping);

    void handleMessage(int code);

    IOACPIPlatformDevice *atkDevice {nullptr};
    IOACPIPlatformDevice *alsDevice {nullptr};
    IOACPIPlatformDevice *ec0Device {nullptr};
    VirtualAppleKeyboard *kbdDevice {nullptr};

    ALSForceBits forceBits;
    _Atomic(uint32_t) currentLux = ATOMIC_VAR_INIT(0);
    static constexpr uint32_t MaxFanCount = 2;
    uint32_t fanCount {1};
    _Atomic(uint32_t) currentFanSpeed[MaxFanCount];
    _Atomic(bool) fanControlSupported = ATOMIC_VAR_INIT(false);
    bool suspended {false};
    bool hasRawTachometer {false};
    bool useRawFanSpeed {false};

    IONotifier *vsmcNotifier {nullptr};

    IOWorkLoop *workloop {nullptr};
    IOCommandGate *command_gate {nullptr};
    IOTimerEventSource *poller {nullptr};

    KernEventServer kev;

    consumer_input csmrreport;
    apple_vendor_top_case_input tcreport;

    bool directACPImessaging {false};
    bool hasKeyboardBacklight {false};
    _Atomic(bool) keyboardBacklightDimming = ATOMIC_VAR_INIT(false);
    static constexpr int ScreenPadMaxBrightness = 255;
    static constexpr int ScreenPadBrightnessStep = ScreenPadMaxBrightness / 5;
    static constexpr int BrightnessSteps = 16;
    struct BrightnessRequest {
        LIST_ENTRY(BrightnessRequest) link;
        uint32_t id;
        bool answered;
        bool screenPad;
    };
    LIST_HEAD(, BrightnessRequest) brightnessRequests {};
    uint32_t brightnessRequestID {0};
    enum class BrightnessAgent { Offline, Ready, Unresponsive };
    BrightnessAgent brightnessAgent {BrightnessAgent::Offline};
    bool screenPadSupported {false};
    int screenPadPower {-1}; // -1 when firmware status is unavailable.
    bool screenPadDimming {true};
    int screenPadBrightness {-1}; // Firmware cannot report brightness while off.
    bool isALSEnabled {true};
    bool isTouchpadEnabled {true};
    bool isPanelBackLightOn {true};
    bool isTACHAvailable {false};
    bool isBatteryRSOCAvailable {false};

    uint32_t panelBrightnessLevel {16};
    char backlightEntry[1000];

    int checkBacklightEntry();
    int findBacklightEntry();
    void readPanelBrightnessValue();

    IOReturn postKeyboardInputReport(const void *report, uint32_t reportSize);
    void dispatchCSMRReport(int code, int loop = 1);
    void dispatchTCReport(int code, int loop = 1);

    IONotifier *_publishNotify {nullptr};
    IONotifier *_terminateNotify {nullptr};
    OSSet *_notificationServices {nullptr};
    void registerNotifications(void);
    void notificationHandlerGated(IOService *newService, IONotifier *notifier);
    bool notificationHandler(void *refCon, IOService *newService, IONotifier *notifier);
    void dispatchMessageGated(int *message, void *data);
    void dispatchMessage(int message, void *data);

    void registerVSMC(void);
    static bool vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier);
};

#endif //_AsusSMC_hpp

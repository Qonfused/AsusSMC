//
//  AsusSMC.cpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "AsusSMC.hpp"
#include <kern/clock.h>

static IOPMPowerState powerStates[] {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn,
        0, 0, 0, 0, 0, 0, 0, 0}
};

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

#define super IOService
OSDefineMetaClassAndStructors(AsusSMC, IOService)

bool AsusSMC::init(OSDictionary *dict) {
    if (!super::init(dict)) {
        return false;
    }

    _notificationServices = OSSet::withCapacity(1);

    kev.setVendorID(AsusSMCEventVendor);
    kev.setEventCode(AsusSMCEventCode);

    atomic_init(&currentLux, 0);
    for (uint32_t i = 0; i < MaxFanCount; ++i) {
        atomic_init(&currentFanSpeed[i], InvalidFanSpeed);
    }

    return true;
}

IOService *AsusSMC::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider, score)) {
        return NULL;
    }

    IOACPIPlatformDevice *dev = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!dev) {
        return NULL;
    }

    OSObject *obj;
    dev->evaluateObject("_UID", &obj);

    OSString *name = OSDynamicCast(OSString, obj);
    if (!name) {
        return NULL;
    }

    IOService *ret = NULL;
    if (name->isEqualTo("ATK")) {
        ret = this;
    }
    name->release();

    return ret;
}

bool AsusSMC::start(IOService *provider) {
    if (!provider || !super::start(provider)) {
        SYSLOG("atk", "failed to start parent");
        return false;
    }

    atkDevice = (IOACPIPlatformDevice *)provider;

    parse_WDG();

    initATKDevice();

    initALSDevice();

    initEC0Device();

    readScreenPadState();
    if (screenPadSupported && fnLockSupported) {
        // The Fn LED can be off at boot while F4/F5 still emit function keys.
        // Selecting unlocked mode makes firmware send brightness notifications.
        setFnLock(false);
    }

    initBattery();

    initVirtualKeyboard();

    startATKDevice();

    workloop = getWorkLoop();
    if (!workloop) {
        DBGLOG("atk", "Failed to get workloop");
        return false;
    }
    workloop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (workloop->addEventSource(command_gate) != kIOReturnSuccess)) {
        DBGLOG("atk", "Could not open command gate");
        return false;
    }

    initFans();

    PMinit();
    provider->joinPMtree(this);
    if (registerPowerDriver(this, powerStates, 2) != kIOReturnSuccess) {
        fanControlSupported = false;
        SYSLOG("atk", "Failed to register power management");
    }
    setProperty("FanControlSupported", fanControlSupported);

    setProperty("IsTouchpadEnabled", true);
    setProperty("Copyright", "Copyright © 2018-2020 Le Bao Hiep. All rights reserved.");

    extern kmod_info_t kmod_info;
    setProperty("AsusSMC-Version", kmod_info.version);
#ifdef DEBUG
    setProperty("AsusSMC-Build", "Debug");
#else
    setProperty("AsusSMC-Build", "Release");
#endif

    registerNotifications();

    registerVSMC();

    registerService();

    return true;
}

void AsusSMC::stop(IOService *provider) {
    atomic_store(&batteryLimit.acceptingWrites, false);
    updatePowerState(true);
    PMstop();

    if (poller) {
        poller->cancelTimeout();
    }
    if (workloop && poller) {
        workloop->removeEventSource(poller);
    }
    if (workloop && command_gate) {
        workloop->removeEventSource(command_gate);
    }
    OSSafeReleaseNULL(workloop);
    OSSafeReleaseNULL(poller);
    OSSafeReleaseNULL(command_gate);

    _publishNotify->remove();
    _terminateNotify->remove();
    _notificationServices->flushCollection();
    OSSafeReleaseNULL(_publishNotify);
    OSSafeReleaseNULL(_terminateNotify);
    OSSafeReleaseNULL(_notificationServices);

    OSSafeReleaseNULL(kbdDevice);

    super::stop(provider);
    return;
}

IOReturn AsusSMC::message(uint32_t type, IOService *provider, void *argument) {
    DBGLOG("atk", "Message %x from %s", type,
           provider ? provider->getName() : "unknown");

    switch (type) {
        case kIOACPIMessageDeviceNotification:
        {
            if (argument == nullptr) {
                return kIOReturnBadArgument;
            }
            if (directACPImessaging) {
                handleMessage(*((uint32_t *)argument));
            } else {
                uint32_t event = *((uint32_t *)argument);
                OSNumber *arg = OSNumber::withNumber(event, 32);
                if (arg == nullptr) {
                    return kIOReturnNoMemory;
                }
                uint32_t code;
                OSObject *args[] {arg};
                IOReturn ret = atkDevice->evaluateInteger("_WED", &code, args, 1);
                arg->release();
                if (ret != kIOReturnSuccess) {
                    return ret;
                }
                handleMessage(code);
            }
            break;
        }

        case kSetKeyboardBacklightMessage:
        {
            if (argument == nullptr) {
                return kIOReturnBadArgument;
            }
            if (hasKeyboardBacklight) {
                uint16_t brightness = *static_cast<uint16_t *>(argument) >> 4;
                OSNumber *arg = OSNumber::withNumber(brightness, 16);
                if (arg == nullptr) {
                    return kIOReturnNoMemory;
                }
                OSObject *args[] {arg};
                IOReturn ret = atkDevice->evaluateObject("SKBV", nullptr, args, 1);
                arg->release();
                if (ret != kIOReturnSuccess) {
                    return ret;
                }
                uint16_t step = brightness >> 4;
                if (step == 0 || step == 15) {
                    keyboardBacklightDimming = step == 15;
                }
            }
            break;
        }

        default:
            return super::message(type, provider, argument);
    }
    return kIOReturnSuccess;
}

int AsusSMC::wmi_parse_guid(const char *in, char *out) {
    for (int i = 3; i >= 0; i--) {
        out += snprintf(out, 3, "%02X", in[i] & 0xFF);
    }

    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[5] & 0xFF);
    out += snprintf(out, 3, "%02X", in[4] & 0xFF);
    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[7] & 0xFF);
    out += snprintf(out, 3, "%02X", in[6] & 0xFF);
    out += snprintf(out, 2, "-");
    out += snprintf(out, 3, "%02X", in[8] & 0xFF);
    out += snprintf(out, 3, "%02X", in[9] & 0xFF);
    out += snprintf(out, 2, "-");

    for (int i = 10; i <= 15; i++) {
        out += snprintf(out, 3, "%02X", in[i] & 0xFF);
    }

    *out = '\0';
    return 0;
}

int AsusSMC::wmi_evaluate_method(uint32_t method_id, uint32_t arg0, uint32_t arg1) {
    OSObject *params[3];

    struct wmi_args args = {
        .arg0 = arg0,
        .arg1 = arg1
    };

    params[0] = OSNumber::withNumber(static_cast<uint32_t>(0), 32);
    params[1] = OSNumber::withNumber(method_id, 32);
    params[2] = OSData::withBytes(&args, sizeof(wmi_args));

    uint32_t val;
    IOReturn ret = atkDevice->evaluateInteger(wmi_method, &val, params, 3);
    params[0]->release();
    params[1]->release();
    params[2]->release();

    if (ret != kIOReturnSuccess) {
        DBGLOG("wmi", "wmi_evaluate_method failed");
        return -1;
    }

    if (val == 0xfffffffe) {
        DBGLOG("wmi", "wmi_evaluate_method invalid method_id");
        return -1;
    }

    return val;
}

int AsusSMC::wmi_get_devstate(uint32_t dev_id) {
    return wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0);
}

bool AsusSMC::wmi_dev_is_present(uint32_t dev_id) {
    int status = wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0);
    return status != -1 && (status & ASUS_WMI_DSTS_PRESENCE_BIT);
}

void AsusSMC::parse_WDG() {
    OSObject *wdg;
    if (atkDevice->evaluateObject("_WDG", &wdg) != kIOReturnSuccess) {
        SYSLOG("wmi", "No method _WDG!");
        return;
    }

    OSData *data = OSDynamicCast(OSData, wdg);
    if (!data) {
        SYSLOG("guid", "Cast WDG error!");
        return;
    }

    int total = data->getLength() / sizeof(struct guid_block);

    char guid_string[37];

    for (int i = 0; i < total; i++) {
        struct guid_block *g = (struct guid_block *) data->getBytesNoCopy(i * sizeof(struct guid_block), sizeof(struct guid_block));
        wmi_parse_guid(g->guid, guid_string);

        if (strncmp(guid_string, ASUS_WMI_MGMT_GUID, 36) == 0) {
            snprintf(wmi_method, 5, "WM%c%c", g->object_id[0], g->object_id[1]);
            DBGLOG("wmi", "parse_WDG found WMI method %s", wmi_method);
            return;
        }
    }

    // Couldn't find WMI method. Let's assume it's WMNB
    SYSLOG("wmi", "parse_WDG couldn't find WMI method");
    lilu_os_strncpy(wmi_method, "WMNB", 5);
}

void AsusSMC::initATKDevice() {
    wmi_evaluate_method(ASUS_WMI_METHODID_INIT, 0, 0);

    int status = wmi_get_devstate(ASUS_WMI_DEVID_FNLOCK);
    fnLockSupported = status >= 0 && (status & ASUS_WMI_DSTS_PRESENCE_BIT) &&
                      !(status & ASUS_WMI_FNLOCK_BIOS_DISABLED);
}

void AsusSMC::setFnLock(bool enabled) {
    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, ASUS_WMI_DEVID_FNLOCK, enabled) == 1) {
        fnLockMode = enabled;
        setProperty("FnLock", enabled);
    } else {
        SYSLOG("atk", "Failed to set Fn Lock to %u", static_cast<unsigned>(enabled));
    }
}

void AsusSMC::initALSDevice() {
    auto dict = IOService::nameMatching("AppleACPIPlatformExpert");
    if (!dict) {
        SYSLOG("als", "WTF? Failed to create matching dictionary");
        return;
    }

    auto acpi = IOService::waitForMatchingService(dict);
    dict->release();

    if (!acpi) {
        SYSLOG("als", "WTF? No ACPI");
        return;
    }

    acpi->release();

    dict = IOService::nameMatching("ACPI0008");
    if (!dict) {
        SYSLOG("als", "WTF? Failed to create matching dictionary");
        return;
    }

    auto deviceIterator = IOService::getMatchingServices(dict);
    dict->release();

    if (!deviceIterator) {
        SYSLOG("als", "No iterator");
        return;
    }

    alsDevice = OSDynamicCast(IOACPIPlatformDevice, deviceIterator->getNextObject());
    deviceIterator->release();

    if (!alsDevice) {
        SYSLOG("als", "ACPI0008 device not found");
        return;
    }

    if (alsDevice->validateObject("_ALI") != kIOReturnSuccess || !refreshALS(false)) {
        SYSLOG("als", "No functional method _ALI on ALS device");
        return;
    }

    SYSLOG("als", "Found ALS Device %s", alsDevice->getName());
}

void AsusSMC::initEC0Device() {
    isTACHAvailable = true;

    auto dict = IOService::nameMatching("AppleACPIPlatformExpert");
    if (!dict) {
        SYSLOG("ec0", "WTF? Failed to create matching dictionary");
        isTACHAvailable = false;
        return;
    }

    auto acpi = IOService::waitForMatchingService(dict);
    dict->release();

    if (!acpi) {
        SYSLOG("ec0", "WTF? No ACPI");
        isTACHAvailable = false;
        return;
    }

    acpi->release();

    dict = IOService::nameMatching("PNP0C09");
    if (!dict) {
        SYSLOG("ec0", "WTF? Failed to create matching dictionary");
        isTACHAvailable = false;
        return;
    }

    auto deviceIterator = IOService::getMatchingServices(dict);
    dict->release();

    if (!deviceIterator) {
        SYSLOG("ec0", "No iterator");
        isTACHAvailable = false;
        return;
    }

    ec0Device = OSDynamicCast(IOACPIPlatformDevice, deviceIterator->getNextObject());
    deviceIterator->release();

    if (!ec0Device) {
        SYSLOG("ec0", "PNP0C09 device not found");
        isTACHAvailable = false;
        return;
    }

    hasRawTachometer =
        ec0Device->validateObject("RTAH") == kIOReturnSuccess &&
        ec0Device->validateObject("ECAV") == kIOReturnSuccess;

    if (ec0Device->validateObject("TACH") != kIOReturnSuccess) {
        SYSLOG("ec0", "No functional method TACH on EC0 device");
        isTACHAvailable = false;
        return;
    }
    SYSLOG("ec0", "Found EC0 Device %s", ec0Device->getName());
}

int AsusSMC::readScreenPadValue(uint32_t device) {
    int status = wmi_get_devstate(device);
    if (status == -1 || !(status & ASUS_WMI_DSTS_PRESENCE_BIT)) {
        return -1;
    }
    return status & 0xff;
}

void AsusSMC::readScreenPadState() {
    int power = readScreenPadValue(ASUS_WMI_DEVID_SCREENPAD);
    if (power < 0) {
        if (screenPadSupported) {
            screenPadPower = -1;
            removeProperty("ScreenPadEnabled");
            removeProperty("ScreenPadBrightness");
        }
        return;
    }

    // An unlit panel advertises backlight support, but its brightness byte
    // is meaningful only while powered. Keep the last level while off.
    int level = readScreenPadValue(ASUS_WMI_DEVID_SCREENPAD_LIGHT);
    if (!screenPadSupported) {
        if (level < 0) {
            return;
        }
        screenPadSupported = true;
        setProperty("ScreenPadSupported", true);
    }
    screenPadPower = power != 0;
    setProperty("ScreenPadEnabled", power != 0);
    if (power != 0) {
        screenPadBrightness = level;
    }
    if (screenPadBrightness >= 0) {
        setProperty("ScreenPadBrightness", screenPadBrightness, 8);
    } else {
        removeProperty("ScreenPadBrightness");
    }
}

bool AsusSMC::setScreenPadBrightness(uint8_t level) {
    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
            ASUS_WMI_DEVID_SCREENPAD_LIGHT, level) != 1) {
        SYSLOG("screenpad", "Firmware declined brightness %u", level);
        return false;
    }
    screenPadBrightness = level;
    setProperty("ScreenPadBrightness", level, 8);
    return true;
}

void AsusSMC::setScreenPadPower(bool enabled) {
    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
            ASUS_WMI_DEVID_SCREENPAD, enabled ? 1 : 0) != 1) {
        SYSLOG("screenpad", "Firmware declined power %s",
            enabled ? "on" : "off");
        readScreenPadState();
        return;
    }
    screenPadPower = enabled;
    setProperty("ScreenPadEnabled", enabled);
    if (enabled && (screenPadBrightness < 0 ||
            !setScreenPadBrightness(static_cast<uint8_t>(screenPadBrightness)))) {
        // Power-on may restore the panel's own level if none was available
        // at boot or firmware declined our second write.
        readScreenPadState();
    }
}

void AsusSMC::cycleScreenPadBrightness() {
    if (screenPadPower != 1) {
        return;
    }
    int level = screenPadBrightness;
    if (level < 0) {
        level = readScreenPadValue(ASUS_WMI_DEVID_SCREENPAD_LIGHT);
        if (level < 0) {
            return;
        }
    }
    bool dim = level == ScreenPadMaxBrightness ||
        (level > 0 && screenPadDimming);
    int next = dim ? max(0, level - ScreenPadBrightnessStep) :
        min(ScreenPadMaxBrightness, level + ScreenPadBrightnessStep);
    if (setScreenPadBrightness(static_cast<uint8_t>(next))) {
        screenPadDimming = dim;
        kev.sendMessage(kDaemonScreenPadBrightness, next,
                        ScreenPadMaxBrightness);
    }
}

void AsusSMC::stepScreenPadBrightness(bool brighter) {
    int level = screenPadBrightness;
    if (level < 0) {
        level = readScreenPadValue(ASUS_WMI_DEVID_SCREENPAD_LIGHT);
        if (level < 0) {
            return;
        }
    }
    // Firmware can start between detents or at a level set by Switch Windows.
    int step = (level * BrightnessSteps + ScreenPadMaxBrightness / 2) /
        ScreenPadMaxBrightness;
    step = brighter ? min(step + 1, BrightnessSteps) : max(step - 1, 0);
    int next = (step * ScreenPadMaxBrightness + BrightnessSteps / 2) /
        BrightnessSteps;
    if (next == level || setScreenPadBrightness(static_cast<uint8_t>(next))) {
        kev.sendMessage(kDaemonScreenPadBrightness, step, BrightnessSteps);
    }
}

void AsusSMC::handleBrightnessKey(bool brighter) {
    if (command_gate && command_gate->runAction(
            [](OSObject *owner, void *arg, void *, void *, void *) {
                return static_cast<AsusSMC *>(owner)->routeBrightnessKey(
                    *static_cast<bool *>(arg));
            }, &brighter) == kIOReturnSuccess) {
        return;
    }
    dispatchTCReport(brighter ? kHIDUsage_AV_TopCase_BrightnessUp :
                                kHIDUsage_AV_TopCase_BrightnessDown);
}

IOReturn AsusSMC::routeBrightnessKey(bool brighter) {
    if (suspended) {
        return kIOReturnSuccess;
    }
    if (brightnessAgent == BrightnessAgent::Offline || !screenPadSupported ||
        screenPadPower != 1) {
        return kIOReturnUnsupported;
    }

    auto locked = IORegistryEntry::getRegistryRoot()->copyProperty(
        "IOConsoleLocked");
    bool atDesktop = locked == kOSBooleanFalse;
    OSSafeReleaseNULL(locked);
    if (!atDesktop) {
        return kIOReturnUnsupported;
    }

    brightnessRequestID = (brightnessRequestID % INT32_MAX) + 1;
    if (brightnessAgent == BrightnessAgent::Unresponsive) {
        // A late reply can rearm routing for the next key. This press goes
        // straight to macOS while the agent is unavailable.
        kev.sendMessage(kDaemonChooseBrightnessDisplay, brightnessRequestID, 0);
        return kIOReturnUnsupported;
    }
    BrightnessRequest request {};
    request.id = brightnessRequestID;
    LIST_INSERT_HEAD(&brightnessRequests, &request, link);

    // Release the gate while the login agent checks the pointer. If it has
    // exited or cannot reply promptly, this press takes the native HID path.
    uint64_t deadline;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    if (kev.sendMessage(kDaemonChooseBrightnessDisplay, request.id, 0)) {
        while (!request.answered && !suspended) {
            if (command_gate->commandSleep(&request, deadline, THREAD_UNINT) !=
                THREAD_AWAKENED) {
                break;
            }
        }
    }
    LIST_REMOVE(&request, link);
    if (!request.answered && request.id == brightnessRequestID) {
        brightnessAgent = BrightnessAgent::Unresponsive;
    }
    if (suspended) {
        return kIOReturnSuccess;
    }
    if (!request.screenPad || screenPadPower != 1) {
        return kIOReturnUnsupported;
    }
    stepScreenPadBrightness(brighter);
    return kIOReturnSuccess;
}

IOReturn AsusSMC::acceptBrightnessReply(OSDictionary *properties) {
    OSObject *ready = properties->getObject(AsusSMCBrightnessRoutingReady);
    auto reply = OSDynamicCast(OSNumber,
        properties->getObject(AsusSMCBrightnessRoutingReply));
    if (properties->getCount() != 1 || (ready && ready != kOSBooleanTrue) ||
        (!ready && (reply == nullptr || reply->unsigned64BitValue() < 2 ||
                    reply->unsigned64BitValue() > UINT32_MAX))) {
        return kIOReturnBadArgument;
    }
    // Only the active GUI session may choose a display for a physical press.
    if (IOUserClient::clientHasPrivilege(current_task(),
            ready ? kIOClientPrivilegeLocalUser : "consolesession") !=
        kIOReturnSuccess) {
        return kIOReturnNotPrivileged;
    }
    if (!command_gate) {
        return kIOReturnNotReady;
    }
    uint32_t value = ready ? 0 : reply->unsigned32BitValue();
    return command_gate->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) {
            auto self = static_cast<AsusSMC *>(owner);
            uint32_t reply = *static_cast<uint32_t *>(arg);
            if (reply == 0) {
                self->brightnessAgent = BrightnessAgent::Ready;
                return kIOReturnSuccess;
            }
            BrightnessRequest *request;
            LIST_FOREACH(request, &self->brightnessRequests, link) {
                if (request->id == (reply >> 1) && !request->answered) {
                    self->brightnessAgent = BrightnessAgent::Ready;
                    request->screenPad = (reply & 1) != 0;
                    request->answered = true;
                    self->command_gate->commandWakeup(request);
                    return kIOReturnSuccess;
                }
            }
            if (self->brightnessAgent == BrightnessAgent::Unresponsive &&
                self->brightnessRequestID == (reply >> 1)) {
                self->brightnessAgent = BrightnessAgent::Ready;
            }
            return kIOReturnNotFound;
        }, &value);
}

void AsusSMC::initBattery() {
    const bool hasBatteryHealth = getKernelVersion() > KernelVersion::Catalina ||
        (getKernelVersion() == KernelVersion::Catalina && getKernelMinorVersion() >= 5);
    uint32_t enabled = 0;
    const bool bindMyAsusKey =
        atkDevice->evaluateInteger("BKEY", &enabled) == kIOReturnSuccess && enabled == 1;
    if ((!hasBatteryHealth && !bindMyAsusKey) || !wmi_dev_is_present(ASUS_WMI_DEVID_RSOC)) {
        return;
    }

    batteryHotkeyAvailable = bindMyAsusKey;
    publishBatteryHealthKey = hasBatteryHealth;
    if (hasBatteryHealth) {
        atomic_store(&batteryLimit.pending, 80);
        applyBatteryLimit();
    }
}

void AsusSMC::initVirtualKeyboard() {
    kbdDevice = new VirtualAppleKeyboard;

    if (!kbdDevice || !kbdDevice->init() || !kbdDevice->attach(this) || !kbdDevice->start(this)) {
        OSSafeReleaseNULL(kbdDevice);
        SYSLOG("vkbd", "Failed to init VirtualAppleKeyboard");
    }
}

void AsusSMC::readKeyboardBacklightDirection() {
    if (hasKeyboardBacklight) {
        int status = wmi_get_devstate(ASUS_WMI_DEVID_KBD_BACKLIGHT);
        if (status >= 0 && (status & ASUS_WMI_DSTS_PRESENCE_BIT)) {
            // DSTS returns the current level in its low byte. After startup
            // or wake, the first press dims a lit keyboard.
            keyboardBacklightDimming = (status & 0xff) != 0;
        }
    }
}

void AsusSMC::startATKDevice() {
    // Check direct ACPI messaging support
    if (atkDevice->validateObject("DMES") == kIOReturnSuccess) {
        DBGLOG("atk", "Direct ACPI message is supported");
        setProperty("IsDirectACPIMessagingSupported", kOSBooleanTrue);
        directACPImessaging = true;
    }

    // Check keyboard backlight support
    if (atkDevice->validateObject("SKBV") == kIOReturnSuccess) {
        SYSLOG("atk", "Keyboard backlight is supported");
        hasKeyboardBacklight = true;
    } else {
        hasKeyboardBacklight = false;
        DBGLOG("atk", "Keyboard backlight is not supported");
    }
    setProperty("IsKeyboardBacklightSupported", hasKeyboardBacklight);
    readKeyboardBacklightDirection();

    // Turn on ALS sensor
    toggleALS(true);
    isALSEnabled = true;
    setProperty("IsALSEnabled", isALSEnabled);
    SYSLOG("atk", "ALS is turned on at boot");
}

bool AsusSMC::refreshALS(bool post) {
    if (!alsDevice) {
        return false;
    }

    IOReturn ret = kIOReturnSuccess;
    uint32_t lux = 150;

    if (isALSEnabled) {
        ret = alsDevice->evaluateInteger("_ALI", &lux);
        if (ret != kIOReturnSuccess) {
            lux = 0xFFFFFFFF; // ACPI invalid
        }
    }

    atomic_store_explicit(&currentLux, lux, memory_order_release);

    if (post) {
        VirtualSMCAPI::postInterrupt(SmcEventALSChange);
    }

    DBGLOG("als", "refreshALS lux %u", lux);

    return ret == kIOReturnSuccess;
}

void AsusSMC::initFans() {
    PE_parse_boot_argn("asussmcfans", &fanCount, sizeof(fanCount));
    if (fanCount == 0 || fanCount > MaxFanCount) {
        SYSLOG("fan", "asussmcfans must be 1 or 2");
        fanCount = 1;
    }

    useRawFanSpeed = checkKernelArgument("-asussmcrawfan");
    if (useRawFanSpeed && !hasRawTachometer) {
        SYSLOG("fan", "-asussmcrawfan requires RTAH and ECAV methods");
    }
    setProperty("FanSpeedMethod", useRawFanSpeed ? "RTAH" :
        (isTACHAvailable ? "TACH" : "WMI"));
    setProperty("FanCount", fanCount, 32);
    refreshFan();
    fanControlSupported = wmi_dev_is_present(ASUS_WMI_DEVID_CPU_FAN_CTRL);
    if (fanControlSupported &&
        applyFanMode(FanModeAutomatic) != kIOReturnSuccess) {
        SYSLOG("fan", "Automatic fan control is not ready at startup");
    }
}

bool AsusSMC::readFanSpeed(uint32_t index, uint32_t &speed) {
    if (useRawFanSpeed) {
        uint32_t available = 0;
        if (!hasRawTachometer ||
            ec0Device->evaluateInteger("ECAV", &available) !=
                kIOReturnSuccess ||
            available == 0) {
            return false;
        }
    } else if (!isTACHAvailable) {
        uint32_t id = index == 0 ? ASUS_WMI_DEVID_CPU_FAN_CTRL :
            ASUS_WMI_DEVID_GPU_FAN_CTRL;
        int status = wmi_get_devstate(id);
        if (status == -1 || !(status & ASUS_WMI_DSTS_PRESENCE_BIT)) {
            return false;
        }
        speed = (status & 0xffff) * 100;
        return speed <= Fpe2MaxRpm;
    }

    auto arg = OSNumber::withNumber(index, 32);
    if (arg == nullptr) {
        return false;
    }
    OSObject *args[] {arg};
    // UX481 WMI reports RTAH / 100 in hundred-RPM units, but TACH treats
    // RTAH as a period. Read RTAH directly when its value is already RPM.
    const char *method = useRawFanSpeed ? "RTAH" : "TACH";
    IOReturn ret = ec0Device->evaluateInteger(method, &speed, args, 1);

    // TACH returns Ones for both a stopped fan and an unavailable reading;
    // confirm a zero RTAH value before reporting a stopped fan.
    if (!useRawFanSpeed && ret == kIOReturnSuccess &&
        speed == InvalidFanSpeed && hasRawTachometer) {
        uint32_t available = 0;
        uint32_t rawSpeed = UINT32_MAX;
        if (ec0Device->evaluateInteger("ECAV", &available) ==
                kIOReturnSuccess &&
            available != 0 &&
            ec0Device->evaluateInteger("RTAH", &rawSpeed, args, 1) ==
                kIOReturnSuccess && rawSpeed == 0) {
            speed = 0;
        }
    }
    arg->release();
    return ret == kIOReturnSuccess && speed <= Fpe2MaxRpm;
}

void AsusSMC::refreshFan() {
    if (suspended) {
        return;
    }
    for (uint32_t i = 0; i < fanCount; ++i) {
        uint32_t speed = InvalidFanSpeed;
        if (!readFanSpeed(i, speed)) {
            speed = InvalidFanSpeed;
        }
        atomic_store_explicit(&currentFanSpeed[i], speed, memory_order_release);

        char property[16];
        snprintf(property, sizeof(property), "Fan%uRPM", i);
        if (speed == InvalidFanSpeed) {
            removeProperty(property);
        } else {
            setProperty(property, speed, 32);
        }
    }
}

IOReturn AsusSMC::applyFanMode(uint32_t mode) {
    if (!fanControlSupported) {
        return kIOReturnUnsupported;
    }
    if (mode > FanModeFullSpeed) {
        return kIOReturnBadArgument;
    }

    // Some firmware returns success even when its ECAV guard skips the write.
    if (ec0Device &&
        ec0Device->validateObject("ECAV") == kIOReturnSuccess) {
        uint32_t ready = 0;
        bool available =
            ec0Device->evaluateInteger("ECAV", &ready) == kIOReturnSuccess &&
            ready != 0;
        setProperty("FanECReady", available);
        if (!available) {
            return kIOReturnNotReady;
        }
    }

    int result = wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
                                    ASUS_WMI_DEVID_CPU_FAN_CTRL, mode);
    if (result != 1) {
        return kIOReturnIOError;
    }
    setProperty("FanMode", mode, 32);
    return kIOReturnSuccess;
}

IOReturn AsusSMC::setProperties(OSObject *properties) {
    auto dict = OSDynamicCast(OSDictionary, properties);
    if (dict && (dict->getObject(AsusSMCBrightnessRoutingReady) ||
                 dict->getObject(AsusSMCBrightnessRoutingReply))) {
        return acceptBrightnessReply(dict);
    }
    OSObject *value = dict ? dict->getObject("FanMode") : nullptr;
    if (!value) {
        return super::setProperties(properties);
    }
    auto mode = OSDynamicCast(OSNumber, value);
    if (mode == nullptr || mode->unsigned64BitValue() > FanModeFullSpeed) {
        return kIOReturnBadArgument;
    }
    if (IOUserClient::clientHasPrivilege(current_task(),
        kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        return kIOReturnNotPrivileged;
    }
    if (!command_gate) {
        return kIOReturnNotReady;
    }

    uint32_t requested = mode->unsigned32BitValue();
    return command_gate->runAction(
        [](OSObject *owner, void *argument, void *, void *, void *) {
            auto self = static_cast<AsusSMC *>(owner);
            if (self->suspended) {
                return kIOReturnNotReady;
            }
            return self->applyFanMode(*static_cast<uint32_t *>(argument));
        }, &requested);
}

void AsusSMC::updatePowerState(bool sleeping) {
    if (!command_gate) {
        return;
    }
    IOReturn ret = command_gate->runAction(
        [](OSObject *owner, void *argument, void *, void *, void *) {
            auto self = static_cast<AsusSMC *>(owner);
            bool wasSuspended = self->suspended;
            self->suspended = *static_cast<bool *>(argument);
            if (self->suspended) {
                BrightnessRequest *request;
                LIST_FOREACH(request, &self->brightnessRequests, link) {
                    request->answered = true;
                    request->screenPad = false;
                    self->command_gate->commandWakeup(request);
                }
            }
            if (wasSuspended && !self->suspended) {
                self->readKeyboardBacklightDirection();
                if (self->fnLockMode >= 0) {
                    self->setFnLock(self->fnLockMode != 0);
                }
                if (self->screenPadSupported) {
                    if (self->screenPadPower >= 0) {
                        self->setScreenPadPower(self->screenPadPower != 0);
                    } else {
                        self->readScreenPadState();
                    }
                }
            }
            if (!self->fanControlSupported) {
                return kIOReturnSuccess;
            }
            return self->applyFanMode(FanModeAutomatic);
        }, &sleeping);
    if (ret != kIOReturnSuccess) {
        SYSLOG("fan", "Failed to restore automatic fan control: %x", ret);
    }
}

IOReturn AsusSMC::setPowerState(unsigned long state, IOService *) {
    updatePowerState(state == 0);
    return IOPMAckImplied;
}

void AsusSMC::systemWillShutdown(IOOptionBits specifier) {
    updatePowerState(true);
    super::systemWillShutdown(specifier);
}

void AsusSMC::handleMessage(int code) {
    switch (code) {
        case 0x57: // AC disconnected
        case 0x58: // AC connected
            // ignore silently
            break;

        case 0x30: // Volume up
            dispatchCSMRReport(kHIDUsage_Csmr_VolumeIncrement);
            break;

        case 0x31: // Volume down
            dispatchCSMRReport(kHIDUsage_Csmr_VolumeDecrement);
            break;

        case 0x32: // Mute
            dispatchCSMRReport(kHIDUsage_Csmr_Mute);
            break;

        // Media buttons
        case 0x40:
        case 0x8A:
            dispatchCSMRReport(kHIDUsage_Csmr_ScanPreviousTrack);
            break;

        case 0x41:
        case 0x82:
            dispatchCSMRReport(kHIDUsage_Csmr_ScanNextTrack);
            break;

        case 0x45:
        case 0x5C:
            dispatchCSMRReport(kHIDUsage_Csmr_PlayOrPause);
            break;

        case 0x33: // hardwired On
        case 0x34: // hardwired Off
        case 0x35: // Soft Event, Fn + F7
            displayOff();
            break;

        case 0x9C: // Switch Windows button on ScreenPad models
        case 0x6A: // ScreenPad power button
            if (command_gate) {
                command_gate->runAction(
                    [](OSObject *owner, void *argument, void *, void *, void *) {
                        auto self = static_cast<AsusSMC *>(owner);
                        if (self->suspended) {
                            return kIOReturnNotReady;
                        }
                        if (self->screenPadPower < 0) {
                            self->readScreenPadState();
                            if (self->screenPadPower < 0) {
                                return kIOReturnNotReady;
                            }
                        }
                        if (*static_cast<int *>(argument) == 0x9C) {
                            self->cycleScreenPadBrightness();
                        } else {
                            self->setScreenPadPower(self->screenPadPower == 0);
                        }
                        return kIOReturnSuccess;
                    }, &code);
            }
            break;

        case 0x4E: // Fn Lock
        case 0x86: // MyASUS (Fn+F12 on the UX481)
            if (command_gate && (code == 0x4E ? fnLockSupported : batteryHotkeyAvailable)) {
                command_gate->runAction(
                    [](OSObject *owner, void *argument, void *, void *, void *) {
                        auto self = static_cast<AsusSMC *>(owner);
                        if (self->suspended) {
                            return kIOReturnNotReady;
                        }
                        if (*static_cast<int *>(argument) == 0x86) {
                            self->applyBatteryLimit(true);
                        } else {
                            self->setFnLock(self->fnLockMode <= 0);
                        }
                        return kIOReturnSuccess;
                    }, &code);
            }
            break;

        case 0x61: // Video Mirror
            dispatchTCReport(kHIDUsage_AV_TopCase_VideoMirror);
            break;

        case 0x6B: // Fn + F9, Touchpad On/Off
            toggleTouchpad();
            break;

        case 0x5E:
            letSleep();
            break;

        case 0x7A: // Fn + A, ALS Sensor
            // We should really do this in userspace
            // (e.g. "Automatically adjust brightness")
            isALSEnabled = !isALSEnabled;
            setProperty("IsALSEnabled", isALSEnabled);
            break;

        case 0x7D: // Airplane mode
            toggleAirplaneMode();
            break;

        case 0xC7: // Cycle keyboard illumination
            if (hasKeyboardBacklight) {
                dispatchTCReport(keyboardBacklightDimming ?
                    kHIDUsage_AV_TopCase_IlluminationDown :
                    kHIDUsage_AV_TopCase_IlluminationUp);
            }
            break;

        case 0xC5: // Keyboard Backlight Down
            if (hasKeyboardBacklight) {
                dispatchTCReport(kHIDUsage_AV_TopCase_IlluminationDown);
            }
            break;

        case 0xC4: // Keyboard Backlight Up
            if (hasKeyboardBacklight) {
                dispatchTCReport(kHIDUsage_AV_TopCase_IlluminationUp);
            }
            break;

        default:
            if (code >= NOTIFY_BRIGHTNESS_DOWN_MIN &&
                code <= NOTIFY_BRIGHTNESS_DOWN_MAX) {
                handleBrightnessKey(false);
            } else if (code >= NOTIFY_BRIGHTNESS_UP_MIN &&
                       code <= NOTIFY_BRIGHTNESS_UP_MAX) {
                handleBrightnessKey(true);
            }
            break;
    }

    DBGLOG("atk", "Received key %d(0x%x)", code, code);
}

void AsusSMC::letSleep() {
    kev.sendMessage(kDaemonSleep, 0, 0);
}

void AsusSMC::toggleAirplaneMode() {
    kev.sendMessage(kDaemonAirplaneMode, 0, 0);
}

void AsusSMC::toggleTouchpad() {
    dispatchMessage(kKeyboardGetTouchStatus, &isTouchpadEnabled);
    isTouchpadEnabled = !isTouchpadEnabled;
    dispatchMessage(kKeyboardSetTouchStatus, &isTouchpadEnabled);

    if (isTouchpadEnabled) {
        setProperty("IsTouchpadEnabled", true);
        DBGLOG("atk", "Enabled Touchpad");
    } else {
        setProperty("IsTouchpadEnabled", false);
        DBGLOG("atk", "Disabled Touchpad");
    }
}

void AsusSMC::toggleALS(bool state) {
    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, ASUS_WMI_DEVID_ALS_ENABLE, state ? 1 : 0) == -1) {
        SYSLOG("atk", "Failed to %s ALSC", state ? "enable" : "disable");
    } else {
        DBGLOG("atk", "ALS is %s", state ? "enabled" : "disabled");
    }
}

void AsusSMC::applyBatteryLimit(bool toggle) {
    if (suspended) {
        return;
    }

    int limit = atomic_exchange(&batteryLimit.pending, -1);
    if (toggle) {
        // If a BDVT write is pending, the keypress reverses that request
        // before the work loop sends a limit to firmware.
        const int previous = limit >= 0 ? limit : atomic_load(&batteryLimit.applied);
        limit = previous == 80 ? 100 : 80;
    }
    if (limit < 0) {
        return;
    }

    if (wmi_evaluate_method(ASUS_WMI_METHODID_DEVS, ASUS_WMI_DEVID_RSOC, limit) != 1) {
        SYSLOG("batt", "Failed to set the battery charge limit to %d%%", limit);
        return;
    }
    atomic_store(&batteryLimit.applied, limit);
    setProperty("BatteryConservativeMode", limit == 80);
}

void AsusSMC::displayOff() {
    if (isPanelBackLightOn) {
        // Read Panel brigthness value to restore later with backlight toggle
        readPanelBrightnessValue();

        dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessDown, 16);
    } else {
        dispatchTCReport(kHIDUsage_AV_TopCase_BrightnessUp, panelBrightnessLevel);
    }

    isPanelBackLightOn = !isPanelBackLightOn;
}

int AsusSMC::checkBacklightEntry() {
    if (IORegistryEntry *bkl = IORegistryEntry::fromPath(backlightEntry)) {
        OSSafeReleaseNULL(bkl);
        return 1;
    } else {
        DBGLOG("atk", "Failed to find backlight entry for %s", backlightEntry);
        return 0;
    }
}

int AsusSMC::findBacklightEntry() {
    // Check for previous found backlight entry
    if (checkBacklightEntry()) {
        return 1;
    }

    snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/IGPU@2/AppleIntelFramebuffer@0/display0/AppleBacklightDisplay");
    if (checkBacklightEntry()) {
        return 1;
    }

    snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2/AppleIntelFramebuffer@0/display0/AppleBacklightDisplay");
    if (checkBacklightEntry()) {
        return 1;
    }

    char deviceName[5][5] = {"PEG0", "PEGP", "PEGR", "P0P2", "IXVE"};
    for (int i = 0; i < 5; i++) {
        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@1/IOPP/GFX0@0/NVDA,Display-A@0/NVDA/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@3/IOPP/GFX0@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@10/IOPP/GFX0@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@1/IOPP/display@0/NVDA,Display-A@0/NVDA/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@3/IOPP/display@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }

        snprintf(backlightEntry, sizeof(backlightEntry), "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/%s@10/IOPP/display@0/NVDA,Display-A@0/NVDATesla/display0/AppleBacklightDisplay", deviceName[i]);
        if (checkBacklightEntry()) {
            return 1;
        }
    }

    return 0;
}

void AsusSMC::readPanelBrightnessValue() {
    if (!findBacklightEntry()) {
        DBGLOG("atk", "GPU device not found");
        return;
    }

    IORegistryEntry *displayDeviceEntry = IORegistryEntry::fromPath(backlightEntry);

    if (displayDeviceEntry) {
        if (OSDictionary *ioDisplayParaDict = OSDynamicCast(OSDictionary, displayDeviceEntry->getProperty("IODisplayParameters"))) {
            if (OSDictionary *brightnessDict = OSDynamicCast(OSDictionary, ioDisplayParaDict->getObject("brightness"))) {
                if (OSNumber *brightnessValue = OSDynamicCast(OSNumber, brightnessDict->getObject("value"))) {
                    panelBrightnessLevel = brightnessValue->unsigned32BitValue() / 64;
                    DBGLOG("atk", "Panel brightness level: %d", panelBrightnessLevel);
                } else {
                    DBGLOG("atk", "Failed to read brightness value");
                }
            } else {
                DBGLOG("atk", "Failed to find dictionary brightness");
            }
        } else {
            DBGLOG("atk", "Failed to find dictionary IODisplayParameters");
        }
    }
    OSSafeReleaseNULL(displayDeviceEntry);
}

IOReturn AsusSMC::postKeyboardInputReport(const void *report, uint32_t reportSize) {
    IOReturn result = kIOReturnError;

    if (!report || reportSize == 0) {
        return kIOReturnBadArgument;
    }

    if (kbdDevice) {
        if (auto buffer = IOBufferMemoryDescriptor::withBytes(report, reportSize, kIODirectionNone)) {
            result = kbdDevice->handleReport(buffer, kIOHIDReportTypeInput, kIOHIDOptionsTypeNone);
            buffer->release();
        }
    }

    return result;
}

void AsusSMC::dispatchCSMRReport(int code, int loop) {
    DBGLOG("atk", "Dispatched key %d(0x%x), loop %d time(s)", code, code, loop);
    while (loop--) {
        csmrreport.keys.insert(code);
        postKeyboardInputReport(&csmrreport, sizeof(csmrreport));
        csmrreport.keys.erase(code);
        postKeyboardInputReport(&csmrreport, sizeof(csmrreport));
    }
}

void AsusSMC::dispatchTCReport(int code, int loop) {
    DBGLOG("atk", "Dispatched key %d(0x%x), loop %d time(s)", code, code, loop);
    while (loop--) {
        tcreport.keys.insert(code);
        postKeyboardInputReport(&tcreport, sizeof(tcreport));
        tcreport.keys.erase(code);
        postKeyboardInputReport(&tcreport, sizeof(tcreport));
    }
}

void AsusSMC::registerNotifications() {
    auto *key = OSSymbol::withCString(kDeliverNotifications);
    auto *propertyMatch = propertyMatching(key, kOSBooleanTrue);

    IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &AsusSMC::notificationHandler);

    _publishNotify = addMatchingNotification(gIOFirstPublishNotification,
                                             propertyMatch,
                                             notificationHandler,
                                             this,
                                             0, 10000);

    _terminateNotify = addMatchingNotification(gIOTerminatedNotification,
                                               propertyMatch,
                                               notificationHandler,
                                               this,
                                               0, 10000);

    key->release();
    propertyMatch->release();
}

void AsusSMC::notificationHandlerGated(IOService *newService, IONotifier *notifier) {
    if (notifier == _publishNotify) {
        SYSLOG("notify", "Notification consumer published: %s", newService->getName());
        _notificationServices->setObject(newService);
    }

    if (notifier == _terminateNotify) {
        SYSLOG("notify", "Notification consumer terminated: %s", newService->getName());
        _notificationServices->removeObject(newService);
    }
}

bool AsusSMC::notificationHandler(void *refCon, IOService *newService, IONotifier *notifier) {
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &AsusSMC::notificationHandlerGated), newService, notifier);
    return true;
}

void AsusSMC::dispatchMessageGated(int *message, void *data) {
    OSCollectionIterator *i = OSCollectionIterator::withCollection(_notificationServices);

    if (i != NULL) {
        while (IOService *service = OSDynamicCast(IOService, i->getNextObject())) {
            service->message(*message, this, data);
        }
        i->release();
    }
}

void AsusSMC::dispatchMessage(int message, void *data) {
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &AsusSMC::dispatchMessageGated), &message, data);
}

void AsusSMC::registerVSMC() {
    ALSSensor sensor {ALSSensor::Type::Unknown7, true, 6, false};
    ALSSensor noSensor {ALSSensor::Type::NoSensor, false, 0, false};
    SMCALSValue::Value emptyValue;
    SMCKBrdBLightValue::lkb lkb;
    SMCKBrdBLightValue::lks lks;

    VirtualSMCAPI::addKey(KeyAL, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(
        0, &forceBits,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));

    VirtualSMCAPI::addKey(KeyALI0, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&sensor), sizeof(sensor), SmcKeyTypeAli, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyALI1, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&noSensor), sizeof(noSensor), SmcKeyTypeAli, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyALRV, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(
        1, nullptr,
        SMC_KEY_ATTRIBUTE_READ));

    VirtualSMCAPI::addKey(KeyALV0, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, new SMCALSValue(&currentLux, &forceBits),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyALV1, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&emptyValue), sizeof(emptyValue), SmcKeyTypeAlv, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyLKSB, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&lkb), sizeof(lkb), SmcKeyTypeLkb, new SMCKBrdBLightValue(this),
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyLKSS, vsmcPlugin.data, VirtualSMCAPI::valueWithData(
        reinterpret_cast<const SMC_DATA *>(&lks), sizeof(lks), SmcKeyTypeLks, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyMSLD, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
        0, nullptr,
        SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_FUNCTION));

    VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(
        fanCount, nullptr,
        SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

    for (uint32_t i = 0; i < fanCount; ++i) {
        char index = '0' + i;
        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('F', index, 'A', 'c'),
            vsmcPlugin.data, VirtualSMCAPI::valueWithFp(
                0, SmcKeyTypeFpe2, new FanSpeed(&currentFanSpeed[i]),
                SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_FUNCTION));

        FanTypeDescStruct desc {};
        if (fanCount == 1) {
            lilu_os_strncpy(desc.strFunction, "System Fan", DiagFunctionStrLen);
        } else {
            snprintf(desc.strFunction, DiagFunctionStrLen, "Fan %u", i + 1);
        }
        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('F', index, 'I', 'D'),
            vsmcPlugin.data, VirtualSMCAPI::valueWithData(
                reinterpret_cast<const SMC_DATA *>(&desc), sizeof(desc),
                SmcKeyTypeFds, nullptr,
                SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));
    }

    if (publishBatteryHealthKey) {
        VirtualSMCAPI::addKey(KeyBDVT, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(
            false, new BDVT(batteryLimit),
            SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_ATOMIC));
    }

    qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
    vsmcNotifier =
        VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
}

bool AsusSMC::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
    if (sensors && vsmc) {
        DBGLOG("atk", "got vsmc notification");
        auto self = static_cast<AsusSMC *>(sensors);
        auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &self->vsmcPlugin, nullptr, nullptr);
        if (ret == kIOReturnSuccess) {
            DBGLOG("atk", "Submitted plugin");

            self->workloop = self->getWorkLoop();
            self->poller = IOTimerEventSource::timerEventSource(self, [](OSObject *object, IOTimerEventSource *sender) {
                auto ls = OSDynamicCast(AsusSMC, object);
                if (ls) {
                    ls->applyBatteryLimit();
                    ls->refreshALS(true);
                    ls->refreshFan();
                    sender->setTimeoutMS(SensorUpdateTimeoutMS);
                }
            });

            if (!self->poller || !self->workloop) {
                SYSLOG("atk", "Failed to create poller or workloop");
                return false;
            }

            if (self->workloop->addEventSource(self->poller) != kIOReturnSuccess) {
                SYSLOG("atk", "Failed to add timer event source to workloop");
                return false;
            }

            if (self->poller->setTimeoutMS(SensorUpdateTimeoutMS) != kIOReturnSuccess) {
                SYSLOG("atk", "Failed to set timeout");
                return false;
            }

            atomic_store(&self->batteryLimit.acceptingWrites, true);
            return true;
        } else if (ret != kIOReturnUnsupported) {
            SYSLOG("atk", "Plugin submission failure %X", ret);
        } else {
            DBGLOG("atk", "Plugin submission to non vsmc");
        }
    } else {
        SYSLOG("atk", "Got null vsmc notification");
    }

    return false;
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg") || checkKernelArgument("-asussmcdbg");
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}

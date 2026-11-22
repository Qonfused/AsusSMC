#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool readNumber(io_registry_entry_t service, CFStringRef key,
                       int32_t *value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(
        service, key, kCFAllocatorDefault, 0);
    bool valid = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
        CFNumberGetValue(property, kCFNumberSInt32Type, value);
    if (property) {
        CFRelease(property);
    }
    return valid;
}

static void showStatus(io_registry_entry_t service) {
    CFTypeRef supported = IORegistryEntryCreateCFProperty(
        service, CFSTR("FanControlSupported"), kCFAllocatorDefault, 0);
    printf("Fan control: %s\n",
           supported == kCFBooleanTrue ? "available" : "unavailable");
    if (supported) {
        CFRelease(supported);
    }

    CFTypeRef ready = IORegistryEntryCreateCFProperty(
        service, CFSTR("FanECReady"), kCFAllocatorDefault, 0);
    if (ready == kCFBooleanFalse) {
        printf("Embedded controller: not ready\n");
    }
    if (ready) {
        CFRelease(ready);
    }

    int32_t mode;
    if (readNumber(service, CFSTR("FanMode"), &mode) &&
        mode >= 0 && mode <= 1) {
        printf("Mode: %s\n", mode == 0 ? "Automatic" : "Full Speed");
    }

    int32_t count;
    if (!readNumber(service, CFSTR("FanCount"), &count)) {
        return;
    }
    for (int32_t i = 0; i < count && i < 2; ++i) {
        CFStringRef key = CFStringCreateWithFormat(
            kCFAllocatorDefault, NULL, CFSTR("Fan%dRPM"), i);
        if (!key) {
            continue;
        }
        int32_t rpm;
        if (readNumber(service, key, &rpm)) {
            printf("Fan %d: %d RPM\n", i + 1, rpm);
        } else {
            printf("Fan %d: unavailable\n", i + 1);
        }
        CFRelease(key);
    }
}

int main(int argc, const char *argv[]) {
    const char *command = argc == 1 ? "status" : argv[1];
    bool status = strcmp(command, "status") == 0;
    bool automatic = strcmp(command, "auto") == 0;
    bool full = strcmp(command, "full") == 0;
    if (argc > 2 || (!status && !automatic && !full)) {
        fprintf(stderr, "Usage: %s [status|auto|full]\n", argv[0]);
        return 2;
    }
    if (!status && geteuid() != 0) {
        fprintf(stderr, "Run with sudo to change the fan mode.\n");
        return 1;
    }

    io_service_t service = IOServiceGetMatchingService(
        MACH_PORT_NULL, IOServiceMatching("AsusSMC"));
    if (!service) {
        fprintf(stderr, "AsusSMC is not loaded.\n");
        return 1;
    }

    if (!status) {
        int32_t mode = full ? 1 : 0;
        CFNumberRef value = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberSInt32Type, &mode);
        if (!value) {
            IOObjectRelease(service);
            return 1;
        }
        kern_return_t result = IORegistryEntrySetCFProperty(
            service, CFSTR("FanMode"), value);
        CFRelease(value);
        if (result != KERN_SUCCESS) {
            fprintf(stderr, "Fan mode change failed: %s (0x%x)\n",
                    mach_error_string(result), result);
            IOObjectRelease(service);
            return 1;
        }
    }

    showStatus(service);
    IOObjectRelease(service);
    return 0;
}

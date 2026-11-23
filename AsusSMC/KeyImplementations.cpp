//
//  KeyImplementations.cpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "KeyImplementations.hpp"
#include "AsusSMC.hpp"

SMC_RESULT SMCALSValue::readAccess() {
    auto value = reinterpret_cast<Value *>(data);
    uint32_t lux = atomic_load_explicit(currentLux, memory_order_acquire);
    uint8_t bits = forceBits->bits();

    if (lux == 0xFFFFFFFF) {
        value->valid = false;
    } else {
        value->valid = true;
        if (!(bits & ALSForceBits::kALSForceHighGain))
            value->highGain = true;
        if (!(bits & ALSForceBits::kALSForceChan))
            value->chan0 = OSSwapHostToBigInt16(lux);
        if (!(bits & ALSForceBits::kALSForceLux))
            value->roomLux = OSSwapHostToBigInt32(lux << 14);
    }

    return SmcSuccess;
}

SMC_RESULT SMCKBrdBLightValue::update(const SMC_DATA *src)  {
    lkb *value = new lkb;
    lilu_os_memcpy(value, src, size);

    // tval is in range [0x0, 0xffb]
    uint16_t tval = (value->val1 << 4) | (value->val2 >> 4);
    DBGLOG("lksb", "LKSB update %d", tval);

    delete value;

    if (asusSMCInstance) {
        asusSMCInstance->message(kSetKeyboardBacklightMessage, nullptr, &tval);
    }

    lilu_os_memcpy(data, src, size);
    return SmcSuccess;
}

SMC_RESULT FanSpeed::readAccess() {
    uint32_t speed = atomic_load_explicit(currentSpeed, memory_order_acquire);
    if (speed > Fpe2MaxRpm) {
        return SmcNotReadable;
    }

    *reinterpret_cast<uint16_t *>(data) =
        VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, speed);
    return SmcSuccess;
}

SMC_RESULT BDVT::readAccess() {
    const int applied = atomic_load(&limit.applied);
    if (applied < 0) {
        return SmcNotReadable;
    }
    // BDVT reads zero at the 80% charge limit and one for full charging.
    data[0] = applied == 100;
    return SmcSuccess;
}

SMC_RESULT BDVT::update(const SMC_DATA *src) {
    // VirtualSMC may hold a spinlock during this callback, so the work loop
    // handles the firmware write that can sleep.
    if (!atomic_load(&limit.acceptingWrites)) {
        return SmcNotWritable;
    }
    atomic_store(&limit.pending, src[0] ? 100 : 80);
    return SmcSuccess;
}

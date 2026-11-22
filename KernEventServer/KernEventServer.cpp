//
//  KernEventServer.cpp
//  KernEventServer
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "KernEventServer.hpp"
#include <Headers/kern_util.hpp>

bool KernEventServer::setVendorID(const char *vendorCode) {
    if (KERN_SUCCESS != kev_vendor_code_find(vendorCode, &vendorID)) {
        DBGLOG("kevserver", "setVendorID error");
        return false;
    }
    return true;
}

void KernEventServer::setEventCode(u_int32_t code) {
    eventCode = code;
}

bool KernEventServer::sendMessage(AsusSMCMessageType type, int x, int y) {
    AsusSMCMessage message {type, x, y};
    struct kev_msg kEventMsg {};
    kEventMsg.vendor_code = vendorID;
    kEventMsg.kev_class = KEV_ANY_CLASS;
    kEventMsg.kev_subclass = KEV_ANY_SUBCLASS;
    kEventMsg.event_code = eventCode;
    kEventMsg.dv[0].data_length = sizeof(message);
    kEventMsg.dv[0].data_ptr = &message;

    if (KERN_SUCCESS != kev_msg_post(&kEventMsg)) {
        DBGLOG("kevserver", "sendMessage error");
        return false;
    }
    return true;
}

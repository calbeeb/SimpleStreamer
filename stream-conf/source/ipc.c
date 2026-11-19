// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <kaihen.h>
#include "config.h"

Result config_open_plugin(Service* out_plugin, const char* plugin_name) {
    Service ro = {0};
    Result rc = kaihenGetRoSession(&ro);
    if (R_FAILED(rc)) {
        return rc;
    }

    Service hen = {0};
    rc = kaihenOpenHomebrewSession(&ro, &hen);
    if (R_FAILED(rc)) {
        return rc;
    }

    HenPluginName req = {0};
    strncpy(req.name, plugin_name, sizeof(req.name) - 1);

    rc = henOpenPlugin(&hen, &req, out_plugin);
    serviceClose(&hen);
    return rc;
}

void config_get_ip_str(char* out_str, size_t max_len) {
    u32 ip_addr = 0;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        nifmGetCurrentIpAddress(&ip_addr);
        nifmExit();
    }
    if (ip_addr != 0) {
        struct in_addr addr = { .s_addr = ip_addr };
        snprintf(out_str, max_len, "%s", inet_ntoa(addr));
    } else {
        snprintf(out_str, max_len, "<Switch-IP>");
    }
}

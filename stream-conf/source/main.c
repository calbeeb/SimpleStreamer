// SPDX-License-Identifier: MIT
// Companion config and telemetry .nro for stream-module.nso on Switch 2.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <kaihen.h>

typedef struct {
    u32 running;
    u32 avg_fps_x100;
    Result last_rc;
    u32 pad;
    u64 frames_captured;
    u64 last_frame_size;
} Caps2sStatus;

static Result open_kaihen_plugin(Service* out_plugin, const char* plugin_name) {
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

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    const char* plugin_name = "stream-module";
    Service pluginSrv = {0};
    bool connected = false;
    Result openRc = open_kaihen_plugin(&pluginSrv, plugin_name);
    if (R_SUCCEEDED(openRc)) {
        connected = true;
    }

    char statusMsg[128] = "Ready.";
    if (!connected) {
        snprintf(statusMsg, sizeof(statusMsg), "Failed to open %s: 0x%x", plugin_name, openRc);
    }

    u64 lastPollTick = 0;
    Caps2sStatus st = {0};

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) {
            break;
        }

        // [A] Save latest captured 1080p JPEG frame to SD card (Cmd 1)
        if ((kDown & HidNpadButton_A) && connected) {
            Result saveRc = serviceDispatch(&pluginSrv, 1);
            if (R_SUCCEEDED(saveRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Saved sdmc:/kaihen_caps2s_frame.jpg!");
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "Save failed: 0x%x", saveRc);
            }
        }

        // [X] Stop & exit background capture plugin (Cmd 2)
        if ((kDown & HidNpadButton_X) && connected) {
            Result stopRc = serviceDispatch(&pluginSrv, 2);
            serviceClose(&pluginSrv);
            memset(&pluginSrv, 0, sizeof(pluginSrv));
            connected = false;
            memset(&st, 0, sizeof(st));
            snprintf(statusMsg, sizeof(statusMsg), "Stopped plugin (rc: 0x%x).", stopRc);
        }

        // [Y] Reconnect to background plugin
        if ((kDown & HidNpadButton_Y) && !connected) {
            openRc = open_kaihen_plugin(&pluginSrv, plugin_name);
            if (R_SUCCEEDED(openRc)) {
                connected = true;
                snprintf(statusMsg, sizeof(statusMsg), "Reconnected to %s!", plugin_name);
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "Open failed: 0x%x", openRc);
            }
        }

        // Poll live telemetry (Cmd 0) every 250 ms
        u64 nowTick = armGetSystemTick();
        if (connected && armTicksToNs(nowTick - lastPollTick) >= 250000000ULL) {
            lastPollTick = nowTick;
            Result pollRc = serviceDispatchOut(&pluginSrv, 0, st);
            if (R_FAILED(pollRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Status poll error: 0x%x", pollRc);
            }
        }

        // Render UI
        consoleClear();
        printf("=== Switch 2 caps2:s Capture Config ===\n\n");
        printf("Plugin IPC:  %s (open_rc: 0x%x)\n", connected ? "CONNECTED" : "DISCONNECTED", openRc);

        if (connected) {
            printf("Capture:     %s (caps2:s rc: 0x%x)\n", st.running ? "RUNNING (1080p JPEG)" : "IDLE / ERROR", st.last_rc);
            printf("Live Speed:  %.2f FPS\n", (double)st.avg_fps_x100 / 100.0);
            printf("Frames:      %lu\n", st.frames_captured);
            printf("Frame Size:  %lu bytes (%.1f KB)\n", st.last_frame_size, (double)st.last_frame_size / 1024.0);
        }

        printf("\n---------------------------------------\n");
        printf("Last Action: %s\n", statusMsg);
        printf("---------------------------------------\n\n");
        printf("[A] Save Frame to SD (sdmc:/kaihen_caps2s_frame.jpg)\n");
        printf("[X] Stop Background Capture Plugin\n");
        printf("[Y] Reconnect to Plugin\n");
        printf("[+] Exit Config (keeps background capture running)\n");

        consoleUpdate(NULL);
    }

    if (connected) {
        serviceClose(&pluginSrv);
    }

    consoleExit(NULL);
    return 0;
}

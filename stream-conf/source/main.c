// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "config.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    char ipStr[32];
    config_get_ip_str(ipStr, sizeof(ipStr));

    const char* plugin_name = "stream-module";
    Service pluginSrv = {0};
    bool connected = false;
    Result openRc = config_open_plugin(&pluginSrv, plugin_name);
    if (R_SUCCEEDED(openRc)) {
        connected = true;
    }

    char statusMsg[128] = "Ready!";
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

        // [A] Save latest frame to SD card (Cmd 1)
        if ((kDown & HidNpadButton_A) && connected) {
            Result saveRc = serviceDispatch(&pluginSrv, 1);
            if (R_SUCCEEDED(saveRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Saved sdmc:/kaihen_caps2s_frame.jpg!");
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "Save failed: 0x%x", saveRc);
            }
        }

        // [B] Toggle network streaming on/off (Cmd 3)
        if ((kDown & HidNpadButton_B) && connected) {
            Result togRc = serviceDispatch(&pluginSrv, 3);
            if (R_SUCCEEDED(togRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Toggled network streaming.");
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "Toggle failed: 0x%x", togRc);
            }
        }

        // [R] Toggle audio streaming on/off (Cmd 4)
        if ((kDown & HidNpadButton_R) && connected) {
            Result audRc = serviceDispatch(&pluginSrv, 4);
            if (R_SUCCEEDED(audRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Toggled audio streaming.");
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "Audio toggle failed: 0x%x", audRc);
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
            openRc = config_open_plugin(&pluginSrv, plugin_name);
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
        printf("=== Switch 2 Live MJPEG Streamer ===\n\n");
        printf("Plugin IPC:  %s (open_rc: 0x%x)\n", connected ? "CONNECTED" : "DISCONNECTED", openRc);

        if (connected) {
            printf("Capture:     %s (caps2:s rc: 0x%x)\n", st.running ? "ACTIVE (Auto 1080p/720p JPEG)" : "IDLE / ERROR", st.last_rc);
            printf("Live Speed:  %.2f FPS (Capture) | %.2f FPS (Net TX)\n",
                   (double)st.avg_fps_x100 / 100.0,
                   (double)st.net_fps_x100 / 100.0);
            printf("Frames:      %lu\n", st.frames_captured);
            printf("Frame Size:  %lu bytes (%.1f KB)\n", st.last_frame_size, (double)st.last_frame_size / 1024.0);
            printf("TCP Server:  %s (Port %u)\n", st.net_listening ? "LISTENING" : "NOT BOUND / FAILED", st.stream_port ? st.stream_port : 8080);
            printf("Audio TCP:   %s (Port 8081 | %s | %s | rc: 0x%x)\n",
                   st.audio_listening ? "LISTENING" : "OFF / FAILED",
                   st.audio_enabled ? "ENABLED" : "PAUSED",
                   st.audio_client_connected ? "PC CONNECTED" : "WAITING",
                   st.audio_last_rc);
            printf("\nConnect CMD: python cv2-client.py %s\n", ipStr);
            printf("Net Stream:  %s | Viewer: %s\n",
                   st.streaming_enabled ? "ENABLED" : "PAUSED",
                   st.client_connected ? "PC CONNECTED" : "WAITING FOR PC");
        }

        printf("\n---------------------------------------\n");
        printf("Last Action: %s\n", statusMsg);
        printf("---------------------------------------\n\n");
        printf("[A] Save Frame to SD (sdmc:/kaihen_caps2s_frame.jpg)\n");
        printf("[B] Toggle Network Stream ON/OFF\n");
        printf("[R] Toggle Audio Stream ON/OFF\n");
        printf("[X] Stop Background Plugin\n");
        printf("[Y] Reconnect to Plugin\n");
        printf("[+] Exit Config (keeps background stream running)\n");

        consoleUpdate(NULL);
    }

    if (connected) {
        serviceClose(&pluginSrv);
    }

    consoleExit(NULL);
    return 0;
}

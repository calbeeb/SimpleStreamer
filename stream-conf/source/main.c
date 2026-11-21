// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "config.h"

static const char* usb_speed_name(u8 sp) {
    switch (sp) {
        case 1:  return "Low";
        case 2:  return "Full-12M";
        case 3:  return "High-480M";
        case 4:  return "Super-5G";
        default: return "None";
    }
}

static const char* usb_state_name(u8 st) {
    switch (st) {
        case 0:  return "Detached";
        case 1:  return "Attached";
        case 2:  return "Powered";
        case 3:  return "Default";
        case 4:  return "Address";
        case 5:  return "Configured";
        case 6:  return "Suspended";
        default: return "Unknown";
    }
}

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

        // [B] Toggle streaming on/off (Cmd 3)
        if ((kDown & HidNpadButton_B) && connected) {
            Result togRc = serviceDispatch(&pluginSrv, 3);
            if (R_SUCCEEDED(togRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Toggled streaming.");
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

        // [L] Toggle USB device mode on/off (Cmd 5)
        if ((kDown & HidNpadButton_L) && connected) {
            Result usbRc = serviceDispatch(&pluginSrv, 5);
            if (R_SUCCEEDED(usbRc)) {
                snprintf(statusMsg, sizeof(statusMsg), "Toggled USB streaming.");
            } else {
                snprintf(statusMsg, sizeof(statusMsg), "USB toggle failed: 0x%x", usbRc);
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
            bool running      = (st.flags & STATUS_FLAG_RUNNING) != 0;
            bool stream_en    = (st.flags & STATUS_FLAG_STREAMING_ENABLED) != 0;
            bool tcp_conn     = (st.flags & STATUS_FLAG_CLIENT_CONNECTED) != 0;
            bool net_listen   = (st.flags & STATUS_FLAG_NET_LISTENING) != 0;
            bool aud_listen   = (st.flags & STATUS_FLAG_AUDIO_LISTENING) != 0;
            bool aud_en       = (st.flags & STATUS_FLAG_AUDIO_ENABLED) != 0;
            bool aud_conn     = (st.flags & STATUS_FLAG_AUDIO_CLIENT_CONNECTED) != 0;
            bool usb_en       = (st.flags & STATUS_FLAG_USB_ENABLED) != 0;
            bool usb_active   = (st.flags & STATUS_FLAG_USB_ACTIVE) != 0;
            bool usb_conn     = (st.flags & STATUS_FLAG_USB_CLIENT_CONNECTED) != 0;

            const char* cap_str = running ? "ACTIVE (Auto 1080p/720p JPEG)"
                                          : (st.last_rc == 0 ? "STANDBY (caps2:s closed)" : "ERROR");
            printf("Capture:     %s (caps2:s rc: 0x%x)\n", cap_str, st.last_rc);
            printf("Live Speed:  %.2f FPS (Cap) | %.2f FPS (TCP) | %.2f FPS (USB)\n",
                   (double)st.avg_fps_x100 / 100.0,
                   (double)st.net_fps_x100 / 100.0,
                   (double)st.usb_fps_x100 / 100.0);
            printf("Frames:      %lu | Size: %lu bytes (%.1f KB)\n",
                   st.frames_captured, st.last_frame_size, (double)st.last_frame_size / 1024.0);
            printf("TCP Server:  %s (Port %u | Viewer: %s)\n",
                   net_listen ? "LISTENING" : "OFF",
                   st.stream_port ? st.stream_port : 8080,
                   tcp_conn ? "CONNECTED" : "WAITING");
            printf("Audio:       %s (TCP 8081 %s/%s | rc: 0x%x)\n",
                   aud_en ? "ENABLED" : "PAUSED",
                   aud_listen ? "ON" : "OFF",
                   aud_conn ? "CONNECTED" : "WAITING",
                   st.audio_last_rc);
            printf("USB (Bot-C): %s | %s (%s/%s | Viewer: %s | rc: 0x%x)\n",
                   usb_en ? "ENABLED" : "DISABLED",
                   usb_active ? "ACTIVE" : "OFF",
                   usb_state_name(st.usb_state),
                   usb_speed_name(st.usb_speed),
                   usb_conn ? "CONNECTED" : "WAITING",
                   st.usb_last_rc);
            printf("\nWi-Fi CMD:   python cv2-client.py %s\n", ipStr);
            printf("USB CMD:     python cv2-client.py --usb\n");
            printf("Stream:      %s\n", stream_en ? "ENABLED" : "PAUSED");
        }

        printf("\n---------------------------------------\n");
        printf("Last Action: %s\n", statusMsg);
        printf("---------------------------------------\n\n");
        printf("[A] Save Frame to SD (sdmc:/kaihen_caps2s_frame.jpg)\n");
        printf("[B] Toggle Streaming ON/OFF\n");
        printf("[L] Toggle USB Mode ON/OFF (disable for Album USB copy)\n");
        printf("[R] Toggle Audio ON/OFF\n");
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

// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <switch.h>

#define STATUS_FLAG_RUNNING                (1u << 0)
#define STATUS_FLAG_STREAMING_ENABLED      (1u << 1)
#define STATUS_FLAG_CLIENT_CONNECTED       (1u << 2)
#define STATUS_FLAG_NET_LISTENING          (1u << 3)
#define STATUS_FLAG_AUDIO_LISTENING        (1u << 4)
#define STATUS_FLAG_AUDIO_ENABLED          (1u << 5)
#define STATUS_FLAG_AUDIO_CLIENT_CONNECTED (1u << 6)
#define STATUS_FLAG_USB_ENABLED            (1u << 7)
#define STATUS_FLAG_USB_ACTIVE             (1u << 8)
#define STATUS_FLAG_USB_CLIENT_CONNECTED   (1u << 9)

typedef struct {
    u32 flags;
    u32 avg_fps_x100;
    Result last_rc;
    u32 net_fps_x100;
    u64 frames_captured;
    u64 last_frame_size;
    u32 usb_fps_x100;
    Result audio_last_rc;
    Result usb_last_rc;
    u16 stream_port;
    u8  usb_state;
    u8  usb_speed;
    u32 reserved;
} Caps2sStatus;

Result config_open_plugin(Service* out_plugin, const char* plugin_name);
void config_get_ip_str(char* out_str, size_t max_len);

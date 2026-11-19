// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <switch.h>

typedef struct {
    u32 running;
    u32 avg_fps_x100;
    Result last_rc;
    u32 streaming_enabled;
    u64 frames_captured;
    u64 last_frame_size;
    u32 client_connected;
    u32 stream_port;
    u32 net_listening;
    u32 reserved;
} Caps2sStatus;

Result config_open_plugin(Service* out_plugin, const char* plugin_name);
void config_get_ip_str(char* out_str, size_t max_len);

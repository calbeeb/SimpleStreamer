// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <switch.h>
#include <kaihen/plugin.h>

#define CAP_BUF_SIZE           0x400000 // 4 MB buffer
#define CAPS2S_MODE_1080P_JPEG 0x00020200
#define CAPS2S_MODE_720P_JPEG  0x00020100
#define CAPS2S_MODE_1080P_RAW  0x00010200
#define CAPS2S_MODE_720P_RAW   0x00010100
#define STREAM_PORT            8080

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
    u32 net_fps_x100;
} Caps2sStatus;

_Static_assert(sizeof(Caps2sStatus) <= KAIHEN_PLUGIN_MESSAGE_SIZE, "Status must fit in 64-byte scalar reply");

typedef struct {
    Handle exit_write;
    Mutex buf_mutex;
    CondVar frame_cv;
    u8* raw_bufs[3];
    u8* bufs[3];
    int cap_idx;
    int ready_idx;
    int net_idx;
    u64 ready_size;
    u64 frame_seq;
    int listen_fd;
    atomic_bool stop_requested;
    atomic_bool running;
    atomic_bool streaming_enabled;
    atomic_bool client_connected;
    atomic_bool net_listening;
    atomic_uint_fast32_t last_rc;
    atomic_uint_fast32_t avg_fps_x100;
    atomic_uint_fast32_t net_fps_x100;
    atomic_uint_fast32_t last_cap_us;
    atomic_uint_fast32_t last_net_us;
    atomic_uint_fast64_t frames_captured;
    atomic_uint_fast64_t last_frame_size;
} CaptureContext;

void stream_write_log(const char* msg);
void capture_thread_func(void* arg);
Result capture_save_latest_frame(CaptureContext* ctx, const char* path);
void network_thread_func(void* arg);

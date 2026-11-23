// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <switch.h>
#include <kaihen/plugin.h>

#define CAP_BUF_COUNT          4
#define CAP_BUF_SIZE           0x400000 // 4 MB buffer
#define CAPS2S_MODE_1080P_JPEG 0x00020200
#define CAPS2S_MODE_720P_JPEG  0x00020100
#define CAPS2S_MODE_1080P_RAW  0x00010200
#define CAPS2S_MODE_720P_RAW   0x00010100
#define STREAM_PORT            8080
#define AUDIO_STREAM_PORT      8081

#define STREAM_LOG_DIR         "sdmc:/switch/SimpleStreamer/logs"
#define STREAM_LOG_PATH        STREAM_LOG_DIR "/streamer.log"
#define STREAM_CFG_PATH        "sdmc:/switch/SimpleStreamer/streamer.cfg"

#define STREAM_MAGIC           0x5357324Au // "SW2J"
#define AUDIO_STREAM_MAGIC     0x53573241u // "SW2A"

#define AUDIO_CHANNELS         2
#define AUDIO_SAMPLE_RATE      48000
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES      (AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_MAX_PKT_FRAMES   1920       // 40 ms max per packet (7680 bytes)

// Status flags packed into Caps2sStatus.flags (keeps struct <= 64 bytes)
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

_Static_assert(sizeof(Caps2sStatus) <= KAIHEN_PLUGIN_MESSAGE_SIZE, "Status must fit in 64-byte scalar reply");

typedef struct {
    Service dmg_sess;
    Service smx_mgr;
    Service smx_sess;
    Service smx_stream;
    Event   ev;
    bool    has_ev;
    bool    started;
    u32     stream_id;
    u32     ring_frames;
    u32     ring_slots;
    u8*     ring_base;
    size_t  ring_size;
} AudioCaptureSession;

typedef struct {
    bool streaming_enabled;
    bool audio_enabled;
    bool usb_enabled;
    u16  video_port;
    u16  audio_port;
} StreamConfig;

typedef struct {
    Handle exit_write;
    Mutex buf_mutex;
    CondVar frame_cv;
    u8* raw_bufs[CAP_BUF_COUNT];
    u8* bufs[CAP_BUF_COUNT];
    int cap_idx;
    int ready_idx;
    int net_idx;
    int usb_idx;
    u64 ready_size;
    u64 frame_seq;
    u64 ready_seq;
    u64 net_last_seq;
    u64 usb_last_seq;
    int listen_fd;
    int audio_listen_fd;
    u16 video_port;
    u16 audio_port;
    atomic_bool stop_requested;
    atomic_bool running;
    atomic_bool streaming_enabled;
    atomic_bool client_connected;
    atomic_bool net_listening;
    atomic_bool audio_enabled;
    atomic_bool audio_listening;
    atomic_bool audio_client_connected;
    atomic_bool usb_enabled;
    atomic_bool usb_active;
    atomic_bool usb_client_connected;
    atomic_bool nifm_inited;
    atomic_uint_fast32_t last_rc;
    atomic_uint_fast32_t audio_last_rc;
    atomic_uint_fast32_t usb_last_rc;
    atomic_uint_fast32_t usb_state;
    atomic_uint_fast32_t usb_speed;
    atomic_uint_fast32_t avg_fps_x100;
    atomic_uint_fast32_t net_fps_x100;
    atomic_uint_fast32_t usb_fps_x100;
    atomic_uint_fast32_t last_cap_us;
    atomic_uint_fast32_t last_net_us;
    atomic_uint_fast64_t frames_captured;
    atomic_uint_fast64_t last_frame_size;
} CaptureContext;

void stream_ensure_log_dir(void);
void stream_write_log(const char* msg);
void stream_config_defaults(StreamConfig* cfg);
void stream_config_load(StreamConfig* cfg, bool sd_mounted);
void capture_thread_func(void* arg);
Result capture_save_latest_frame(CaptureContext* ctx, const char* path);
void network_thread_func(void* arg);
void audio_thread_func(void* arg);
void usb_thread_func(void* arg);

Result audio_capture_open(AudioCaptureSession* s);
void audio_capture_close(AudioCaptureSession* s);
size_t audio_capture_read_pcm(AudioCaptureSession* s, u8* out_pcm, u32 max_frames, bool* out_error);

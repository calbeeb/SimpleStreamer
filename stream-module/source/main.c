// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stream.h"

u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x2000000;

void __appInit(void) {}
void __appExit(void) {}

static Result pluginCommand(u32 command_id, const void* input,
                            size_t input_size, void* output,
                            size_t* output_size, void* context) {
    (void)input;
    (void)input_size;
    CaptureContext* ctx = (CaptureContext*)context;

    if (command_id == 0) {
        u32 flags = 0;
        if (atomic_load(&ctx->running))                flags |= STATUS_FLAG_RUNNING;
        if (atomic_load(&ctx->streaming_enabled))      flags |= STATUS_FLAG_STREAMING_ENABLED;
        if (atomic_load(&ctx->client_connected))       flags |= STATUS_FLAG_CLIENT_CONNECTED;
        if (atomic_load(&ctx->net_listening))          flags |= STATUS_FLAG_NET_LISTENING;
        if (atomic_load(&ctx->audio_listening))        flags |= STATUS_FLAG_AUDIO_LISTENING;
        if (atomic_load(&ctx->audio_enabled))          flags |= STATUS_FLAG_AUDIO_ENABLED;
        if (atomic_load(&ctx->audio_client_connected)) flags |= STATUS_FLAG_AUDIO_CLIENT_CONNECTED;
        if (atomic_load(&ctx->usb_enabled))            flags |= STATUS_FLAG_USB_ENABLED;
        if (atomic_load(&ctx->usb_active))             flags |= STATUS_FLAG_USB_ACTIVE;
        if (atomic_load(&ctx->usb_client_connected))   flags |= STATUS_FLAG_USB_CLIENT_CONNECTED;

        Caps2sStatus st = {
            .flags           = flags,
            .avg_fps_x100    = atomic_load(&ctx->avg_fps_x100),
            .last_rc         = atomic_load(&ctx->last_rc),
            .net_fps_x100    = atomic_load(&ctx->net_fps_x100),
            .frames_captured = atomic_load(&ctx->frames_captured),
            .last_frame_size = atomic_load(&ctx->last_frame_size),
            .usb_fps_x100    = atomic_load(&ctx->usb_fps_x100),
            .audio_last_rc   = atomic_load(&ctx->audio_last_rc),
            .usb_last_rc     = atomic_load(&ctx->usb_last_rc),
            .stream_port     = ctx->video_port,
            .usb_state       = (u8)atomic_load(&ctx->usb_state),
            .usb_speed       = (u8)atomic_load(&ctx->usb_speed),
            .reserved        = 0,
        };
        memcpy(output, &st, sizeof(st));
        *output_size = sizeof(st);
        return 0;
    }

    if (command_id == 1) {
        *output_size = 0;
        return capture_save_latest_frame(ctx, "sdmc:/switch/SimpleStreamer/logs/test_capture.jpg");
    }

    if (command_id == 2) {
        *output_size = 0;
        atomic_store(&ctx->stop_requested, true);
        if (ctx->listen_fd >= 0) {
            close(ctx->listen_fd);
            ctx->listen_fd = -1;
        }
        if (ctx->audio_listen_fd >= 0) {
            close(ctx->audio_listen_fd);
            ctx->audio_listen_fd = -1;
        }
        return svcSignalEvent(ctx->exit_write);
    }

    if (command_id == 3) {
        bool cur = atomic_load(&ctx->streaming_enabled);
        atomic_store(&ctx->streaming_enabled, !cur);
        mutexLock(&ctx->buf_mutex);
        condvarWakeAll(&ctx->frame_cv);
        mutexUnlock(&ctx->buf_mutex);
        *output_size = 0;
        return 0;
    }

    if (command_id == 4) {
        bool cur = atomic_load(&ctx->audio_enabled);
        atomic_store(&ctx->audio_enabled, !cur);
        *output_size = 0;
        return 0;
    }

    if (command_id == 5) {
        bool cur = atomic_load(&ctx->usb_enabled);
        atomic_store(&ctx->usb_enabled, !cur);
        mutexLock(&ctx->buf_mutex);
        condvarWakeAll(&ctx->frame_cv);
        mutexUnlock(&ctx->buf_mutex);
        *output_size = 0;
        return 0;
    }

    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

static bool create_tuned_thread(Thread* t, ThreadFunc func, void* arg,
                                const int* prios, size_t n_prios, s32 core_id, const char* name) {
    for (size_t i = 0; i < n_prios; i++) {
        Result rc = threadCreate(t, func, arg, NULL, 0x10000, prios[i], core_id);
        if (R_FAILED(rc) && core_id != -2) {
            rc = threadCreate(t, func, arg, NULL, 0x10000, prios[i], -2);
        }
        if (R_SUCCEEDED(rc)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Created %s (prio=0x%x, core=%d)", name, prios[i], core_id);
            stream_write_log(msg);
            return true;
        }
    }
    return false;
}

int main(void) {
    Handle ready = INVALID_HANDLE;
    Handle control_session = INVALID_HANDLE;
    Result rc = pluginRuntimeInitializeWithService(&ready, &control_session);
    if (R_FAILED(rc)) return rc;

    bool fs_inited = R_SUCCEEDED(fsInitialize());
    bool sd_mounted = fs_inited && R_SUCCEEDED(fsdevMountSdmc());

    static const SocketInitConfig sock_cfg = {
        .tcp_tx_buf_size     = 0x80000,  // 512 KB TX socket buffer for 2+ MB frames
        .tcp_rx_buf_size     = 0x10000,  // 64 KB RX socket buffer
        .tcp_tx_buf_max_size = 0x200000, // 2 MB max TX TransferMemory pool (4x headroom)
        .tcp_rx_buf_max_size = 0x40000,  // 256 KB max RX TransferMemory pool
        .udp_tx_buf_size     = 0x2400,
        .udp_rx_buf_size     = 0xA500,
        .sb_efficiency       = 4,
        .num_bsd_sessions    = 6,
        .bsd_service_type    = BsdServiceType_User,
    };
    Result sock_rc = socketInitialize(&sock_cfg);
    if (R_FAILED(sock_rc)) {
        sock_rc = socketInitializeDefault();
    }
    bool sock_inited = R_SUCCEEDED(sock_rc);
    bool nifm_inited = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));

    u64 core_mask = 0;
    svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);

    if (sd_mounted) {
        stream_ensure_log_dir();
        FILE* f = fopen(STREAM_LOG_PATH, "w");
        if (f) {
            fprintf(f, "stream-module started (fs=%d, sd=%d, sock_rc=0x%x, nifm=%d, core_mask=0x%lx)\n",
                    fs_inited, sd_mounted, sock_rc, nifm_inited, (unsigned long)core_mask);
            fclose(f);
        }
    }

    StreamConfig cfg;
    stream_config_load(&cfg, sd_mounted);

    CaptureContext ctx = {0};
    ctx.listen_fd = -1;
    ctx.audio_listen_fd = -1;
    ctx.video_port = cfg.video_port;
    ctx.audio_port = cfg.audio_port;
    ctx.cap_idx   = 0;
    ctx.ready_idx = 1;
    ctx.net_idx   = 2;
    ctx.usb_idx   = 3;
    mutexInit(&ctx.buf_mutex);
    condvarInit(&ctx.frame_cv);
    atomic_init(&ctx.stop_requested, false);
    atomic_init(&ctx.running, false);
    atomic_init(&ctx.streaming_enabled, cfg.streaming_enabled);
    atomic_init(&ctx.client_connected, false);
    atomic_init(&ctx.net_listening, false);
    atomic_init(&ctx.audio_enabled, cfg.audio_enabled);
    atomic_init(&ctx.audio_listening, false);
    atomic_init(&ctx.audio_client_connected, false);
    atomic_init(&ctx.usb_enabled, cfg.usb_enabled);
    atomic_init(&ctx.usb_active, false);
    atomic_init(&ctx.usb_client_connected, false);
    atomic_init(&ctx.nifm_inited, nifm_inited);
    atomic_init(&ctx.last_rc, 0);
    atomic_init(&ctx.audio_last_rc, 0);
    atomic_init(&ctx.usb_last_rc, 0);
    atomic_init(&ctx.usb_state, 0);
    atomic_init(&ctx.usb_speed, 0);
    atomic_init(&ctx.avg_fps_x100, 0);
    atomic_init(&ctx.net_fps_x100, 0);
    atomic_init(&ctx.usb_fps_x100, 0);
    atomic_init(&ctx.last_cap_us, 0);
    atomic_init(&ctx.last_net_us, 0);
    atomic_init(&ctx.frames_captured, 0);
    atomic_init(&ctx.last_frame_size, 0);

    // Zero-copy quad-buffering with a 4 KB prefix page per buffer:
    // - bufs[i] = raw_bufs[i] + 0x1000 (4 KB page-aligned 4 MB region for caps2:s & usb:ds DMA)
    // - net_thread writes TcpFrameHeader to bufs[net_idx] - 8 for single-pass TCP send()
    bool bufs_ok = true;
    for (int i = 0; i < CAP_BUF_COUNT; i++) {
        ctx.raw_bufs[i] = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE + 0x1000);
        ctx.bufs[i]     = ctx.raw_bufs[i] ? (ctx.raw_bufs[i] + 0x1000) : NULL;
        if (!ctx.bufs[i]) bufs_ok = false;
    }

    Handle exit_write = INVALID_HANDLE;
    Handle exit_read = INVALID_HANDLE;
    PluginService service = {0};

    if (!bufs_ok) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else {
        rc = svcCreateEvent(&exit_write, &exit_read);
    }

    if (R_SUCCEEDED(rc)) {
        ctx.exit_write = exit_write;
        rc = pluginServiceStart(&service, control_session, pluginCommand, &ctx);
    }

    Thread cap_thread = {0};
    Thread net_thread = {0};
    Thread audio_thread = {0};
    Thread usb_thread = {0};
    bool cap_ok = false;
    bool net_ok = false;
    bool audio_ok = false;
    bool usb_ok = false;

    s32 cap_core = -2;
    s32 net_core = -2;
    if (__builtin_popcountll(core_mask) >= 2) {
        cap_core = __builtin_ctzll(core_mask);
        net_core = 63 - __builtin_clzll(core_mask);
    }

    static const int cap_prios[] = { 0x24, 0x26, 0x28, 0x2B, 0x2C };
    static const int net_prios[] = { 0x28, 0x2A, 0x2B, 0x2C };

    if (R_SUCCEEDED(rc) && create_tuned_thread(&cap_thread, capture_thread_func, &ctx,
            cap_prios, sizeof(cap_prios)/sizeof(cap_prios[0]), cap_core, "cap_thread")) {
        cap_ok = R_SUCCEEDED(threadStart(&cap_thread));
    }

    if (R_SUCCEEDED(rc) && sock_inited && create_tuned_thread(&net_thread, network_thread_func, &ctx,
            net_prios, sizeof(net_prios)/sizeof(net_prios[0]), net_core, "net_thread")) {
        net_ok = R_SUCCEEDED(threadStart(&net_thread));
    }

    if (R_SUCCEEDED(rc) && sock_inited && create_tuned_thread(&audio_thread, audio_thread_func, &ctx,
            net_prios, sizeof(net_prios)/sizeof(net_prios[0]), net_core, "audio_thread")) {
        audio_ok = R_SUCCEEDED(threadStart(&audio_thread));
    }

    if (R_SUCCEEDED(rc) && create_tuned_thread(&usb_thread, usb_thread_func, &ctx,
            net_prios, sizeof(net_prios)/sizeof(net_prios[0]), net_core, "usb_thread")) {
        usb_ok = R_SUCCEEDED(threadStart(&usb_thread));
    }

    if (R_SUCCEEDED(rc)) {
        rc = svcSignalEvent(ready);
    }

    if (R_SUCCEEDED(rc)) {
        rc = svcWaitSynchronizationSingle(exit_read, UINT64_MAX);
    }

    atomic_store(&ctx.stop_requested, true);
    if (ctx.listen_fd >= 0) {
        close(ctx.listen_fd);
        ctx.listen_fd = -1;
    }
    if (ctx.audio_listen_fd >= 0) {
        close(ctx.audio_listen_fd);
        ctx.audio_listen_fd = -1;
    }

    if (cap_ok)   { threadWaitForExit(&cap_thread);   threadClose(&cap_thread); }
    if (net_ok)   { threadWaitForExit(&net_thread);   threadClose(&net_thread); }
    if (audio_ok) { threadWaitForExit(&audio_thread); threadClose(&audio_thread); }
    if (usb_ok)   { threadWaitForExit(&usb_thread);   threadClose(&usb_thread); }
    if (service.started) pluginServiceStop(&service);

    if (exit_read != INVALID_HANDLE) svcCloseHandle(exit_read);
    if (exit_write != INVALID_HANDLE) svcCloseHandle(exit_write);

    for (int i = 0; i < CAP_BUF_COUNT; i++) {
        free(ctx.raw_bufs[i]);
    }

    if (nifm_inited) nifmExit();
    if (sock_inited) socketExit();
    if (sd_mounted) fsdevUnmountAll();
    if (fs_inited) fsExit();

    pluginRuntimeExit();
    return rc;
}

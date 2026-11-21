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
        Caps2sStatus st = {
            .running                = atomic_load(&ctx->running) ? 1 : 0,
            .avg_fps_x100           = atomic_load(&ctx->avg_fps_x100),
            .last_rc                = atomic_load(&ctx->last_rc),
            .streaming_enabled      = atomic_load(&ctx->streaming_enabled) ? 1 : 0,
            .frames_captured        = atomic_load(&ctx->frames_captured),
            .last_frame_size        = atomic_load(&ctx->last_frame_size),
            .client_connected       = atomic_load(&ctx->client_connected) ? 1 : 0,
            .stream_port            = STREAM_PORT,
            .net_listening          = atomic_load(&ctx->net_listening) ? 1 : 0,
            .net_fps_x100           = atomic_load(&ctx->net_fps_x100),
            .audio_listening        = atomic_load(&ctx->audio_listening) ? 1 : 0,
            .audio_enabled          = atomic_load(&ctx->audio_enabled) ? 1 : 0,
            .audio_client_connected = atomic_load(&ctx->audio_client_connected) ? 1 : 0,
            .audio_last_rc          = atomic_load(&ctx->audio_last_rc),
        };
        memcpy(output, &st, sizeof(st));
        *output_size = sizeof(st);
        return 0;
    }

    if (command_id == 1) {
        *output_size = 0;
        return capture_save_latest_frame(ctx, "sdmc:/kaihen_caps2s_frame.jpg");
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

    u64 core_mask = 0;
    svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);

    if (sd_mounted) {
        FILE* f = fopen("sdmc:/kaihen_caps2s.log", "w");
        if (f) {
            fprintf(f, "stream-module started (fs=%d, sd=%d, sock_rc=0x%x, core_mask=0x%lx)\n",
                    fs_inited, sd_mounted, sock_rc, (unsigned long)core_mask);
            fclose(f);
        }
    }

    CaptureContext ctx = {0};
    ctx.listen_fd = -1;
    ctx.audio_listen_fd = -1;
    ctx.cap_idx   = 0;
    ctx.ready_idx = 1;
    ctx.net_idx   = 2;
    mutexInit(&ctx.buf_mutex);
    condvarInit(&ctx.frame_cv);
    atomic_init(&ctx.stop_requested, false);
    atomic_init(&ctx.running, false);
    atomic_init(&ctx.streaming_enabled, true);
    atomic_init(&ctx.client_connected, false);
    atomic_init(&ctx.net_listening, false);
    atomic_init(&ctx.audio_enabled, true);
    atomic_init(&ctx.audio_listening, false);
    atomic_init(&ctx.audio_client_connected, false);
    atomic_init(&ctx.last_rc, 0);
    atomic_init(&ctx.audio_last_rc, 0);
    atomic_init(&ctx.avg_fps_x100, 0);
    atomic_init(&ctx.net_fps_x100, 0);
    atomic_init(&ctx.last_cap_us, 0);
    atomic_init(&ctx.last_net_us, 0);
    atomic_init(&ctx.frames_captured, 0);
    atomic_init(&ctx.last_frame_size, 0);

    // Zero-copy atomic triple-buffering with a 4 KB prefix page per buffer:
    // - bufs[i] = raw_bufs[i] + 0x1000 (page-aligned 4 MB region passed to caps2:s)
    // - net_thread writes TcpFrameHeader to bufs[net_idx] - 8 for single-pass send()
    for (int i = 0; i < 3; i++) {
        ctx.raw_bufs[i] = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE + 0x1000);
        ctx.bufs[i]     = ctx.raw_bufs[i] ? (ctx.raw_bufs[i] + 0x1000) : NULL;
    }

    Handle exit_write = INVALID_HANDLE;
    Handle exit_read = INVALID_HANDLE;
    PluginService service = {0};

    if (!ctx.bufs[0] || !ctx.bufs[1] || !ctx.bufs[2]) {
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
    bool cap_ok = false;
    bool net_ok = false;
    bool audio_ok = false;

    // Pick lowest/highest allowed cores if multiple cores exist so net_thread never contends with capsrv2
    s32 cap_core = -2;
    s32 net_core = -2;
    if (__builtin_popcountll(core_mask) >= 2) {
        cap_core = __builtin_ctzll(core_mask);
        net_core = 63 - __builtin_clzll(core_mask);
    }

    // Keep cap_thread (0x24) and net_thread/audio_thread (0x28) strictly below capsrv2's 0x22 worker threads
    // so network I/O never preempts JPEG encoding inside capsrv2, yet stay above game threads (0x2C).
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

    if (cap_ok) { threadWaitForExit(&cap_thread); threadClose(&cap_thread); }
    if (net_ok) { threadWaitForExit(&net_thread); threadClose(&net_thread); }
    if (audio_ok) { threadWaitForExit(&audio_thread); threadClose(&audio_thread); }
    if (service.started) pluginServiceStop(&service);

    if (exit_read != INVALID_HANDLE) svcCloseHandle(exit_read);
    if (exit_write != INVALID_HANDLE) svcCloseHandle(exit_write);

    free(ctx.raw_bufs[0]);
    free(ctx.raw_bufs[1]);
    free(ctx.raw_bufs[2]);

    if (sock_inited) socketExit();
    if (sd_mounted) fsdevUnmountAll();
    if (fs_inited) fsExit();

    pluginRuntimeExit();
    return rc;
}

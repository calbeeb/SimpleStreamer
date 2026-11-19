// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stream.h"

u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x1400000; // 20 MB heap

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
            .running           = atomic_load(&ctx->running) ? 1 : 0,
            .avg_fps_x100      = atomic_load(&ctx->avg_fps_x100),
            .last_rc           = atomic_load(&ctx->last_rc),
            .streaming_enabled = atomic_load(&ctx->streaming_enabled) ? 1 : 0,
            .frames_captured   = atomic_load(&ctx->frames_captured),
            .last_frame_size   = atomic_load(&ctx->last_frame_size),
            .client_connected  = atomic_load(&ctx->client_connected) ? 1 : 0,
            .stream_port       = STREAM_PORT,
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
        return svcSignalEvent(ctx->exit_write);
    }

    if (command_id == 3) {
        bool cur = atomic_load(&ctx->streaming_enabled);
        atomic_store(&ctx->streaming_enabled, !cur);
        mutexLock(&ctx->buf_mutex);
        condvarBroadcast(&ctx->frame_cv);
        mutexUnlock(&ctx->buf_mutex);
        *output_size = 0;
        return 0;
    }

    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

int main(void) {
    Handle ready = INVALID_HANDLE;
    Handle control_session = INVALID_HANDLE;
    Result rc = pluginRuntimeInitializeWithService(&ready, &control_session);
    if (R_FAILED(rc)) return rc;

    bool fs_inited = R_SUCCEEDED(fsInitialize());
    bool sd_mounted = fs_inited && R_SUCCEEDED(fsdevMountSdmc());
    bool sock_inited = R_SUCCEEDED(socketInitializeDefault());

    if (sd_mounted) {
        FILE* f = fopen("sdmc:/kaihen_caps2s.log", "w");
        if (f) {
            fprintf(f, "stream-module initialized (fs=%d, sd=%d, sock=%d)\n", fs_inited, sd_mounted, sock_inited);
            fclose(f);
        }
    }

    CaptureContext ctx = {0};
    ctx.listen_fd = -1;
    mutexInit(&ctx.buf_mutex);
    condvarInit(&ctx.frame_cv);
    atomic_init(&ctx.stop_requested, false);
    atomic_init(&ctx.running, false);
    atomic_init(&ctx.streaming_enabled, true);
    atomic_init(&ctx.client_connected, false);
    atomic_init(&ctx.last_rc, 0);
    atomic_init(&ctx.avg_fps_x100, 0);
    atomic_init(&ctx.frames_captured, 0);
    atomic_init(&ctx.last_frame_size, 0);

    ctx.work_buf = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE);
    ctx.snap_buf = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE);
    ctx.net_buf  = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE);

    Handle exit_write = INVALID_HANDLE;
    Handle exit_read = INVALID_HANDLE;
    PluginService service = {0};
    Thread cap_thread = {0};
    Thread net_thread = {0};
    bool cap_ok = false;
    bool net_ok = false;

    if (!ctx.work_buf || !ctx.snap_buf || !ctx.net_buf) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else {
        rc = svcCreateEvent(&exit_write, &exit_read);
    }

    if (R_SUCCEEDED(rc)) {
        ctx.exit_write = exit_write;
        rc = pluginServiceStart(&service, control_session, pluginCommand, &ctx);
    }

    if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadCreate(&cap_thread, capture_thread_func, &ctx, NULL, 0x10000, 0x2C, -2))) {
        cap_ok = R_SUCCEEDED(threadStart(&cap_thread));
    }

    if (R_SUCCEEDED(rc) && sock_inited && R_SUCCEEDED(threadCreate(&net_thread, network_thread_func, &ctx, NULL, 0x10000, 0x2D, -2))) {
        net_ok = R_SUCCEEDED(threadStart(&net_thread));
    }

    if (R_SUCCEEDED(rc)) rc = svcSignalEvent(ready);
    if (R_SUCCEEDED(rc)) rc = svcWaitSynchronizationSingle(exit_read, UINT64_MAX);

    atomic_store(&ctx.stop_requested, true);
    if (ctx.listen_fd >= 0) {
        close(ctx.listen_fd);
        ctx.listen_fd = -1;
    }

    if (cap_ok) { threadWaitForExit(&cap_thread); threadClose(&cap_thread); }
    if (net_ok) { threadWaitForExit(&net_thread); threadClose(&net_thread); }
    if (service.started) pluginServiceStop(&service);

    if (exit_read != INVALID_HANDLE) svcCloseHandle(exit_read);
    if (exit_write != INVALID_HANDLE) svcCloseHandle(exit_write);
    free(ctx.work_buf);
    free(ctx.snap_buf);
    free(ctx.net_buf);

    if (sock_inited) socketExit();
    if (sd_mounted) fsdevUnmountAll();
    if (fs_inited) fsExit();

    pluginRuntimeExit();
    return rc;
}

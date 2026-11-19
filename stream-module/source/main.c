// SPDX-License-Identifier: MIT
// Background Switch 2 screen capture plugin using caps2:s Cmd 101 (1080p JPEG @ 30 FPS).
// Place plugin-caps2s.nso in sdmc:/kaihen/plugins/plugin-caps2s.nso.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <switch.h>
#include <kaihen/plugin.h>

u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0x1000000; // 16 MB heap for 4 MB aligned capture + snapshot buffers

void __appInit(void) {}
void __appExit(void) {}

#define CAP_BUF_SIZE 0x400000 // 4 MB buffer
#define CAPS2S_MODE_1080P_JPEG 0x00020200

typedef struct {
    u32 running;
    u32 avg_fps_x100;
    Result last_rc;
    u32 pad;
    u64 frames_captured;
    u64 last_frame_size;
} Caps2sStatus;

_Static_assert(sizeof(Caps2sStatus) <= KAIHEN_PLUGIN_MESSAGE_SIZE, "Status must fit in 64-byte scalar reply");

typedef struct {
    Handle exit_write;
    Mutex buf_mutex;
    u8* work_buf;
    u8* snap_buf;
    u64 snap_size;
    atomic_bool stop_requested;
    atomic_bool running;
    atomic_uint_fast32_t last_rc;
    atomic_uint_fast32_t avg_fps_x100;
    atomic_uint_fast64_t frames_captured;
    atomic_uint_fast64_t last_frame_size;
} CaptureContext;

static void write_log(const char* msg) {
    FILE* f = fopen("sdmc:/kaihen_caps2s.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void capture_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;

    // smGetService works directly using the bootstrap's installed SM override
    Service caps2sSrv;
    Result rc = smGetService(&caps2sSrv, "caps2:s");
    atomic_store(&ctx->last_rc, rc);

    char log_line[128];
    snprintf(log_line, sizeof(log_line), "smGetService(caps2:s) -> 0x%x", rc);
    write_log(log_line);

    if (R_FAILED(rc)) {
        return;
    }

    atomic_store(&ctx->running, true);

    u32 mode = CAPS2S_MODE_1080P_JPEG;
    u64 window_frames = 0;
    u64 window_start_tick = armGetSystemTick();

    while (!atomic_load(&ctx->stop_requested)) {
        u64 out_size = 0;
        rc = serviceDispatchInOut(&caps2sSrv, 101, mode, out_size,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out | SfBufferAttr_HipcMapTransferAllowsNonSecure },
            .buffers = { { ctx->work_buf, CAP_BUF_SIZE } },
        );

        atomic_store(&ctx->last_rc, rc);

        if (R_SUCCEEDED(rc) && out_size > 0 && out_size <= CAP_BUF_SIZE) {
            mutexLock(&ctx->buf_mutex);
            memcpy(ctx->snap_buf, ctx->work_buf, out_size);
            ctx->snap_size = out_size;
            mutexUnlock(&ctx->buf_mutex);

            atomic_fetch_add(&ctx->frames_captured, 1);
            atomic_store(&ctx->last_frame_size, out_size);

            window_frames++;
            u64 now_tick = armGetSystemTick();
            u64 elapsed_ns = armTicksToNs(now_tick - window_start_tick);
            if (elapsed_ns >= 1000000000ULL) {
                double fps = ((double)window_frames * 1000000000.0) / (double)elapsed_ns;
                atomic_store(&ctx->avg_fps_x100, (u32)(fps * 100.0));
                window_frames = 0;
                window_start_tick = now_tick;
            }
        } else {
            svcSleepThread(16000000ULL);
        }
    }

    atomic_store(&ctx->running, false);
    serviceClose(&caps2sSrv);
    write_log("Capture thread stopped cleanly.");
}

static Result pluginCommand(u32 command_id, const void* input,
                            size_t input_size, void* output,
                            size_t* output_size, void* context) {
    (void)input;
    (void)input_size;
    CaptureContext* ctx = (CaptureContext*)context;

    // Cmd 0: Query live capture status
    if (command_id == 0) {
        Caps2sStatus st = {
            .running         = atomic_load(&ctx->running) ? 1 : 0,
            .avg_fps_x100    = atomic_load(&ctx->avg_fps_x100),
            .last_rc         = atomic_load(&ctx->last_rc),
            .pad             = 0,
            .frames_captured = atomic_load(&ctx->frames_captured),
            .last_frame_size = atomic_load(&ctx->last_frame_size),
        };
        memcpy(output, &st, sizeof(st));
        *output_size = sizeof(st);
        return 0;
    }

    // Cmd 1: Save latest captured JPEG frame to sdmc:/kaihen_caps2s_frame.jpg
    if (command_id == 1) {
        Result save_rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        mutexLock(&ctx->buf_mutex);
        if (ctx->snap_size > 0 && ctx->snap_size <= CAP_BUF_SIZE) {
            FILE* f = fopen("sdmc:/kaihen_caps2s_frame.jpg", "wb");
            if (f) {
                size_t written = fwrite(ctx->snap_buf, 1, ctx->snap_size, f);
                fclose(f);
                if (written == ctx->snap_size) {
                    save_rc = 0;
                }
            }
        }
        mutexUnlock(&ctx->buf_mutex);
        *output_size = 0;
        return save_rc;
    }

    // Cmd 2: Stop background capture and exit plugin
    if (command_id == 2) {
        *output_size = 0;
        atomic_store(&ctx->stop_requested, true);
        return svcSignalEvent(ctx->exit_write);
    }

    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

int main(void) {
    Handle ready = INVALID_HANDLE;
    Handle control_session = INVALID_HANDLE;
    Result rc = pluginRuntimeInitializeWithService(&ready, &control_session);
    if (R_FAILED(rc)) return rc;

    // Per kaihen/plugin.h: SM is already overridden; plugins needing FS call fsInitialize() themselves
    bool fs_inited = R_SUCCEEDED(fsInitialize());
    bool sd_mounted = fs_inited && R_SUCCEEDED(fsdevMountSdmc());

    if (sd_mounted) {
        FILE* f = fopen("sdmc:/kaihen_caps2s.log", "w");
        if (f) {
            fprintf(f, "plugin-caps2s initialized (fs=%d, sd=%d)\n", fs_inited, sd_mounted);
            fclose(f);
        }
    }

    CaptureContext ctx = {0};
    mutexInit(&ctx.buf_mutex);
    atomic_init(&ctx.stop_requested, false);
    atomic_init(&ctx.running, false);
    atomic_init(&ctx.last_rc, 0);
    atomic_init(&ctx.avg_fps_x100, 0);
    atomic_init(&ctx.frames_captured, 0);
    atomic_init(&ctx.last_frame_size, 0);

    ctx.work_buf = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE);
    ctx.snap_buf = (u8*)aligned_alloc(0x1000, CAP_BUF_SIZE);

    Handle exit_write = INVALID_HANDLE;
    Handle exit_read = INVALID_HANDLE;
    PluginService service = {0};
    Thread cap_thread = {0};
    bool cap_thread_created = false;

    if (!ctx.work_buf || !ctx.snap_buf) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else {
        rc = svcCreateEvent(&exit_write, &exit_read);
    }

    if (R_SUCCEEDED(rc)) {
        ctx.exit_write = exit_write;
        rc = pluginServiceStart(&service, control_session, pluginCommand, &ctx);
    }

    if (R_SUCCEEDED(rc)) {
        rc = threadCreate(&cap_thread, capture_thread_func, &ctx, NULL, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            cap_thread_created = true;
            rc = threadStart(&cap_thread);
        }
    }

    // Signal ready within the 5-second bootstrap window so HEN publishes "plugin-caps2s"
    if (R_SUCCEEDED(rc)) rc = svcSignalEvent(ready);
    if (R_SUCCEEDED(rc)) rc = svcWaitSynchronizationSingle(exit_read, UINT64_MAX);

    atomic_store(&ctx.stop_requested, true);
    if (cap_thread_created) {
        threadWaitForExit(&cap_thread);
        threadClose(&cap_thread);
    }

    if (service.started) {
        Result stop_result = pluginServiceStop(&service);
        if (R_SUCCEEDED(rc)) rc = stop_result;
    }

    if (exit_read != INVALID_HANDLE) svcCloseHandle(exit_read);
    if (exit_write != INVALID_HANDLE) svcCloseHandle(exit_write);
    if (ctx.work_buf) free(ctx.work_buf);
    if (ctx.snap_buf) free(ctx.snap_buf);

    if (sd_mounted) fsdevUnmountAll();
    if (fs_inited) fsExit();

    pluginRuntimeExit();
    return rc;
}

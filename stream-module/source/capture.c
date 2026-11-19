// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "stream.h"

void stream_write_log(const char* msg) {
    FILE* f = fopen("sdmc:/kaihen_caps2s.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

void capture_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;

    Service caps2sSrv;
    Result rc = smGetService(&caps2sSrv, "caps2:s");
    atomic_store(&ctx->last_rc, rc);

    char log_line[128];
    snprintf(log_line, sizeof(log_line), "smGetService(caps2:s) -> 0x%x", rc);
    stream_write_log(log_line);

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
            ctx->frame_seq++;
            condvarBroadcast(&ctx->frame_cv);
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

    mutexLock(&ctx->buf_mutex);
    condvarBroadcast(&ctx->frame_cv);
    mutexUnlock(&ctx->buf_mutex);

    atomic_store(&ctx->running, false);
    serviceClose(&caps2sSrv);
    stream_write_log("Capture thread stopped cleanly.");
}

Result capture_save_latest_frame(CaptureContext* ctx, const char* path) {
    Result save_rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    mutexLock(&ctx->buf_mutex);
    if (ctx->snap_size > 0 && ctx->snap_size <= CAP_BUF_SIZE) {
        FILE* f = fopen(path, "wb");
        if (f) {
            size_t written = fwrite(ctx->snap_buf, 1, ctx->snap_size, f);
            fclose(f);
            if (written == ctx->snap_size) {
                save_rc = 0;
            }
        }
    }
    mutexUnlock(&ctx->buf_mutex);
    return save_rc;
}

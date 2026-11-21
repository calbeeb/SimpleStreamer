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
        u64 t0 = armGetSystemTick();
        u64 out_size = 0;
        rc = serviceDispatchInOut(&caps2sSrv, 101, mode, out_size,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out | SfBufferAttr_HipcMapTransferAllowsNonSecure },
            .buffers = { { ctx->bufs[ctx->cap_idx], CAP_BUF_SIZE } },
        );
        u64 t1 = armGetSystemTick();

        atomic_store(&ctx->last_rc, rc);

        if (rc == 0x104110) {
            // Resolution mismatch (Docked 1080p vs Handheld 720p): toggle mode
            mode = (mode == CAPS2S_MODE_1080P_JPEG) ? CAPS2S_MODE_720P_JPEG : CAPS2S_MODE_1080P_JPEG;
            svcSleepThread(50000000ULL); // 50 ms pause
            continue;
        }

        if (rc == 0x10d310) {
            // Layer transition / HOME menu / no active layer: sleep 50ms and wait for layer
            svcSleepThread(50000000ULL);
            continue;
        }

        if (R_SUCCEEDED(rc) && out_size > 0 && out_size <= CAP_BUF_SIZE) {
            // Zero-copy swap: exchange cap_idx with ready_idx
            mutexLock(&ctx->buf_mutex);
            int tmp = ctx->cap_idx;
            ctx->cap_idx = ctx->ready_idx;
            ctx->ready_idx = tmp;
            ctx->ready_size = out_size;
            ctx->frame_seq++;
            ctx->ready_seq = ctx->frame_seq;
            condvarWakeAll(&ctx->frame_cv);
            mutexUnlock(&ctx->buf_mutex);

            u32 cap_us = (u32)(armTicksToNs(t1 - t0) / 1000ULL);
            atomic_store(&ctx->last_cap_us, cap_us);
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
            // Log any unexpected error code once
            static Result last_logged_err = 0;
            if (rc != last_logged_err) {
                last_logged_err = rc;
                char err_buf[64];
                snprintf(err_buf, sizeof(err_buf), "caps2:s Cmd 101 error: 0x%x", rc);
                stream_write_log(err_buf);
            }
            // When capture fails (e.g. HOME menu open / layer transition), back off 100ms
            // to avoid hammering capsrv2 and prevent abort panics (2168-0002)
            svcSleepThread(100000000ULL);
        }
    }

    mutexLock(&ctx->buf_mutex);
    condvarWakeAll(&ctx->frame_cv);
    mutexUnlock(&ctx->buf_mutex);

    atomic_store(&ctx->running, false);
    serviceClose(&caps2sSrv);
    stream_write_log("Capture thread stopped cleanly.");
}

Result capture_save_latest_frame(CaptureContext* ctx, const char* path) {
    Result save_rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    mutexLock(&ctx->buf_mutex);
    if (ctx->ready_size > 0 && ctx->ready_size <= CAP_BUF_SIZE) {
        FILE* f = fopen(path, "wb");
        if (f) {
            size_t written = fwrite(ctx->bufs[ctx->ready_idx], 1, ctx->ready_size, f);
            fclose(f);
            if (written == ctx->ready_size) {
                save_rc = 0;
            }
        }
    }
    mutexUnlock(&ctx->buf_mutex);
    return save_rc;
}

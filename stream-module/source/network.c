// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "stream.h"

#define STREAM_MAGIC 0x5357324A // "SW2J"

typedef struct __attribute__((packed)) {
    u32 magic;      // 0x5357324A
    u32 frame_size; // Frame size in bytes
} TcpFrameHeader;

static bool send_all(int fd, const void* data, size_t len) {
    const u8* ptr = (const u8*)data;
    size_t sent = 0;
    int retry_count = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 262144) chunk = 262144; // 256 KB blocks: ~6 bsdSend IPC calls per 1.5 MB frame
        ssize_t n = send(fd, ptr + sent, chunk, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS || errno == ENOMEM) {
                retry_count++;
                if (retry_count > 500) { // 500 * 10ms = 5 seconds of retries
                    char err_log[128];
                    snprintf(err_log, sizeof(err_log), "send() timed out after 5s retry (errno=%d): sent=%zu/%zu",
                             errno, sent, len);
                    stream_write_log(err_log);
                    return false;
                }
                svcSleepThread(10000000ULL); // 10 ms yield
                continue;
            }
            Result bsd_rc = socketGetLastResult();
            char err_log[160];
            snprintf(err_log, sizeof(err_log), "send() failed: n=%zd, errno=%d (%s), bsd_rc=0x%x, sent=%zu/%zu",
                     n, errno, strerror(errno), bsd_rc, sent, len);
            stream_write_log(err_log);
            return false;
        }
        if (n == 0) {
            stream_write_log("send() returned 0 (client EOF)");
            return false;
        }
        sent += (size_t)n;
        retry_count = 0;
    }
    return true;
}

void network_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;

    stream_write_log("network_thread_func started.");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "TCP socket() failed: errno=%d", errno);
        stream_write_log(err_msg);
        return;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Non-blocking so accept() doesn't block shutdown
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    // Important: Do NOT set sin_len. Horizon OS BSD expects standard { sin_family = AF_INET }
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(STREAM_PORT);

    if (bind(server_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "TCP bind() failed on port %d: errno=%d", STREAM_PORT, errno);
        stream_write_log(err_msg);
        close(server_fd);
        return;
    }

    if (listen(server_fd, 1) < 0) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "TCP listen() failed: errno=%d", errno);
        stream_write_log(err_msg);
        close(server_fd);
        return;
    }

    ctx->listen_fd = server_fd;
    atomic_store(&ctx->net_listening, true);
    stream_write_log("TCP low-latency JPEG streamer listening on port 8080.");

    while (!atomic_load(&ctx->stop_requested)) {
        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            svcSleepThread(50000000ULL); // 50 ms idle sleep waiting for client
            continue;
        }

        if (!atomic_load(&ctx->streaming_enabled)) {
            close(client_fd);
            continue;
        }

        // Ensure client socket is in blocking mode (clear O_NONBLOCK inherited from server_fd)
        int cflags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, cflags & ~O_NONBLOCK);

        // Disable Nagle's algorithm for instant low-latency packet dispatch
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 10-second send timeout to survive heavy app/game launches without aborting
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        atomic_store(&ctx->client_connected, true);
        stream_write_log("Client connected via TCP.");

        u64 last_sent_seq = 0;
        u64 net_window_frames = 0;
        u64 net_window_start_tick = armGetSystemTick();
        u64 perf_log_start_tick = net_window_start_tick;

        while (!atomic_load(&ctx->stop_requested) && atomic_load(&ctx->streaming_enabled)) {
            u64 frame_size = 0;

            mutexLock(&ctx->buf_mutex);
            while (!atomic_load(&ctx->stop_requested) &&
                   atomic_load(&ctx->streaming_enabled) &&
                   ctx->frame_seq == last_sent_seq) {
                Result res = condvarWaitTimeout(&ctx->frame_cv, &ctx->buf_mutex, 100000000ULL);
                if (R_FAILED(res)) {
                    break;
                }
            }

            if (ctx->frame_seq == last_sent_seq) {
                mutexUnlock(&ctx->buf_mutex);
                continue;
            }

            if (!atomic_load(&ctx->stop_requested) &&
                atomic_load(&ctx->streaming_enabled) &&
                ctx->ready_size > 0 &&
                ctx->ready_size <= CAP_BUF_SIZE) {
                // Zero-copy triple-buffer swap: exchange net_idx with ready_idx
                int tmp = ctx->net_idx;
                ctx->net_idx = ctx->ready_idx;
                ctx->ready_idx = tmp;
                frame_size = ctx->ready_size;
                last_sent_seq = ctx->frame_seq;
            }
            mutexUnlock(&ctx->buf_mutex);

            if (frame_size == 0) {
                continue;
            }

            // Write 8-byte header directly into the prefix page immediately before bufs[net_idx]
            u8* pkt = ctx->bufs[ctx->net_idx] - sizeof(TcpFrameHeader);
            TcpFrameHeader hdr = {
                .magic      = STREAM_MAGIC,
                .frame_size = (u32)frame_size,
            };
            memcpy(pkt, &hdr, sizeof(hdr));

            u64 tx0 = armGetSystemTick();

            // Send header + JPEG payload in a single contiguous send_all() pass
            if (!send_all(client_fd, pkt, sizeof(hdr) + frame_size)) {
                stream_write_log("Client disconnected (frame send failed).");
                break;
            }

            u64 tx1 = armGetSystemTick();
            u32 net_us = (u32)(armTicksToNs(tx1 - tx0) / 1000ULL);
            atomic_store(&ctx->last_net_us, net_us);

            net_window_frames++;
            u64 elapsed_ns = armTicksToNs(tx1 - net_window_start_tick);
            if (elapsed_ns >= 1000000000ULL) {
                double nfps = ((double)net_window_frames * 1000000000.0) / (double)elapsed_ns;
                atomic_store(&ctx->net_fps_x100, (u32)(nfps * 100.0));
                net_window_frames = 0;
                net_window_start_tick = tx1;
            }

            if (armTicksToNs(tx1 - perf_log_start_tick) >= 5000000000ULL) {
                perf_log_start_tick = tx1;
                char perf_msg[160];
                snprintf(perf_msg, sizeof(perf_msg),
                         "[PERF] cap=%.1ffps (%.1fms) | net=%.1ffps (%.1fms) | size=%luKB",
                         (double)atomic_load(&ctx->avg_fps_x100) / 100.0,
                         (double)atomic_load(&ctx->last_cap_us) / 1000.0,
                         (double)atomic_load(&ctx->net_fps_x100) / 100.0,
                         (double)net_us / 1000.0,
                         (unsigned long)(frame_size / 1024));
                stream_write_log(perf_msg);
            }
        }

        atomic_store(&ctx->net_fps_x100, 0);
        close(client_fd);
        atomic_store(&ctx->client_connected, false);
    }

    atomic_store(&ctx->net_listening, false);
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    stream_write_log("TCP network thread stopped cleanly.");
}

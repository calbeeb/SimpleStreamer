// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "stream.h"

static bool send_all(int fd, const void* data, size_t len) {
    const u8* ptr = (const u8*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

void network_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        stream_write_log("socket() failed.");
        return;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(STREAM_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        stream_write_log("bind() failed on port 8080.");
        close(server_fd);
        return;
    }

    if (listen(server_fd, 2) < 0) {
        stream_write_log("listen() failed.");
        close(server_fd);
        return;
    }

    ctx->listen_fd = server_fd;
    stream_write_log("MJPEG HTTP server listening on port 8080.");

    while (!atomic_load(&ctx->stop_requested)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (atomic_load(&ctx->stop_requested)) break;
            svcSleepThread(50000000ULL);
            continue;
        }

        if (!atomic_load(&ctx->streaming_enabled)) {
            close(client_fd);
            continue;
        }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char req_buf[512];
        recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);

        const char* http_header =
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

        if (!send_all(client_fd, http_header, strlen(http_header))) {
            close(client_fd);
            continue;
        }

        atomic_store(&ctx->client_connected, true);
        u64 last_sent_seq = 0;

        while (!atomic_load(&ctx->stop_requested) && atomic_load(&ctx->streaming_enabled)) {
            u64 frame_size = 0;

            mutexLock(&ctx->buf_mutex);
            while (!atomic_load(&ctx->stop_requested) &&
                   atomic_load(&ctx->streaming_enabled) &&
                   ctx->frame_seq == last_sent_seq) {
                condvarWait(&ctx->frame_cv, &ctx->buf_mutex);
            }

            if (!atomic_load(&ctx->stop_requested) &&
                atomic_load(&ctx->streaming_enabled) &&
                ctx->snap_size > 0 && ctx->snap_size <= CAP_BUF_SIZE) {
                frame_size = ctx->snap_size;
                memcpy(ctx->net_buf, ctx->snap_buf, frame_size);
                last_sent_seq = ctx->frame_seq;
            }
            mutexUnlock(&ctx->buf_mutex);

            if (frame_size == 0) {
                continue;
            }

            char part_hdr[128];
            int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                                   "--frame\r\n"
                                   "Content-Type: image/jpeg\r\n"
                                   "Content-Length: %lu\r\n\r\n",
                                   frame_size);

            if (!send_all(client_fd, part_hdr, (size_t)hdr_len) ||
                !send_all(client_fd, ctx->net_buf, (size_t)frame_size) ||
                !send_all(client_fd, "\r\n", 2)) {
                break;
            }
        }

        atomic_store(&ctx->client_connected, false);
        close(client_fd);
    }

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    stream_write_log("Network thread stopped cleanly.");
}

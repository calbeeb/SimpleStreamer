// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <malloc.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "stream.h"

#define AUDIO_DEVICE_NAME      "ScoopVideo"
#define AUDIO_FORMAT_Q15       2          // 16-bit signed PCM (s16le)

typedef struct __attribute__((packed)) {
    u32 magic;        // 0x53573241 ("SW2A")
    u32 payload_size; // Raw interleaved 48kHz s16le stereo PCM bytes
} TcpAudioHeader;

typedef struct {
    u32 sample_format;
    u32 channel_count;
    u32 sample_rate;
} Sw2AudioFormat;
_Static_assert(sizeof(Sw2AudioFormat) == 12, "Sw2AudioFormat size");

static Result auddmg_open_user_session(Service* mgr, Service* out_session) {
    const struct {
        u32 client_type;
        u32 pad;
        u64 arg1;
        u64 arg2;
    } in = { 2, 0, 0, 0 };
    _Static_assert(sizeof(in) == 24, "auddmg OpenUserSession in size");

    return serviceDispatchIn(mgr, 0, in,
        .in_send_pid = true,
        .out_num_objects = 1,
        .out_objects = out_session,
    );
}

static Result auddmg_set_format(Service* sess, const char* dev_name, const Sw2AudioFormat* fmt) {
    return serviceDispatchIn(sess, 2000, *fmt,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { dev_name, strlen(dev_name) } },
    );
}

static Result auddmg_get_device_info(Service* sess, const char* dev_name, u8 out_info[0x120]) {
    memset(out_info, 0, 0x120);
    return serviceDispatch(sess, 2301,
        .buffer_attrs = {
            SfBufferAttr_Out | SfBufferAttr_HipcMapAlias,
            SfBufferAttr_In  | SfBufferAttr_HipcMapAlias,
        },
        .buffers = {
            { out_info, 0x120 },
            { dev_name, strlen(dev_name) },
        },
    );
}

static Result auddmg_start_stream(Service* sess, const char* dev_name, u32 stream_id) {
    return serviceDispatchIn(sess, 2100, stream_id,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { dev_name, strlen(dev_name) } },
    );
}

static Result auddmg_stop_stream(Service* sess, u32 stream_id) {
    return serviceDispatchIn(sess, 800, stream_id);
}

static Result audsmx_open_user_session(Service* mgr_domain, Service* out_session) {
    const struct {
        u32 mode;
        u32 pad;
        u64 arg;
    } in = { 1, 0, 0 };
    _Static_assert(sizeof(in) == 16, "audsmx OpenUserSession in size");

    return serviceDispatchIn(mgr_domain, 0, in,
        .in_send_pid = true,
        .in_num_handles = 1,
        .in_handles = { CUR_PROCESS_HANDLE },
        .out_num_objects = 1,
        .out_objects = out_session,
    );
}

static Result audsmx_open_external_stream(
    Service* smx_sess,
    u32 endpoint_id,
    u32 mode,
    const char* name,
    void* ringbuf_va,
    size_t ringbuf_size,
    u32* out_stream_id,
    Service* out_stream
) {
    const struct {
        u32 endpoint_id;
        u32 mode;
        u64 reserved;
        u64 ringbuf_va;
        u64 ringbuf_size;
    } in = { endpoint_id, mode, 0, (u64)(uintptr_t)ringbuf_va, (u64)ringbuf_size };
    _Static_assert(sizeof(in) == 32, "audsmx OpenExternalStream in size");

    return serviceDispatchInOut(smx_sess, 1, in, *out_stream_id,
        .in_send_pid = true,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { name, strlen(name) } },
        .out_num_objects = 1,
        .out_objects = out_stream,
    );
}

static Result audsmx_stream_configure(
    Service* stream,
    const Sw2AudioFormat* fmt,
    u32 ring_frames,
    u32 initial_write,
    u32 enable_timestamps
) {
    const struct {
        Sw2AudioFormat fmt;
        u32 ring_frames;
        u32 initial_write;
        u32 enable_timestamps;
    } in = { *fmt, ring_frames, initial_write, enable_timestamps };
    _Static_assert(sizeof(in) == 24, "audsmx Configure in size");

    return serviceDispatchIn(stream, 0, in);
}

static Result audsmx_stream_unconfigure(Service* stream) {
    return serviceDispatch(stream, 1);
}

static Result audsmx_stream_register_event(Service* stream, u32 event_type, u32 threshold_frames, Event* out_ev) {
    const struct {
        u32 event_type;
        u32 threshold_frames;
    } in = { event_type, threshold_frames };
    _Static_assert(sizeof(in) == 8, "audsmx RegisterEvent in size");

    Handle h = INVALID_HANDLE;
    Result rc = serviceDispatchIn(stream, 7, in,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &h,
    );
    if (R_SUCCEEDED(rc)) {
        eventLoadRemote(out_ev, h, true);
    }
    return rc;
}

static Result audsmx_stream_unregister_event(Service* stream) {
    return serviceDispatch(stream, 8);
}

static Result audsmx_stream_set_name(Service* stream, const char* full_name) {
    return serviceDispatch(stream, 12,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { full_name, strlen(full_name) } },
    );
}

static bool audio_send_all(int fd, const void* data, size_t len) {
    const u8* ptr = (const u8*)data;
    size_t sent = 0;
    int retry_count = 0;
    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS || errno == ENOMEM) {
                retry_count++;
                if (retry_count > 200) {
                    return false;
                }
                svcSleepThread(5000000ULL);
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
        retry_count = 0;
    }
    return true;
}

void audio_capture_close(AudioCaptureSession* s) {
    if (s->started) {
        auddmg_stop_stream(&s->dmg_sess, s->stream_id);
        s->started = false;
    }
    if (s->has_ev) {
        audsmx_stream_unregister_event(&s->smx_stream);
        eventClose(&s->ev);
        s->has_ev = false;
    }
    if (serviceIsActive(&s->smx_stream)) {
        audsmx_stream_unconfigure(&s->smx_stream);
        serviceClose(&s->smx_stream);
    }
    if (serviceIsActive(&s->smx_sess)) {
        serviceClose(&s->smx_sess);
    }
    if (serviceIsActive(&s->smx_mgr)) {
        serviceClose(&s->smx_mgr);
    }
    if (serviceIsActive(&s->dmg_sess)) {
        serviceClose(&s->dmg_sess);
    }
    free(s->ring_base);
    s->ring_base = NULL;
    s->ring_size = 0;
}

Result audio_capture_open(AudioCaptureSession* s) {
    memset(s, 0, sizeof(*s));

    Service dmg_mgr = {0};
    Result rc = smGetService(&dmg_mgr, "auddmg:u");
    if (R_FAILED(rc)) return rc;

    rc = auddmg_open_user_session(&dmg_mgr, &s->dmg_sess);
    serviceClose(&dmg_mgr);
    if (R_FAILED(rc)) goto fail;

    Sw2AudioFormat fmt = {
        .sample_format = AUDIO_FORMAT_Q15,
        .channel_count = AUDIO_CHANNELS,
        .sample_rate   = AUDIO_SAMPLE_RATE,
    };
    rc = auddmg_set_format(&s->dmg_sess, AUDIO_DEVICE_NAME, &fmt);
    if (R_FAILED(rc)) goto fail;

    u8 dev_info[0x120];
    rc = auddmg_get_device_info(&s->dmg_sess, AUDIO_DEVICE_NAME, dev_info);
    if (R_FAILED(rc)) goto fail;

    u64 min_dur_ns = 0;
    u32 endpoint_id = 0;
    memcpy(&min_dur_ns, dev_info + 0x110, sizeof(min_dur_ns));
    memcpy(&endpoint_id, dev_info + 0x118, sizeof(endpoint_id));

    u32 min_frames = (u32)((min_dur_ns * (u64)AUDIO_SAMPLE_RATE + 999999999ULL) / 1000000000ULL);
    u32 period_frames = (min_frames > 480) ? min_frames : 480; // 10 ms low-latency wakeup
    u32 ring_frames   = period_frames * 16;                     // 160+ ms ring headroom
    if (ring_frames < 9600) ring_frames = 9600;                 // >= 200 ms
    u32 ring_slots    = ring_frames + 1;

    size_t pcm_bytes = ((size_t)ring_slots * AUDIO_FRAME_BYTES + 0x3F) & ~(size_t)0x3F;
    size_t ring_size = (0x10C0 + pcm_bytes + 0x1FFF) & ~(size_t)0xFFF;

    s->ring_base = (u8*)memalign(0x1000, ring_size);
    if (!s->ring_base) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto fail;
    }
    s->ring_size   = ring_size;
    s->ring_frames = ring_frames;
    s->ring_slots  = ring_slots;
    memset(s->ring_base, 0, ring_size);
    armDCacheFlush(s->ring_base, ring_size);

    rc = smGetService(&s->smx_mgr, "audsmx:u");
    if (R_FAILED(rc)) goto fail;

    rc = serviceConvertToDomain(&s->smx_mgr);
    if (R_FAILED(rc)) goto fail;

    rc = audsmx_open_user_session(&s->smx_mgr, &s->smx_sess);
    if (R_FAILED(rc)) goto fail;

    const char* stream_name = "OutputRecorder-0<=2ch48kQ15<=ScoopVideo";
    rc = audsmx_open_external_stream(
        &s->smx_sess, endpoint_id, 2, stream_name,
        s->ring_base, s->ring_size, &s->stream_id, &s->smx_stream
    );
    if (R_FAILED(rc)) goto fail;

    // Initialize shared-memory ringbuffer state before Configure (Cmd 0)
    memset(s->ring_base, 0, s->ring_size);
    s->ring_base[0x0000] = 1;
    armDCacheFlush(s->ring_base, s->ring_size);

    rc = audsmx_stream_configure(&s->smx_stream, &fmt, s->ring_frames, 0, 1);
    if (R_FAILED(rc)) goto fail;

    rc = audsmx_stream_register_event(&s->smx_stream, 1, period_frames, &s->ev);
    if (R_SUCCEEDED(rc)) {
        s->has_ev = true;
    }

    audsmx_stream_set_name(&s->smx_stream, "OutputRecorder-0<=2ch48kQ15<=ScoopVideo|stream-module");

    rc = auddmg_start_stream(&s->dmg_sess, AUDIO_DEVICE_NAME, s->stream_id);
    if (R_FAILED(rc)) goto fail;

    s->started = true;
    return 0;

fail:
    audio_capture_close(s);
    return rc;
}

size_t audio_capture_read_pcm(AudioCaptureSession* s, u8* out_pcm, u32 max_frames, bool* out_error) {
    if (out_error) *out_error = false;
    if (!s->started || !s->ring_base) return 0;

    armDCacheFlush(s->ring_base + 0x1000, 0x40);
    u32 write_idx = *(volatile u32*)(s->ring_base + 0x1004);
    u8  idle_flag = *(volatile u8* )(s->ring_base + 0x1008);
    u32 read_idx  = *(volatile u32*)(s->ring_base + 0x0044);

    if (write_idx >= s->ring_slots || read_idx >= s->ring_slots) {
        if (out_error) *out_error = true;
        return 0;
    }
    if (idle_flag & 1) {
        return 0;
    }

    u32 avail = (write_idx >= read_idx)
                ? (write_idx - read_idx)
                : (write_idx + s->ring_slots - read_idx);
    if (avail == 0) {
        return 0;
    }

    u32 take = (avail > max_frames) ? max_frames : avail;
    u32 first = s->ring_slots - read_idx;
    if (first > take) first = take;
    u32 second = take - first;

    u8* pcm_ring = s->ring_base + 0x10C0;
    size_t copied = 0;

    if (first > 0) {
        u8* src = pcm_ring + (size_t)read_idx * AUDIO_FRAME_BYTES;
        size_t bytes = (size_t)first * AUDIO_FRAME_BYTES;
        armDCacheFlush(src, bytes);
        memcpy(out_pcm, src, bytes);
        copied += bytes;
    }
    if (second > 0) {
        size_t bytes = (size_t)second * AUDIO_FRAME_BYTES;
        armDCacheFlush(pcm_ring, bytes);
        memcpy(out_pcm + copied, pcm_ring, bytes);
        copied += bytes;
    }

    read_idx += take;
    if (read_idx >= s->ring_slots) read_idx -= s->ring_slots;
    *(volatile u32*)(s->ring_base + 0x0044) = read_idx;
    armDCacheFlush(s->ring_base + 0x0040, 0x40);

    return copied;
}

static u32 audio_query_current_ip(CaptureContext* ctx) {
    if (!atomic_load(&ctx->nifm_inited)) return 1;
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip))) {
        return 0;
    }
    return ip;
}

static int audio_create_tcp_listener(u16 port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void audio_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;
    stream_write_log("audio_thread_func started.");

    u8* pkt_buf = (u8*)malloc(sizeof(TcpAudioHeader) + AUDIO_MAX_PKT_FRAMES * AUDIO_FRAME_BYTES);
    if (!pkt_buf) {
        stream_write_log("Audio packet buffer alloc failed.");
        return;
    }

    int server_fd = -1;
    u32 bound_ip = 0;
    u64 last_link_check = 0;

    while (!atomic_load(&ctx->stop_requested)) {
        u64 now_tick = armGetSystemTick();

        if (server_fd < 0 || armTicksToNs(now_tick - last_link_check) >= 1000000000ULL) {
            last_link_check = now_tick;
            u32 cur_ip = audio_query_current_ip(ctx);

            bool need_rebind = false;
            if (server_fd >= 0) {
                struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
                int pr = poll(&pfd, 1, 0);
                if (pr < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) || cur_ip == 0 || cur_ip != bound_ip) {
                    need_rebind = true;
                }
            }

            if (need_rebind && server_fd >= 0) {
                close(server_fd);
                server_fd = -1;
                ctx->audio_listen_fd = -1;
                bound_ip = 0;
                atomic_store(&ctx->audio_listening, false);
            }

            if (server_fd < 0 && cur_ip != 0) {
                server_fd = audio_create_tcp_listener(AUDIO_STREAM_PORT);
                if (server_fd >= 0) {
                    ctx->audio_listen_fd = server_fd;
                    bound_ip = cur_ip;
                    atomic_store(&ctx->audio_listening, true);
                    stream_write_log("Audio TCP PCM streamer listening on port 8081 (48kHz 16-bit stereo).");
                } else {
                    svcSleepThread(500000000ULL);
                    continue;
                }
            }
        }

        if (server_fd < 0) {
            svcSleepThread(200000000ULL);
            continue;
        }

        struct sockaddr_in client_addr = {0};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            int err = errno;
            if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
                close(server_fd);
                server_fd = -1;
                ctx->audio_listen_fd = -1;
                bound_ip = 0;
                atomic_store(&ctx->audio_listening, false);
                svcSleepThread(250000000ULL);
                continue;
            }
            svcSleepThread(50000000ULL);
            continue;
        }

        if (!atomic_load(&ctx->streaming_enabled) || !atomic_load(&ctx->audio_enabled)) {
            close(client_fd);
            continue;
        }

        int cflags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, cflags & ~O_NONBLOCK);

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int sndbuf = 32768;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        AudioCaptureSession sess;
        Result open_rc = audio_capture_open(&sess);
        atomic_store(&ctx->audio_last_rc, open_rc);
        if (R_FAILED(open_rc)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Audio capture open failed: rc=0x%x", open_rc);
            stream_write_log(msg);
            close(client_fd);
            svcSleepThread(200000000ULL);
            continue;
        }

        atomic_store(&ctx->audio_client_connected, true);
        stream_write_log("Audio client connected via TCP (port 8081).");

        u8* pcm_ring = sess.ring_base + 0x10C0;
        bool client_ok = true;

        while (client_ok &&
               !atomic_load(&ctx->stop_requested) &&
               atomic_load(&ctx->streaming_enabled) &&
               atomic_load(&ctx->audio_enabled)) {
            if (sess.has_ev) {
                eventWait(&sess.ev, 50000000ULL);
            } else {
                svcSleepThread(10000000ULL);
            }

            armDCacheFlush(sess.ring_base + 0x1000, 0x40);
            u32 write_idx = *(volatile u32*)(sess.ring_base + 0x1004);
            u8  idle_flag = *(volatile u8* )(sess.ring_base + 0x1008);
            u32 read_idx  = *(volatile u32*)(sess.ring_base + 0x0044);

            if (write_idx >= sess.ring_slots || read_idx >= sess.ring_slots) {
                stream_write_log("Audio ringbuffer index out of range; resetting session.");
                break;
            }
            if (idle_flag & 1) {
                continue;
            }

            u32 avail = (write_idx >= read_idx)
                        ? (write_idx - read_idx)
                        : (write_idx + sess.ring_slots - read_idx);

            while (avail > 0 && client_ok) {
                u32 take = (avail > AUDIO_MAX_PKT_FRAMES) ? AUDIO_MAX_PKT_FRAMES : avail;
                u32 first = sess.ring_slots - read_idx;
                if (first > take) first = take;
                u32 second = take - first;

                u8* payload = pkt_buf + sizeof(TcpAudioHeader);
                size_t copied = 0;

                if (first > 0) {
                    u8* src = pcm_ring + (size_t)read_idx * AUDIO_FRAME_BYTES;
                    size_t bytes = (size_t)first * AUDIO_FRAME_BYTES;
                    armDCacheFlush(src, bytes);
                    memcpy(payload, src, bytes);
                    copied += bytes;
                }
                if (second > 0) {
                    size_t bytes = (size_t)second * AUDIO_FRAME_BYTES;
                    armDCacheFlush(pcm_ring, bytes);
                    memcpy(payload + copied, pcm_ring, bytes);
                    copied += bytes;
                }

                read_idx += take;
                if (read_idx >= sess.ring_slots) read_idx -= sess.ring_slots;
                *(volatile u32*)(sess.ring_base + 0x0044) = read_idx;
                armDCacheFlush(sess.ring_base + 0x0040, 0x40);

                TcpAudioHeader hdr = {
                    .magic        = AUDIO_STREAM_MAGIC,
                    .payload_size = (u32)copied,
                };
                memcpy(pkt_buf, &hdr, sizeof(hdr));

                if (!audio_send_all(client_fd, pkt_buf, sizeof(hdr) + copied)) {
                    stream_write_log("Audio client disconnected.");
                    client_ok = false;
                    break;
                }

                avail -= take;
            }
        }

        audio_capture_close(&sess);
        close(client_fd);
        atomic_store(&ctx->audio_client_connected, false);
    }

    free(pkt_buf);
    atomic_store(&ctx->audio_listening, false);
    if (ctx->audio_listen_fd >= 0) {
        close(ctx->audio_listen_fd);
        ctx->audio_listen_fd = -1;
    }
    stream_write_log("Audio TCP thread stopped cleanly.");
}

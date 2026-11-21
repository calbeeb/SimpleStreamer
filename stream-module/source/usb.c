// SPDX-License-Identifier: MIT
// Native Switch 2 usb:ds (usb.gen2 / DsControllerOunce) live video + audio streamer.
// Uses libounce for IDsRootSession/IDsService and Switch 2 native CMIF command IDs
// for IDsInterface (Cmd 0/1/6) and IDsEndpoint (Cmd 0/1/2/3/5).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "stream.h"

#define USB_STREAM_VID 0x1209
#define USB_STREAM_PID 0xBEEB

#define USB_CMD_START  0x54525453u // "STRT"
#define USB_CMD_STOP   0x504F5453u // "STOP"

#define USB_FLAG_VIDEO (1u << 0)
#define USB_FLAG_AUDIO (1u << 1)

#define USB_CTRL_BUF_SIZE   0x1000 // 4 KB page-aligned
#define USB_AUDIO_BUF_SIZE  0x4000 // 16 KB page-aligned (>= 8 + 7680)
#define USB_TX_TIMEOUT_NS   2000000000ULL // 2s timeout to detect dropped PC viewer

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 arg;
} UsbCtrlPacket;

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 payload_size;
} UsbStreamHeader;

typedef struct {
    bool initialized;
    Service s;
    Event setup_event;
} Sw2UsbInterface;

typedef struct {
    bool initialized;
    Service s;
    Event completion_event;
} Sw2UsbEndpoint;

static Result sw2_usb_register_interface(Sw2UsbInterface* out, u8 intf_num) {
    memset(out, 0, sizeof(*out));
    Service* ds = usbDsGetServiceSession();
    Result rc = serviceDispatchIn(ds, 1, intf_num,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
    if (R_FAILED(rc)) return rc;

    Handle ev = INVALID_HANDLE;
    rc = serviceDispatch(&out->s, 1,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &ev,
    );
    if (R_SUCCEEDED(rc)) {
        eventLoadRemote(&out->setup_event, ev, false);
        out->initialized = true;
    } else {
        serviceClose(&out->s);
    }
    return rc;
}

static void sw2_usb_interface_close(Sw2UsbInterface* intf) {
    if (!intf->initialized) return;
    eventClose(&intf->setup_event);
    serviceClose(&intf->s);
    intf->initialized = false;
}

static Result sw2_usb_interface_append_config(Sw2UsbInterface* intf, UsbDeviceSpeed speed,
                                              const void* buf, size_t size) {
    u32 spd = (u32)speed;
    return serviceDispatchIn(&intf->s, 6, spd,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { buf, size } },
    );
}

static Result sw2_usb_interface_register_endpoint(Sw2UsbInterface* intf, Sw2UsbEndpoint* out, u8 ep_addr) {
    memset(out, 0, sizeof(*out));
    Result rc = serviceDispatchIn(&intf->s, 0, ep_addr,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
    if (R_FAILED(rc)) return rc;

    Handle ev = INVALID_HANDLE;
    rc = serviceDispatch(&out->s, 2,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &ev,
    );
    if (R_SUCCEEDED(rc)) {
        eventLoadRemote(&out->completion_event, ev, false);
        out->initialized = true;
    } else {
        serviceClose(&out->s);
    }
    return rc;
}

static void sw2_usb_endpoint_close(Sw2UsbEndpoint* ep) {
    if (!ep->initialized) return;
    eventClose(&ep->completion_event);
    serviceClose(&ep->s);
    ep->initialized = false;
}

static Result sw2_usb_ep_post_async(Sw2UsbEndpoint* ep, void* buf, u32 size, u32* urb_id) {
    const struct {
        u32 size;
        u32 pad;
        u64 buffer;
    } in = { size, 0, (u64)(uintptr_t)buf };
    return serviceDispatchInOut(&ep->s, 0, in, *urb_id);
}

static Result sw2_usb_ep_cancel(Sw2UsbEndpoint* ep) {
    return serviceDispatch(&ep->s, 1);
}

static Result sw2_usb_ep_get_report(Sw2UsbEndpoint* ep, UsbDsReportData* out) {
    return serviceDispatchOut(&ep->s, 3, *out);
}

static Result sw2_usb_ep_set_zlt(Sw2UsbEndpoint* ep, bool zlt) {
    u8 val = zlt ? 1 : 0;
    return serviceDispatchIn(&ep->s, 5, val);
}

typedef struct {
    bool active;
    Sw2UsbInterface intf;
    Sw2UsbEndpoint ep_in;
    Sw2UsbEndpoint ep_out;
} UsbDeviceHandle;

static void usb_dev_close(UsbDeviceHandle* dev) {
    if (!dev->active) return;
    if (dev->ep_in.initialized)  sw2_usb_ep_cancel(&dev->ep_in);
    if (dev->ep_out.initialized) sw2_usb_ep_cancel(&dev->ep_out);
    usbDsDisable();
    sw2_usb_endpoint_close(&dev->ep_in);
    sw2_usb_endpoint_close(&dev->ep_out);
    sw2_usb_interface_close(&dev->intf);
    usbDsExit();
    dev->active = false;
}

static Result usb_dev_open(UsbDeviceHandle* dev) {
    memset(dev, 0, sizeof(*dev));

    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;

    rc = usbDsClearDeviceData();
    if (R_FAILED(rc)) goto fail;

    u8 i_lang = 0, i_manuf = 0, i_prod = 0, i_serial = 0;
    const u16 lang_us = 0x0409;
    rc = usbDsAddUsbLanguageStringDescriptor(&i_lang, &lang_us, 1);
    if (R_FAILED(rc)) goto fail;
    rc = usbDsAddUsbStringDescriptor(&i_manuf, "beebStream");
    if (R_FAILED(rc)) goto fail;
    rc = usbDsAddUsbStringDescriptor(&i_prod, "Switch2 Streamer");
    if (R_FAILED(rc)) goto fail;
    // Keep same serial as probe so Windows reuses existing WinUSB device node cleanly
    rc = usbDsAddUsbStringDescriptor(&i_serial, "SW2USB01");
    if (R_FAILED(rc)) goto fail;

    struct usb_device_descriptor dev_desc = {
        .bLength            = USB_DT_DEVICE_SIZE,
        .bDescriptorType    = USB_DT_DEVICE,
        .bcdUSB             = 0x0210,
        .bDeviceClass       = 0x00,
        .bDeviceSubClass    = 0x00,
        .bDeviceProtocol    = 0x00,
        .bMaxPacketSize0    = 0x40,
        .idVendor           = USB_STREAM_VID,
        .idProduct          = USB_STREAM_PID,
        .bcdDevice          = 0x0100,
        .iManufacturer      = i_manuf,
        .iProduct           = i_prod,
        .iSerialNumber      = i_serial,
        .bNumConfigurations = 0x01,
    };
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev_desc);
    if (R_FAILED(rc)) goto fail;
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev_desc);
    if (R_FAILED(rc)) goto fail;

    struct usb_device_descriptor ss_dev_desc = dev_desc;
    ss_dev_desc.bcdUSB = 0x0300;
    ss_dev_desc.bMaxPacketSize0 = 0x09;
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &ss_dev_desc);
    if (R_FAILED(rc)) goto fail;

    static const u8 bos_desc[0x16] = {
        0x05, 0x0F, 0x16, 0x00, 0x02,
        0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    rc = usbDsSetBinaryObjectStore(bos_desc, sizeof(bos_desc));
    if (R_FAILED(rc)) goto fail;

    rc = sw2_usb_register_interface(&dev->intf, 0);
    if (R_FAILED(rc)) goto fail;

    struct usb_interface_descriptor intf_desc = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0x00,
        .bAlternateSetting  = 0x00,
        .bNumEndpoints      = 0x02,
        .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = 0xFF,
        .bInterfaceProtocol = 0xFF,
        .iInterface         = 0x00,
    };
    struct usb_endpoint_descriptor ep_out_desc = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT | 0x01,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x0040,
        .bInterval        = 0x00,
    };
    struct usb_endpoint_descriptor ep_in_desc = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN | 0x01,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x0040,
        .bInterval        = 0x00,
    };

    // Full-Speed
    rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Full, &intf_desc, sizeof(intf_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Full, &ep_out_desc, sizeof(ep_out_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Full, &ep_in_desc, sizeof(ep_in_desc));
    if (R_FAILED(rc)) goto fail;

    // High-Speed
    ep_out_desc.wMaxPacketSize = 0x0200;
    ep_in_desc.wMaxPacketSize  = 0x0200;
    rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_High, &intf_desc, sizeof(intf_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_High, &ep_out_desc, sizeof(ep_out_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_High, &ep_in_desc, sizeof(ep_in_desc));
    if (R_FAILED(rc)) goto fail;

    // SuperSpeed
    ep_out_desc.wMaxPacketSize = 0x0400;
    ep_in_desc.wMaxPacketSize  = 0x0400;
    struct usb_ss_endpoint_companion_descriptor ss_comp = {
        .bLength           = USB_DT_SS_ENDPOINT_COMPANION_SIZE,
        .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst         = 0x0F,
        .bmAttributes      = 0x00,
        .wBytesPerInterval = 0x0000,
    };
    rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Super, &intf_desc, sizeof(intf_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Super, &ep_out_desc, sizeof(ep_out_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Super, &ss_comp, sizeof(ss_comp));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Super, &ep_in_desc, sizeof(ep_in_desc));
    if (R_SUCCEEDED(rc)) rc = sw2_usb_interface_append_config(&dev->intf, UsbDeviceSpeed_Super, &ss_comp, sizeof(ss_comp));
    if (R_FAILED(rc)) goto fail;

    rc = sw2_usb_interface_register_endpoint(&dev->intf, &dev->ep_out, USB_ENDPOINT_OUT | 0x01);
    if (R_FAILED(rc)) goto fail;
    rc = sw2_usb_interface_register_endpoint(&dev->intf, &dev->ep_in, USB_ENDPOINT_IN | 0x01);
    if (R_FAILED(rc)) goto fail;

    sw2_usb_ep_set_zlt(&dev->ep_in, false);

    rc = usbDsEnable();
    if (R_FAILED(rc)) goto fail;

    dev->active = true;
    return 0;

fail:
    sw2_usb_endpoint_close(&dev->ep_in);
    sw2_usb_endpoint_close(&dev->ep_out);
    sw2_usb_interface_close(&dev->intf);
    usbDsExit();
    return rc;
}

// Send 1 page-aligned URB synchronously with timeout
static bool usb_send_one(Sw2UsbEndpoint* ep, void* buf, u32 size) {
    u32 urb_id = 0;
    armDCacheFlush(buf, size);
    Result rc = sw2_usb_ep_post_async(ep, buf, size, &urb_id);
    if (R_FAILED(rc)) return false;

    rc = eventWait(&ep->completion_event, USB_TX_TIMEOUT_NS);
    if (R_FAILED(rc)) {
        sw2_usb_ep_cancel(ep);
        eventClear(&ep->completion_event);
        return false;
    }
    eventClear(&ep->completion_event);

    UsbDsReportData rep;
    memset(&rep, 0, sizeof(rep));
    rc = sw2_usb_ep_get_report(ep, &rep);
    if (R_FAILED(rc)) return false;

    u32 req_sz = 0, xfer_sz = 0;
    rc = usbDsParseReportData(&rep, urb_id, &req_sz, &xfer_sz);
    return R_SUCCEEDED(rc) && xfer_sz == size;
}

// Pipelined 2-URB send: 8-byte header + zero-copy page-aligned JPEG payload back-to-back
static bool usb_send_two(Sw2UsbEndpoint* ep, void* buf0, u32 size0, void* buf1, u32 size1) {
    u32 id[2] = {0, 0};
    u32 sz[2] = {size0, size1};
    bool done[2] = {false, false};

    armDCacheFlush(buf0, size0);
    armDCacheFlush(buf1, size1);

    if (R_FAILED(sw2_usb_ep_post_async(ep, buf0, size0, &id[0]))) return false;
    if (R_FAILED(sw2_usb_ep_post_async(ep, buf1, size1, &id[1]))) {
        sw2_usb_ep_cancel(ep);
        eventClear(&ep->completion_event);
        return false;
    }

    u64 deadline = armGetSystemTick() + armNsToTicks(USB_TX_TIMEOUT_NS);
    while (!done[0] || !done[1]) {
        u64 now = armGetSystemTick();
        if (now >= deadline) {
            sw2_usb_ep_cancel(ep);
            eventClear(&ep->completion_event);
            return false;
        }
        u64 rem_ns = armTicksToNs(deadline - now);
        Result rc = eventWait(&ep->completion_event, rem_ns);
        if (R_FAILED(rc)) {
            sw2_usb_ep_cancel(ep);
            eventClear(&ep->completion_event);
            return false;
        }
        eventClear(&ep->completion_event);

        UsbDsReportData rep;
        memset(&rep, 0, sizeof(rep));
        if (R_FAILED(sw2_usb_ep_get_report(ep, &rep))) {
            sw2_usb_ep_cancel(ep);
            return false;
        }

        for (int i = 0; i < 2; i++) {
            if (done[i]) continue;
            u32 req_sz = 0, xfer_sz = 0;
            if (R_SUCCEEDED(usbDsParseReportData(&rep, id[i], &req_sz, &xfer_sz))) {
                if (xfer_sz != sz[i]) {
                    sw2_usb_ep_cancel(ep);
                    return false;
                }
                done[i] = true;
            }
        }
    }
    return true;
}

static bool usb_drain_audio(Sw2UsbEndpoint* ep_in, AudioCaptureSession* audio_sess, u8* audio_tx_buf) {
    if (!audio_sess->started) return true;

    for (int iter = 0; iter < 4; iter++) {
        bool ring_err = false;
        size_t pcm_bytes = audio_capture_read_pcm(
            audio_sess,
            audio_tx_buf + sizeof(UsbStreamHeader),
            AUDIO_MAX_PKT_FRAMES,
            &ring_err
        );
        if (ring_err) {
            stream_write_log("USB audio ringbuffer out of range; stopping USB audio.");
            audio_capture_close(audio_sess);
            return true;
        }
        if (pcm_bytes == 0) {
            break;
        }
        UsbStreamHeader hdr = {
            .magic        = AUDIO_STREAM_MAGIC,
            .payload_size = (u32)pcm_bytes,
        };
        memcpy(audio_tx_buf, &hdr, sizeof(hdr));
        if (!usb_send_one(ep_in, audio_tx_buf, (u32)(sizeof(hdr) + pcm_bytes))) {
            return false;
        }
    }
    return true;
}

void usb_thread_func(void* arg) {
    CaptureContext* ctx = (CaptureContext*)arg;
    stream_write_log("usb_thread_func started.");

    u8* rx_buf       = (u8*)memalign(0x1000, USB_CTRL_BUF_SIZE);
    u8* hdr_tx_buf   = (u8*)memalign(0x1000, USB_CTRL_BUF_SIZE);
    u8* audio_tx_buf = (u8*)memalign(0x1000, USB_AUDIO_BUF_SIZE);
    if (!rx_buf || !hdr_tx_buf || !audio_tx_buf) {
        stream_write_log("USB thread DMA buffer alloc failed.");
        free(rx_buf);
        free(hdr_tx_buf);
        free(audio_tx_buf);
        return;
    }

    UsbDeviceHandle dev = {0};
    AudioCaptureSession audio_sess = {0};
    bool rx_posted = false;
    u32 rx_urb_id = 0;
    bool client_streaming = false;
    u32 client_flags = 0;

    u64 last_sent_seq = 0;
    u64 usb_window_frames = 0;
    u64 usb_window_start_tick = 0;

    while (!atomic_load(&ctx->stop_requested)) {
        bool want_usb = atomic_load(&ctx->usb_enabled);

        if (!want_usb) {
            if (client_streaming) {
                audio_capture_close(&audio_sess);
                client_streaming = false;
                atomic_store(&ctx->usb_client_connected, false);
                atomic_store(&ctx->usb_fps_x100, 0);
            }
            if (dev.active) {
                usb_dev_close(&dev);
                rx_posted = false;
                atomic_store(&ctx->usb_active, false);
                atomic_store(&ctx->usb_state, UsbState_Detached);
                atomic_store(&ctx->usb_speed, UsbDeviceSpeed_None);
                stream_write_log("USB device mode disabled (usb:ds released).");
            }
            svcSleepThread(50000000ULL);
            continue;
        }

        if (!dev.active) {
            Result rc = usb_dev_open(&dev);
            atomic_store(&ctx->usb_last_rc, rc);
            if (R_FAILED(rc)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "USB device open failed: rc=0x%x", rc);
                stream_write_log(msg);
                svcSleepThread(1000000000ULL);
                continue;
            }
            atomic_store(&ctx->usb_active, true);
            stream_write_log("USB device mode active (VID=1209 PID=BEEB, bottom USB-C port).");
        }

        UsbState st = UsbState_Detached;
        UsbDeviceSpeed sp = UsbDeviceSpeed_None;
        usbDsGetState(&st);
        usbDsGetSpeed(&sp);
        atomic_store(&ctx->usb_state, (u32)st);
        atomic_store(&ctx->usb_speed, (u32)sp);

        if (st != UsbState_Configured) {
            if (client_streaming) {
                audio_capture_close(&audio_sess);
                client_streaming = false;
                atomic_store(&ctx->usb_client_connected, false);
                atomic_store(&ctx->usb_fps_x100, 0);
                stream_write_log("USB host disconnected.");
            }
            if (rx_posted) {
                sw2_usb_ep_cancel(&dev.ep_out);
                eventClear(&dev.ep_out.completion_event);
                rx_posted = false;
            }
            svcSleepThread(50000000ULL);
            continue;
        }

        // Keep an async Bulk OUT read posted to receive STRT / STOP control packets
        if (!rx_posted) {
            memset(rx_buf, 0, sizeof(UsbCtrlPacket));
            armDCacheFlush(rx_buf, USB_CTRL_BUF_SIZE);
            if (R_SUCCEEDED(sw2_usb_ep_post_async(&dev.ep_out, rx_buf, USB_CTRL_BUF_SIZE, &rx_urb_id))) {
                rx_posted = true;
            }
        }

        // Poll Bulk OUT non-blocking when streaming, or 30ms timeout when idle
        if (rx_posted) {
            u64 wait_ns = client_streaming ? 0ULL : 30000000ULL;
            if (R_SUCCEEDED(eventWait(&dev.ep_out.completion_event, wait_ns))) {
                eventClear(&dev.ep_out.completion_event);
                rx_posted = false;

                UsbDsReportData rep;
                memset(&rep, 0, sizeof(rep));
                u32 req_sz = 0, xfer_sz = 0;
                if (R_SUCCEEDED(sw2_usb_ep_get_report(&dev.ep_out, &rep)) &&
                    R_SUCCEEDED(usbDsParseReportData(&rep, rx_urb_id, &req_sz, &xfer_sz)) &&
                    xfer_sz >= sizeof(UsbCtrlPacket)) {
                    armDCacheFlush(rx_buf, sizeof(UsbCtrlPacket));
                    UsbCtrlPacket pkt;
                    memcpy(&pkt, rx_buf, sizeof(pkt));

                    if (pkt.magic == USB_CMD_START) {
                        client_flags = pkt.arg ? pkt.arg : (USB_FLAG_VIDEO | USB_FLAG_AUDIO);
                        client_streaming = true;
                        atomic_store(&ctx->usb_client_connected, true);
                        last_sent_seq = 0;
                        usb_window_frames = 0;
                        usb_window_start_tick = armGetSystemTick();

                        char msg[128];
                        snprintf(msg, sizeof(msg), "USB client connected (flags=0x%x, speed=%u).",
                                 client_flags, (u32)sp);
                        stream_write_log(msg);

                        if ((client_flags & USB_FLAG_AUDIO) && atomic_load(&ctx->audio_enabled)) {
                            if (!audio_sess.started) {
                                Result arc = audio_capture_open(&audio_sess);
                                atomic_store(&ctx->audio_last_rc, arc);
                                if (R_FAILED(arc)) {
                                    snprintf(msg, sizeof(msg), "USB audio open failed: rc=0x%x", arc);
                                    stream_write_log(msg);
                                }
                            }
                        } else if (audio_sess.started) {
                            audio_capture_close(&audio_sess);
                        }
                    } else if (pkt.magic == USB_CMD_STOP) {
                        if (client_streaming) {
                            stream_write_log("USB client sent STOP.");
                            audio_capture_close(&audio_sess);
                            client_streaming = false;
                            atomic_store(&ctx->usb_client_connected, false);
                            atomic_store(&ctx->usb_fps_x100, 0);
                        }
                    }
                }
            }
        }

        if (!client_streaming) {
            continue;
        }

        if (!atomic_load(&ctx->streaming_enabled)) {
            audio_capture_close(&audio_sess);
            svcSleepThread(30000000ULL);
            continue;
        }

        // Sync USB audio capture with live audio_enabled toggle
        bool want_audio = ((client_flags & USB_FLAG_AUDIO) != 0) && atomic_load(&ctx->audio_enabled);
        if (want_audio && !audio_sess.started) {
            Result arc = audio_capture_open(&audio_sess);
            atomic_store(&ctx->audio_last_rc, arc);
        } else if (!want_audio && audio_sess.started) {
            audio_capture_close(&audio_sess);
        }

        // Drain any ready PCM audio first (fast, <= 8 KB)
        if (!usb_drain_audio(&dev.ep_in, &audio_sess, audio_tx_buf)) {
            stream_write_log("USB client disconnected (audio TX timeout/error).");
            audio_capture_close(&audio_sess);
            client_streaming = false;
            atomic_store(&ctx->usb_client_connected, false);
            atomic_store(&ctx->usb_fps_x100, 0);
            continue;
        }

        // Wait up to 10 ms for next captured video frame so audio keeps draining smoothly on static screens
        u64 frame_size = 0;
        if (client_flags & USB_FLAG_VIDEO) {
            mutexLock(&ctx->buf_mutex);
            if (!atomic_load(&ctx->stop_requested) &&
                atomic_load(&ctx->streaming_enabled) &&
                ctx->ready_seq <= last_sent_seq) {
                condvarWaitTimeout(&ctx->frame_cv, &ctx->buf_mutex, 10000000ULL);
            }

            if (ctx->ready_seq > last_sent_seq &&
                !atomic_load(&ctx->stop_requested) &&
                atomic_load(&ctx->streaming_enabled) &&
                ctx->ready_size > 0 &&
                ctx->ready_size <= CAP_BUF_SIZE) {
                bool tcp_also_needs = atomic_load(&ctx->client_connected) &&
                                      (ctx->net_last_seq < ctx->ready_seq);
                frame_size = ctx->ready_size;
                last_sent_seq = ctx->ready_seq;
                ctx->usb_last_seq = last_sent_seq;
                if (tcp_also_needs) {
                    memcpy(ctx->bufs[ctx->usb_idx], ctx->bufs[ctx->ready_idx], frame_size);
                } else {
                    int tmp = ctx->usb_idx;
                    ctx->usb_idx = ctx->ready_idx;
                    ctx->ready_idx = tmp;
                    ctx->ready_seq = 0;
                }
            }
            mutexUnlock(&ctx->buf_mutex);
        } else {
            svcSleepThread(10000000ULL);
        }

        if (frame_size > 0) {
            UsbStreamHeader hdr = {
                .magic        = STREAM_MAGIC,
                .payload_size = (u32)frame_size,
            };
            memcpy(hdr_tx_buf, &hdr, sizeof(hdr));

            if (!usb_send_two(&dev.ep_in, hdr_tx_buf, sizeof(hdr),
                              ctx->bufs[ctx->usb_idx], (u32)frame_size)) {
                stream_write_log("USB client disconnected (video TX timeout/error).");
                audio_capture_close(&audio_sess);
                client_streaming = false;
                atomic_store(&ctx->usb_client_connected, false);
                atomic_store(&ctx->usb_fps_x100, 0);
                continue;
            }

            usb_window_frames++;
            u64 now = armGetSystemTick();
            u64 elapsed_ns = armTicksToNs(now - usb_window_start_tick);
            if (elapsed_ns >= 1000000000ULL) {
                double ufps = ((double)usb_window_frames * 1000000000.0) / (double)elapsed_ns;
                atomic_store(&ctx->usb_fps_x100, (u32)(ufps * 100.0));
                usb_window_frames = 0;
                usb_window_start_tick = now;
            }
        }
    }

    audio_capture_close(&audio_sess);
    usb_dev_close(&dev);
    atomic_store(&ctx->usb_active, false);
    atomic_store(&ctx->usb_client_connected, false);
    atomic_store(&ctx->usb_fps_x100, 0);
    free(rx_buf);
    free(hdr_tx_buf);
    free(audio_tx_buf);
    stream_write_log("USB thread stopped cleanly.");
}

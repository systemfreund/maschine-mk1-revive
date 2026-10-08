#include "mk1_usb.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mk1_display.h"
#include "mk1_log.h"
#include "mk1_proto.h"

#define READERS_PER_ENDPOINT 4
#define WRITE_TIMEOUT_MS     500

typedef struct {
    uint8_t  address;
    uint8_t  type;          // LIBUSB_TRANSFER_TYPE_BULK / _INTERRUPT
    uint16_t max_packet;
    bool     present;
} endpoint_info_t;

struct mk1_usb {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    mk1_usb_callbacks_t   cb;
    void                 *cb_ctx;

    endpoint_info_t ep_cmd_in, ep_pads_in, ep_cmd_out, ep_display_out;

    struct libusb_transfer *readers[2 * READERS_PER_ENDPOINT];
    int                     reader_count;
    atomic_int              in_flight;
    atomic_bool             alive;
    atomic_bool             closing;
    pthread_mutex_t         write_lock;
};

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void LIBUSB_CALL reader_done(struct libusb_transfer *t)
{
    mk1_usb_t *usb = t->user_data;

    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
        if (t->endpoint == MK1_EP_PADS_IN) {
            if (usb->cb.pads_in) usb->cb.pads_in(usb->cb_ctx, t->buffer, (size_t)t->actual_length);
        } else if (usb->cb.ep1_in) {
            usb->cb.ep1_in(usb->cb_ctx, t->buffer, (size_t)t->actual_length);
        }
    }

    bool resubmit = !atomic_load(&usb->closing) &&
                    (t->status == LIBUSB_TRANSFER_COMPLETED ||
                     t->status == LIBUSB_TRANSFER_TIMED_OUT);
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        atomic_store(&usb->alive, false);
    } else if (t->status == LIBUSB_TRANSFER_STALL) {
        // clear_halt is synchronous and must not run on the event thread;
        // dropping the reader marks the device dead and main reopens it.
        MK1_LOG("input endpoint 0x%02x stalled", t->endpoint);
    } else if (t->status == LIBUSB_TRANSFER_ERROR || t->status == LIBUSB_TRANSFER_OVERFLOW) {
        MK1_LOG("input transfer on 0x%02x failed (status %d)", t->endpoint, t->status);
        resubmit = !atomic_load(&usb->closing);
    }

    if (resubmit && libusb_submit_transfer(t) == 0) {
        return;
    }
    if (!atomic_load(&usb->closing)) {
        atomic_store(&usb->alive, false);
    }
    atomic_fetch_sub(&usb->in_flight, 1);
}

static bool start_reader(mk1_usb_t *usb, const endpoint_info_t *ep)
{
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    // One max-size packet per transfer: a larger buffer would wait for several
    // full packets before completing and add latency to every pad hit.
    int len = ep->max_packet ? ep->max_packet : 64;
    uint8_t *buf = malloc((size_t)len);
    if (!t || !buf) {
        libusb_free_transfer(t);
        free(buf);
        return false;
    }
    if (ep->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        libusb_fill_interrupt_transfer(t, usb->handle, ep->address, buf, len, reader_done, usb, 0);
    } else {
        libusb_fill_bulk_transfer(t, usb->handle, ep->address, buf, len, reader_done, usb, 0);
    }
    t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
    if (libusb_submit_transfer(t) != 0) {
        libusb_free_transfer(t);
        return false;
    }
    usb->readers[usb->reader_count++] = t;
    atomic_fetch_add(&usb->in_flight, 1);
    return true;
}

static void lookup_endpoints(mk1_usb_t *usb)
{
    struct libusb_config_descriptor *config = NULL;
    libusb_device *dev = libusb_get_device(usb->handle);

    if (libusb_get_active_config_descriptor(dev, &config) != 0) {
        return;
    }
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &config->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
            if (alt->bInterfaceNumber != MK1_INTERFACE || alt->bAlternateSetting != MK1_ALT_SETTING) {
                continue;
            }
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *d = &alt->endpoint[e];
                endpoint_info_t info = {
                    .address = d->bEndpointAddress,
                    .type = d->bmAttributes & 0x03,
                    .max_packet = d->wMaxPacketSize & 0x7ff,
                    .present = true,
                };
                MK1_DEBUG("endpoint 0x%02x type=%u maxpacket=%u", info.address, info.type, info.max_packet);
                switch (d->bEndpointAddress) {
                case MK1_EP_CMD_IN:      usb->ep_cmd_in = info; break;
                case MK1_EP_PADS_IN:     usb->ep_pads_in = info; break;
                case MK1_EP_CMD_OUT:     usb->ep_cmd_out = info; break;
                case MK1_EP_DISPLAY_OUT: usb->ep_display_out = info; break;
                default: break;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
}

mk1_usb_t *mk1_usb_open(libusb_context *ctx, const mk1_usb_callbacks_t *cb,
                        void *cb_ctx, char *err, size_t err_len)
{
    libusb_device_handle *handle = libusb_open_device_with_vid_pid(ctx, MK1_VENDOR_ID, MK1_PRODUCT_ID);
    int rc;

    if (!handle) {
        // open_device_with_vid_pid hides the reason; probe once to report EACCES.
        libusb_device **list = NULL;
        ssize_t n = libusb_get_device_list(ctx, &list);
        snprintf(err, err_len, "no Maschine MK1 connected");
        for (ssize_t i = 0; i < n; i++) {
            struct libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
                desc.idVendor == MK1_VENDOR_ID && desc.idProduct == MK1_PRODUCT_ID) {
                libusb_device_handle *h = NULL;
                rc = libusb_open(list[i], &h);
                if (rc == 0) {
                    handle = h;
                } else {
                    snprintf(err, err_len, "cannot open MK1: %s%s", libusb_error_name(rc),
                             rc == LIBUSB_ERROR_ACCESS ? " (install the udev rule, see README)" : "");
                }
                break;
            }
        }
        libusb_free_device_list(list, 1);
        if (!handle) return NULL;
    }

    mk1_usb_t *usb = calloc(1, sizeof(*usb));
    if (!usb) {
        libusb_close(handle);
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    usb->ctx = ctx;
    usb->handle = handle;
    usb->cb = *cb;
    usb->cb_ctx = cb_ctx;
    atomic_init(&usb->in_flight, 0);
    atomic_init(&usb->alive, true);
    atomic_init(&usb->closing, false);
    pthread_mutex_init(&usb->write_lock, NULL);

    // snd-usb-caiaq binds the MK1 by default; take it over and hand it back on close.
    libusb_set_auto_detach_kernel_driver(handle, 1);

    rc = libusb_claim_interface(handle, MK1_INTERFACE);
    if (rc != 0) {
        snprintf(err, err_len, "cannot claim interface: %s%s", libusb_error_name(rc),
                 rc == LIBUSB_ERROR_BUSY ? " (another program owns the MK1)" : "");
        goto fail;
    }
    rc = libusb_set_interface_alt_setting(handle, MK1_INTERFACE, MK1_ALT_SETTING);
    if (rc != 0) {
        snprintf(err, err_len, "cannot select alt setting %d: %s", MK1_ALT_SETTING, libusb_error_name(rc));
        goto fail_release;
    }

    lookup_endpoints(usb);
    if (!usb->ep_cmd_in.present || !usb->ep_pads_in.present || !usb->ep_cmd_out.present) {
        snprintf(err, err_len, "unexpected endpoint layout (is this really a Maschine MK1?)");
        goto fail_release;
    }

    for (int i = 0; i < READERS_PER_ENDPOINT; i++) {
        if (!start_reader(usb, &usb->ep_cmd_in) || !start_reader(usb, &usb->ep_pads_in)) {
            snprintf(err, err_len, "cannot start input transfers");
            mk1_usb_close(usb);
            return NULL;
        }
    }
    return usb;

fail_release:
    libusb_release_interface(handle, MK1_INTERFACE);
fail:
    libusb_close(handle);
    pthread_mutex_destroy(&usb->write_lock);
    free(usb);
    return NULL;
}

bool mk1_usb_write(mk1_usb_t *usb, uint8_t endpoint, const uint8_t *data, size_t len)
{
    const endpoint_info_t *ep = endpoint == MK1_EP_DISPLAY_OUT ? &usb->ep_display_out : &usb->ep_cmd_out;
    int transferred = 0;
    int rc;

    if (!atomic_load(&usb->alive) || !ep->present) {
        return false;
    }

    pthread_mutex_lock(&usb->write_lock);
    if (ep->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        rc = libusb_interrupt_transfer(usb->handle, endpoint, (uint8_t *)data, (int)len,
                                       &transferred, WRITE_TIMEOUT_MS);
    } else {
        rc = libusb_bulk_transfer(usb->handle, endpoint, (uint8_t *)data, (int)len,
                                  &transferred, WRITE_TIMEOUT_MS);
    }
    pthread_mutex_unlock(&usb->write_lock);

    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        atomic_store(&usb->alive, false);
    }
    if (rc != 0) {
        MK1_DEBUG("write to 0x%02x (%zu bytes) failed: %s", endpoint, len, libusb_error_name(rc));
        return false;
    }
    return (size_t)transferred == len;
}

static bool emit_ep8(void *ctx, const uint8_t *packet, size_t len)
{
    return mk1_usb_write(ctx, MK1_EP_DISPLAY_OUT, packet, len);
}

bool mk1_usb_init_hardware(mk1_usb_t *usb)
{
    // caiaq bring-up, same order as the macOS bridge. Replies arrive on the
    // EP1 reader and are ignored; the device just needs them to be drained.
    static const uint8_t get_info[] = { MK1_CMD_GET_DEVICE_INFO };
    // AUTO_MSG: digital=1, analog=2, erp=5 (macOS kext capture)
    static const uint8_t auto_msg[] = { MK1_CMD_AUTO_MSG, 0x01, 0x02, 0x05 };

    if (!mk1_usb_write(usb, MK1_EP_CMD_OUT, get_info, sizeof(get_info))) return false;
    sleep_ms(50);
    if (!mk1_usb_write(usb, MK1_EP_CMD_OUT, auto_msg, sizeof(auto_msg))) return false;
    sleep_ms(10);

    if (!usb->ep_display_out.present) {
        MK1_LOG("EP8 missing - displays disabled");
        return true;
    }
    for (unsigned d = 0; d < MK1_DISPLAY_COUNT; d++) {
        for (size_t i = 0; i < mk1_display_init_cmd_count; i++) {
            mk1_display_send_cmd(d, mk1_display_init_cmds[i].bytes, mk1_display_init_cmds[i].len,
                                 emit_ep8, usb);
            sleep_ms(2);
        }
    }
    return true;
}

bool mk1_usb_alive(mk1_usb_t *usb)
{
    return usb && atomic_load(&usb->alive);
}

void mk1_usb_close(mk1_usb_t *usb)
{
    if (!usb) return;

    atomic_store(&usb->closing, true);
    for (int i = 0; i < usb->reader_count; i++) {
        libusb_cancel_transfer(usb->readers[i]);
    }
    // The event thread completes the cancellations; wait for all callbacks.
    for (int waited = 0; atomic_load(&usb->in_flight) > 0 && waited < 2000; waited += 5) {
        sleep_ms(5);
    }
    if (atomic_load(&usb->in_flight) == 0) {
        for (int i = 0; i < usb->reader_count; i++) {
            libusb_free_transfer(usb->readers[i]);
        }
    } else {
        MK1_LOG("input transfers did not finish; leaking them");
    }

    libusb_release_interface(usb->handle, MK1_INTERFACE);
    libusb_close(usb->handle);
    pthread_mutex_destroy(&usb->write_lock);
    free(usb);
}

// libusb transport for the MK1: device open/claim, async input readers,
// synchronous output writes and the caiaq + display init sequence.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

typedef struct mk1_usb mk1_usb_t;

typedef struct {
    // Called on the libusb event thread.
    void (*ep1_in)(void *ctx, const uint8_t *data, size_t len);
    void (*pads_in)(void *ctx, const uint8_t *data, size_t len);
} mk1_usb_callbacks_t;

// Opens the first MK1 found, detaches snd-usb-caiaq if bound, claims the
// interface and starts the input readers. The caller must already be pumping
// libusb events on another thread. Returns NULL (with a message in err) if no
// device is present or it cannot be opened.
mk1_usb_t *mk1_usb_open(libusb_context *ctx, const mk1_usb_callbacks_t *cb,
                        void *cb_ctx, char *err, size_t err_len);

// Sends GET_DEVICE_INFO, AUTO_MSG and the display init sequence.
bool mk1_usb_init_hardware(mk1_usb_t *usb);

bool mk1_usb_write(mk1_usb_t *usb, uint8_t endpoint, const uint8_t *data, size_t len);

// False once the device was unplugged or an input reader died.
bool mk1_usb_alive(mk1_usb_t *usb);

void mk1_usb_close(mk1_usb_t *usb);

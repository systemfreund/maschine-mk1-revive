// Maschine MK1 wire-protocol constants for the Linux userspace driver.
//
// Everything in here is derived from the macOS bridge (mk1-usb/, mk1-bridge/)
// and the CABL reference (cabl.cpp at repo root). See CLAUDE.md for the
// capture evidence behind each value.
#pragma once

#include <stdint.h>

#define MK1_VENDOR_ID  0x17cc
#define MK1_PRODUCT_ID 0x0808

#define MK1_INTERFACE         0
#define MK1_ALT_SETTING       1

#define MK1_EP_CMD_OUT        0x01   // caiaq commands: DIMM_LEDS, AUTO_MSG, MIDI out
#define MK1_EP_CMD_IN         0x81   // buttons, encoders, command replies, MIDI in
#define MK1_EP_PADS_IN        0x84   // 64-byte pad pressure reports (~700 Hz)
#define MK1_EP_DISPLAY_OUT    0x08   // ST7529 display commands + framebuffer

// EP1 command bytes
#define MK1_CMD_GET_DEVICE_INFO 0x01
#define MK1_CMD_MIDI_WRITE      0x07
#define MK1_CMD_AUTO_MSG        0x0b
#define MK1_CMD_DIMM_LEDS       0x0c

// EP1 IN report types (byte 0)
#define MK1_REPORT_MIDI_IN      0x06
#define MK1_REPORT_ENCODERS     0x02  // 33 bytes
#define MK1_REPORT_BUTTONS      0x04  // 8 bytes

#define MK1_PAD_COUNT       16
#define MK1_PAD_REPORT_LEN  64
#define MK1_PAD_MAX         4095

// ---------------------------------------------------------------------------
// Buttons. The order here is our own; see mk1_input.c for the EP1 bit map.
// ---------------------------------------------------------------------------
typedef enum {
    MK1_BTN_MUTE = 0,
    MK1_BTN_SOLO,
    MK1_BTN_SELECT,
    MK1_BTN_DUPLICATE,
    MK1_BTN_NAVIGATE,
    MK1_BTN_PAD_MODE,
    MK1_BTN_PATTERN,
    MK1_BTN_SCENE,

    MK1_BTN_RESTART,
    MK1_BTN_TRANSPORT_LEFT,
    MK1_BTN_TRANSPORT_RIGHT,
    MK1_BTN_GRID,
    MK1_BTN_PLAY,
    MK1_BTN_REC,
    MK1_BTN_ERASE,
    MK1_BTN_SHIFT,

    MK1_BTN_GROUP_A,
    MK1_BTN_GROUP_B,
    MK1_BTN_GROUP_C,
    MK1_BTN_GROUP_D,
    MK1_BTN_GROUP_E,
    MK1_BTN_GROUP_F,
    MK1_BTN_GROUP_G,
    MK1_BTN_GROUP_H,

    MK1_BTN_CONTROL,
    MK1_BTN_STEP,
    MK1_BTN_BROWSE,
    MK1_BTN_SAMPLING,
    MK1_BTN_LEFT,
    MK1_BTN_RIGHT,
    MK1_BTN_SNAP,
    MK1_BTN_AUTO_WRITE,

    MK1_BTN_SCREEN1,
    MK1_BTN_SCREEN2,
    MK1_BTN_SCREEN3,
    MK1_BTN_SCREEN4,
    MK1_BTN_SCREEN5,
    MK1_BTN_SCREEN6,
    MK1_BTN_SCREEN7,
    MK1_BTN_SCREEN8,

    MK1_BTN_NOTE_REPEAT,

    MK1_BTN_COUNT
} mk1_button_t;

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------
typedef enum {
    MK1_ENC_VOLUME = 0,
    MK1_ENC_TEMPO,
    MK1_ENC_SWING,
    MK1_ENC_SCREEN1,   // left display, leftmost
    MK1_ENC_SCREEN2,
    MK1_ENC_SCREEN3,
    MK1_ENC_SCREEN4,
    MK1_ENC_SCREEN5,   // right display, leftmost
    MK1_ENC_SCREEN6,
    MK1_ENC_SCREEN7,
    MK1_ENC_SCREEN8,
    MK1_ENC_COUNT
} mk1_encoder_t;

// ---------------------------------------------------------------------------
// LED array: 62 slots, sent as two DIMM_LEDS packets
//   { 0x0c, 0x00, leds[0..30]  }
//   { 0x0c, 0x1e, leds[31..61] }
// Slot order is CABL's MaschineMK1::Led enum (hardware-verified by the bridge).
// ---------------------------------------------------------------------------
#define MK1_LED_COUNT          62
#define MK1_LED_BLOCK_A_LEN    31
#define MK1_LED_UNUSED         0xff
#define MK1_LED_BACKLIGHT      58
#define MK1_LED_BACKLIGHT_ON   0x5c

// Firmware brightness tiers (Frida trace of NIHA)
#define MK1_LED_DIM            0x13
#define MK1_LED_MEDIUM         0x32
#define MK1_LED_BRIGHT         0x5c

// Pad rubber LED slot for 0-based pad index (pad 1 = bottom-left = index 0).
static inline uint8_t mk1_pad_led_slot(unsigned pad)
{
    return (uint8_t)((pad / 4) * 4 + (3 - (pad % 4)));
}

// ---------------------------------------------------------------------------
// Displays: two ST7529 panels, 255x64 logical pixels each, 5-bit gray.
// Framebuffer packs 3 pixels into 2 bytes -> 170 bytes per row.
// ---------------------------------------------------------------------------
#define MK1_DISPLAY_COUNT       2
#define MK1_DISPLAY_WIDTH       255
#define MK1_DISPLAY_HEIGHT      64
#define MK1_DISPLAY_ROW_BYTES   170
#define MK1_DISPLAY_FB_BYTES    (MK1_DISPLAY_ROW_BYTES * MK1_DISPLAY_HEIGHT)  // 10880
#define MK1_DISPLAY_MAX_GRAY    0x1f
#define MK1_EP8_MAX_PAYLOAD     508
#define MK1_ST7529_RAMWR        0x5c

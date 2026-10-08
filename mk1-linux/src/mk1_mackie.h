// Mackie Control Universal (MCU) emulation.
//
// Turns MK1 buttons/knobs into MCU messages and keeps the state the DAW sends
// back (LCD text, LEDs, V-Pot rings, fader positions, meters) so it can be
// drawn on the MK1 displays. Pure logic: MIDI goes in and out as raw bytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mk1_display.h"
#include "mk1_proto.h"

#define MK1_MCU_STRIPS     8
#define MK1_MCU_LCD_COLS   56
#define MK1_MCU_FADERS     9          // 8 strips + master
#define MK1_MCU_MASTER     8
#define MK1_MCU_METER_MAX  12

// Bits returned by the update functions.
#define MK1_MCU_CHANGED_DISPLAY 0x1u
#define MK1_MCU_CHANGED_LEDS    0x2u

typedef void (*mk1_mcu_send_fn)(void *ctx, const uint8_t *bytes, size_t len);

typedef struct {
    mk1_mcu_send_fn send;
    void           *ctx;

    // state set by the DAW
    char     lcd[2][MK1_MCU_LCD_COLS];
    uint8_t  note[128];                    // LED notes: 0 off, 1 blink, 127 on
    uint8_t  vpot_ring[MK1_MCU_STRIPS];
    uint16_t fader[MK1_MCU_FADERS];        // 14-bit
    uint8_t  meter[MK1_MCU_STRIPS];        // 0..12
    bool     overload[MK1_MCU_STRIPS];
    char     assignment[2];                // two-digit assignment display
    uint64_t meter_decay_ns;

    // local state
    bool     shift, mute_held, solo_held, rec_held, rec_combined;
    uint8_t  pressed_note[MK1_BTN_COUNT];  // note sent on press, 0xff = none
    bool     touched[MK1_MCU_FADERS];
    uint64_t touch_until_ns[MK1_MCU_FADERS];
} mk1_mcu_t;

void mk1_mcu_init(mk1_mcu_t *m, mk1_mcu_send_fn send, void *ctx);

// Returns true if the button belongs to the MCU layer (then nothing else
// should be sent for it). Buttons without an MCU function return false.
bool mk1_mcu_button(mk1_mcu_t *m, mk1_button_t button, bool pressed);

// steps > 0 = clockwise. Returns MK1_MCU_CHANGED_* bits.
unsigned mk1_mcu_encoder(mk1_mcu_t *m, mk1_encoder_t encoder, int steps, uint64_t now_ns);

// One complete MIDI message from the DAW (SysEx including F0..F7).
unsigned mk1_mcu_host_message(mk1_mcu_t *m, const uint8_t *msg, size_t len);

// Meter decay and fader touch release; call regularly.
unsigned mk1_mcu_tick(mk1_mcu_t *m, uint64_t now_ns);

// LED level (0..127) for an MK1 button in MCU mode, or -1 if the button's LED
// is not driven by MCU state.
int mk1_mcu_button_led(const mk1_mcu_t *m, mk1_button_t button);

// Draws strips 1-4 (display 0) or 5-8 (display 1).
void mk1_mcu_render(const mk1_mcu_t *m, unsigned display, mk1_canvas_t *c);

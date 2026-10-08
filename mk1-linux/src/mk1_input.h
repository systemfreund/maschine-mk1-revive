// Platform-independent decoding of MK1 input reports (pads, buttons, encoders).
//
// Ported from mk1-usb/mk1_device.c and the pad state machine in mk1-bridge/main.c.
// Contains no USB or MIDI code so it can be unit-tested with synthetic reports.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mk1_proto.h"

typedef struct {
    int      pad_hit_on;        // pressure (0-4095) that starts a hit
    int      pad_hit_off;       // pressure below which an active pad is released
    int      pad_pressure_step; // minimum change before reporting held-pad pressure
    uint64_t pad_debounce_ns;   // quiet time after release before the next hit
    int      pad_sustain;       // consecutive reports above hit_on before firing
    int      encoder_min_delta; // raw encoder counts below this are jitter
    bool     len33_buttons;     // decode Auto Write / Sampling from the encoder report
} mk1_input_config_t;

typedef struct {
    // Pad struck. peak = highest pressure seen while the hit was being confirmed.
    void (*pad_hit)(void *ctx, unsigned pad, int peak);
    // Held pad pressure changed by at least pad_pressure_step.
    void (*pad_pressure)(void *ctx, unsigned pad, int pressure);
    void (*pad_release)(void *ctx, unsigned pad);
    // Every pressure report after baseline (for LED feedback); may be NULL.
    void (*pad_levels)(void *ctx, const uint16_t pressure[MK1_PAD_COUNT]);
    void (*button)(void *ctx, mk1_button_t button, bool pressed);
    // Signed raw encoder delta, positive = clockwise.
    void (*encoder)(void *ctx, mk1_encoder_t encoder, int delta);
} mk1_input_callbacks_t;

typedef struct {
    mk1_input_config_t    cfg;
    mk1_input_callbacks_t cb;
    void                 *ctx;

    // pads
    bool     pad_baseline_set;
    uint16_t pad_baseline[MK1_PAD_COUNT];
    uint16_t pad_prev[MK1_PAD_COUNT];
    uint16_t pad_sent[MK1_PAD_COUNT];      // >0 while a hit is active
    uint16_t pad_peak[MK1_PAD_COUNT];
    int      pad_above[MK1_PAD_COUNT];
    uint64_t pad_release_ns[MK1_PAD_COUNT];

    // buttons
    bool    buttons_valid;
    uint8_t buttons_report[8];
    bool    button_state[MK1_BTN_COUNT];

    // encoders
    bool    enc_prev_valid;
    uint8_t enc_prev[33];
    bool    len33_auto_write;
    bool    len33_sampling;
} mk1_input_t;

void mk1_input_default_config(mk1_input_config_t *cfg);
void mk1_input_init(mk1_input_t *in, const mk1_input_config_t *cfg,
                    const mk1_input_callbacks_t *cb, void *ctx);

// Forget all device state (call after reconnect). Releases held pads/buttons
// through the callbacks so no notes or CCs are left hanging.
void mk1_input_reset(mk1_input_t *in);

void mk1_input_feed_pads(mk1_input_t *in, const uint8_t *data, size_t len, uint64_t now_ns);
void mk1_input_feed_ep1(mk1_input_t *in, const uint8_t *data, size_t len);

const char *mk1_button_name(mk1_button_t button);
const char *mk1_encoder_name(mk1_encoder_t encoder);

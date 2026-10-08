#include "mk1_input.h"

#include <string.h>

static const char *const k_button_names[MK1_BTN_COUNT] = {
    [MK1_BTN_MUTE] = "Mute",           [MK1_BTN_SOLO] = "Solo",
    [MK1_BTN_SELECT] = "Select",       [MK1_BTN_DUPLICATE] = "Duplicate",
    [MK1_BTN_NAVIGATE] = "Navigate",   [MK1_BTN_PAD_MODE] = "Pad Mode",
    [MK1_BTN_PATTERN] = "Pattern",     [MK1_BTN_SCENE] = "Scene",
    [MK1_BTN_RESTART] = "Restart",     [MK1_BTN_TRANSPORT_LEFT] = "Transport <",
    [MK1_BTN_TRANSPORT_RIGHT] = "Transport >", [MK1_BTN_GRID] = "Grid",
    [MK1_BTN_PLAY] = "Play",           [MK1_BTN_REC] = "Rec",
    [MK1_BTN_ERASE] = "Erase",         [MK1_BTN_SHIFT] = "Shift",
    [MK1_BTN_GROUP_A] = "Group A",     [MK1_BTN_GROUP_B] = "Group B",
    [MK1_BTN_GROUP_C] = "Group C",     [MK1_BTN_GROUP_D] = "Group D",
    [MK1_BTN_GROUP_E] = "Group E",     [MK1_BTN_GROUP_F] = "Group F",
    [MK1_BTN_GROUP_G] = "Group G",     [MK1_BTN_GROUP_H] = "Group H",
    [MK1_BTN_CONTROL] = "Control",     [MK1_BTN_STEP] = "Step",
    [MK1_BTN_BROWSE] = "Browse",       [MK1_BTN_SAMPLING] = "Sampling",
    [MK1_BTN_LEFT] = "Left",           [MK1_BTN_RIGHT] = "Right",
    [MK1_BTN_SNAP] = "Snap",           [MK1_BTN_AUTO_WRITE] = "Auto Write",
    [MK1_BTN_SCREEN1] = "Screen 1",    [MK1_BTN_SCREEN2] = "Screen 2",
    [MK1_BTN_SCREEN3] = "Screen 3",    [MK1_BTN_SCREEN4] = "Screen 4",
    [MK1_BTN_SCREEN5] = "Screen 5",    [MK1_BTN_SCREEN6] = "Screen 6",
    [MK1_BTN_SCREEN7] = "Screen 7",    [MK1_BTN_SCREEN8] = "Screen 8",
    [MK1_BTN_NOTE_REPEAT] = "Note Repeat",
};

static const char *const k_encoder_names[MK1_ENC_COUNT] = {
    [MK1_ENC_VOLUME] = "Volume", [MK1_ENC_TEMPO] = "Tempo", [MK1_ENC_SWING] = "Swing",
    [MK1_ENC_SCREEN1] = "Knob 1", [MK1_ENC_SCREEN2] = "Knob 2",
    [MK1_ENC_SCREEN3] = "Knob 3", [MK1_ENC_SCREEN4] = "Knob 4",
    [MK1_ENC_SCREEN5] = "Knob 5", [MK1_ENC_SCREEN6] = "Knob 6",
    [MK1_ENC_SCREEN7] = "Knob 7", [MK1_ENC_SCREEN8] = "Knob 8",
};

const char *mk1_button_name(mk1_button_t button)
{
    return (button < MK1_BTN_COUNT) ? k_button_names[button] : "?";
}

const char *mk1_encoder_name(mk1_encoder_t encoder)
{
    return (encoder < MK1_ENC_COUNT) ? k_encoder_names[encoder] : "?";
}

void mk1_input_default_config(mk1_input_config_t *cfg)
{
    // Defaults match the macOS bridge (MK1_PAD_* / MK1_ENCODER_MIN_DELTA).
    cfg->pad_hit_on        = 300;
    cfg->pad_hit_off       = 150;
    cfg->pad_pressure_step = 200;
    cfg->pad_debounce_ns   = 10000000ULL;
    cfg->pad_sustain       = 3;
    cfg->encoder_min_delta = 2;
    cfg->len33_buttons     = true;
}

void mk1_input_init(mk1_input_t *in, const mk1_input_config_t *cfg,
                    const mk1_input_callbacks_t *cb, void *ctx)
{
    memset(in, 0, sizeof(*in));
    in->cfg = *cfg;
    in->cb  = *cb;
    in->ctx = ctx;
}

static void set_button(mk1_input_t *in, mk1_button_t button, bool pressed)
{
    if (button >= MK1_BTN_COUNT || in->button_state[button] == pressed) {
        return;
    }
    in->button_state[button] = pressed;
    if (in->cb.button) {
        in->cb.button(in->ctx, button, pressed);
    }
}

void mk1_input_reset(mk1_input_t *in)
{
    for (unsigned i = 0; i < MK1_PAD_COUNT; i++) {
        if (in->pad_sent[i] && in->cb.pad_release) {
            in->cb.pad_release(in->ctx, i);
        }
    }
    for (unsigned b = 0; b < MK1_BTN_COUNT; b++) {
        set_button(in, (mk1_button_t)b, false);
    }

    mk1_input_config_t cfg = in->cfg;
    mk1_input_callbacks_t cb = in->cb;
    void *ctx = in->ctx;
    mk1_input_init(in, &cfg, &cb, ctx);
}

// ---------------------------------------------------------------------------
// Pads (EP4)
// ---------------------------------------------------------------------------

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

// The EP4 stream interleaves real pressure reports with a "scan table": a ring
// of 16 words whose top nibble steps by 1 per slot, duplicated verbatim in
// both channels. Each word's top nibble is the pad id, the low 12 bits the value.
static bool is_scan_table(const uint8_t *data)
{
    for (unsigned i = 0; i < MK1_PAD_COUNT; i++) {
        uint16_t cur = read_le16(data + i * 2);
        uint16_t dup = read_le16(data + 32 + i * 2);
        uint16_t expected = (uint16_t)((read_le16(data) + i * 0x1000) & 0xf000);
        if ((cur & 0xf000) != expected) return false;
        if ((cur & 0x0fff) != (dup & 0x0fff)) return false;
    }
    return true;
}

static void process_pad(mk1_input_t *in, unsigned i, uint16_t pressure,
                        uint64_t now_ns, bool releases_only)
{
    const mk1_input_config_t *cfg = &in->cfg;
    bool was_active = in->pad_sent[i] > 0;
    bool is_active = pressure >= (was_active ? cfg->pad_hit_off : cfg->pad_hit_on);

    if (pressure == in->pad_prev[i] && was_active == is_active) {
        return;
    }
    if (releases_only && !(was_active && !is_active)) {
        return;
    }

    if (!was_active && !is_active) {
        in->pad_above[i] = 0;
        in->pad_peak[i] = 0;
    } else if (!was_active && is_active) {
        if (now_ns - in->pad_release_ns[i] < cfg->pad_debounce_ns) {
            in->pad_above[i] = 0;
            in->pad_peak[i] = 0;
            in->pad_prev[i] = pressure;
            return;
        }
        // Sustain gate: single-report spikes are mechanical bounce, not hits.
        if (pressure > in->pad_peak[i]) in->pad_peak[i] = pressure;
        if (++in->pad_above[i] < cfg->pad_sustain) {
            in->pad_prev[i] = pressure;
            return;
        }
        in->pad_sent[i] = pressure ? pressure : 1;
        if (in->cb.pad_hit) in->cb.pad_hit(in->ctx, i, in->pad_peak[i]);
    } else if (was_active && !is_active) {
        in->pad_sent[i] = 0;
        in->pad_above[i] = 0;
        in->pad_peak[i] = 0;
        in->pad_release_ns[i] = now_ns;
        if (in->cb.pad_release) in->cb.pad_release(in->ctx, i);
    } else {
        int diff = (int)pressure - (int)in->pad_sent[i];
        if (diff < 0) diff = -diff;
        if (diff >= cfg->pad_pressure_step) {
            in->pad_sent[i] = pressure ? pressure : 1;
            if (in->cb.pad_pressure) in->cb.pad_pressure(in->ctx, i, pressure);
        }
    }
    in->pad_prev[i] = pressure;
}

static void process_pad_report(mk1_input_t *in, const uint8_t *data, uint64_t now_ns)
{
    uint8_t phase = (uint8_t)(read_le16(data) >> 12);
    bool scan = is_scan_table(data);

    // Pressure reports always have phase 0 (pad 0 lives in word 0). Anything
    // else is a rotated scan table and must not be read as pressure.
    if (phase != 0) {
        return;
    }

    uint16_t pressure[MK1_PAD_COUNT];
    for (unsigned i = 0; i < MK1_PAD_COUNT; i++) {
        uint16_t value = read_le16(data + i * 2) & 0x0fff;
        if (!in->pad_baseline_set) {
            in->pad_baseline[i] = value;
            continue;
        }
        int delta = (int)value - (int)in->pad_baseline[i];
        pressure[i] = (uint16_t)(delta < 0 ? 0 : delta);
    }
    if (!in->pad_baseline_set) {
        in->pad_baseline_set = true;
        return;   // first report is calibration only
    }

    // A phase-0 scan table may encode non-pressure data. The macOS bridge drops
    // these entirely; we still let them release held pads so a scan frame
    // arriving at the wrong moment cannot leave a note hanging.
    for (unsigned i = 0; i < MK1_PAD_COUNT; i++) {
        process_pad(in, i, pressure[i], now_ns, scan);
    }
    if (!scan && in->cb.pad_levels) {
        in->cb.pad_levels(in->ctx, pressure);
    }
}

void mk1_input_feed_pads(mk1_input_t *in, const uint8_t *data, size_t len, uint64_t now_ns)
{
    // A bulk read can carry several back-to-back 64-byte reports.
    while (len >= MK1_PAD_REPORT_LEN) {
        process_pad_report(in, data, now_ns);
        data += MK1_PAD_REPORT_LEN;
        len  -= MK1_PAD_REPORT_LEN;
    }
}

// ---------------------------------------------------------------------------
// Buttons (EP1, 8-byte report starting with 0x04)
// ---------------------------------------------------------------------------

static const struct {
    uint8_t      byte;
    uint8_t      bit;
    mk1_button_t button;
} k_button_bits[] = {
    { 1, 0x01, MK1_BTN_MUTE },      { 1, 0x02, MK1_BTN_SOLO },
    { 1, 0x04, MK1_BTN_SELECT },    { 1, 0x08, MK1_BTN_DUPLICATE },
    { 1, 0x10, MK1_BTN_NAVIGATE },  { 1, 0x20, MK1_BTN_PAD_MODE },
    { 1, 0x40, MK1_BTN_PATTERN },   { 1, 0x80, MK1_BTN_SCENE },

    { 2, 0x02, MK1_BTN_REC },       { 2, 0x04, MK1_BTN_ERASE },
    { 2, 0x08, MK1_BTN_SHIFT },     { 2, 0x10, MK1_BTN_GRID },
    { 2, 0x20, MK1_BTN_TRANSPORT_RIGHT }, { 2, 0x40, MK1_BTN_TRANSPORT_LEFT },
    { 2, 0x80, MK1_BTN_RESTART },

    { 3, 0x01, MK1_BTN_GROUP_E },   { 3, 0x02, MK1_BTN_GROUP_F },
    { 3, 0x04, MK1_BTN_GROUP_G },   { 3, 0x08, MK1_BTN_GROUP_H },
    { 3, 0x10, MK1_BTN_GROUP_D },   { 3, 0x20, MK1_BTN_GROUP_C },
    { 3, 0x40, MK1_BTN_GROUP_B },   { 3, 0x80, MK1_BTN_GROUP_A },

    { 4, 0x01, MK1_BTN_CONTROL },   { 4, 0x02, MK1_BTN_BROWSE },
    { 4, 0x04, MK1_BTN_LEFT },      { 4, 0x08, MK1_BTN_SNAP },
    { 4, 0x10, MK1_BTN_AUTO_WRITE },{ 4, 0x20, MK1_BTN_RIGHT },
    { 4, 0x40, MK1_BTN_SAMPLING },  { 4, 0x80, MK1_BTN_STEP },

    { 5, 0x80, MK1_BTN_SCREEN1 },   { 5, 0x40, MK1_BTN_SCREEN2 },
    { 5, 0x20, MK1_BTN_SCREEN3 },   { 5, 0x10, MK1_BTN_SCREEN4 },
    { 5, 0x08, MK1_BTN_SCREEN5 },   { 5, 0x04, MK1_BTN_SCREEN6 },
    { 5, 0x02, MK1_BTN_SCREEN7 },   { 5, 0x01, MK1_BTN_SCREEN8 },

    { 6, 0x01, MK1_BTN_NOTE_REPEAT },
    { 6, 0x02, MK1_BTN_PLAY },
};

static void process_buttons(mk1_input_t *in, const uint8_t *report)
{
    if (!in->buttons_valid) {
        // First report is the resting state; nothing to diff against.
        in->buttons_valid = true;
        memcpy(in->buttons_report, report, sizeof(in->buttons_report));
        return;
    }
    for (size_t i = 0; i < sizeof(k_button_bits) / sizeof(k_button_bits[0]); i++) {
        uint8_t changed = in->buttons_report[k_button_bits[i].byte] ^ report[k_button_bits[i].byte];
        if (changed & k_button_bits[i].bit) {
            set_button(in, k_button_bits[i].button,
                       (report[k_button_bits[i].byte] & k_button_bits[i].bit) != 0);
        }
    }
    memcpy(in->buttons_report, report, sizeof(in->buttons_report));
}

// ---------------------------------------------------------------------------
// Encoders (EP1, 33-byte report starting with 0x02)
// ---------------------------------------------------------------------------

static const struct {
    uint8_t       byte_a;
    uint8_t       byte_b;
    mk1_encoder_t encoder;
} k_encoder_bytes[] = {
    { 17, 18, MK1_ENC_VOLUME },
    { 11, 12, MK1_ENC_TEMPO },
    {  5,  6, MK1_ENC_SWING },
    { 21, 22, MK1_ENC_SCREEN1 },
    { 15, 16, MK1_ENC_SCREEN2 },
    {  9, 10, MK1_ENC_SCREEN3 },
    {  3,  4, MK1_ENC_SCREEN4 },
    { 19, 20, MK1_ENC_SCREEN5 },
    { 13, 14, MK1_ENC_SCREEN6 },
    {  7,  8, MK1_ENC_SCREEN7 },
    {  1,  2, MK1_ENC_SCREEN8 },
};
#define ENCODER_MAP_LEN (sizeof(k_encoder_bytes) / sizeof(k_encoder_bytes[0]))

static int wrapped_delta(uint8_t prev, uint8_t cur)
{
    int d = (int)cur - (int)prev;
    if (d > 127) d -= 256;
    if (d < -127) d += 256;
    return d;
}

// Quadrature direction decoder (same logic as CABL MaschineMK1::processEncoders).
static bool encoder_increased(uint8_t x, uint8_t y, uint8_t px, uint8_t py)
{
    if (x > 127) {
        return (y > 127) ? (x < px && y >= py) : (x >= px && y >= py);
    }
    return (y > 127) ? (x < px && y < py) : (x >= px && y < py);
}

static void process_encoders(mk1_input_t *in, const uint8_t *data)
{
    const uint8_t *prev = in->enc_prev;

    // byte[4] is both Knob 4's second byte and the Auto Write / Sampling
    // discriminator, so button decoding is suppressed while Knob 4 moves.
    uint8_t b4_hi = data[4] & 0xf0;
    bool b4_button = (b4_hi == 0x60 || b4_hi == 0x70);
    bool knob4_moving = in->enc_prev_valid &&
                        (wrapped_delta(prev[3], data[3]) != 0 ||
                         wrapped_delta(prev[4], data[4]) != 0);

    if (in->enc_prev_valid) {
        int  magnitude[ENCODER_MAP_LEN] = {0};
        bool increased[ENCODER_MAP_LEN] = {0};
        int  dominant = 0;

        for (size_t i = 0; i < ENCODER_MAP_LEN; i++) {
            uint8_t a = k_encoder_bytes[i].byte_a, b = k_encoder_bytes[i].byte_b;
            if (b == 4 && b4_button && !knob4_moving) continue;
            int da = wrapped_delta(prev[a], data[a]);
            int db = wrapped_delta(prev[b], data[b]);
            if (da < 0) da = -da;
            if (db < 0) db = -db;
            magnitude[i] = da > db ? da : db;
            increased[i] = encoder_increased(data[a], data[b], prev[a], prev[b]);
            if (magnitude[i] > dominant) dominant = magnitude[i];
        }

        for (size_t i = 0; i < ENCODER_MAP_LEN; i++) {
            int m = magnitude[i];
            if (m == 0 || m < in->cfg.encoder_min_delta) continue;   // jitter
            if (dominant >= 2 * m) continue;                         // crosstalk
            if (in->cb.encoder) {
                in->cb.encoder(in->ctx, k_encoder_bytes[i].encoder, increased[i] ? m : -m);
            }
        }
    }

    memcpy(in->enc_prev, data, sizeof(in->enc_prev));
    in->enc_prev_valid = true;

    if (!in->cfg.len33_buttons || knob4_moving) {
        return;
    }
    // Observed traces: 0x7x = Auto Write held, 0x6x = Sampling held, 0x5x = released.
    // Only release what this path pressed, so it never cancels a press that
    // arrived through the regular button report.
    if (b4_hi == 0x70 && !in->len33_auto_write) {
        if (in->len33_sampling) {
            in->len33_sampling = false;
            set_button(in, MK1_BTN_SAMPLING, false);
        }
        in->len33_auto_write = true;
        set_button(in, MK1_BTN_AUTO_WRITE, true);
    } else if (b4_hi == 0x60 && !in->len33_sampling) {
        if (in->len33_auto_write) {
            in->len33_auto_write = false;
            set_button(in, MK1_BTN_AUTO_WRITE, false);
        }
        in->len33_sampling = true;
        set_button(in, MK1_BTN_SAMPLING, true);
    } else if (b4_hi == 0x50) {
        if (in->len33_auto_write) {
            in->len33_auto_write = false;
            set_button(in, MK1_BTN_AUTO_WRITE, false);
        }
        if (in->len33_sampling) {
            in->len33_sampling = false;
            set_button(in, MK1_BTN_SAMPLING, false);
        }
    }
}

void mk1_input_feed_ep1(mk1_input_t *in, const uint8_t *data, size_t len)
{
    if (len == 8 && data[0] == MK1_REPORT_BUTTONS) {
        process_buttons(in, data);
    } else if (len == 33 && data[0] == MK1_REPORT_ENCODERS) {
        process_encoders(in, data);
    }
}

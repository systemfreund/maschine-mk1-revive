// mk1-linux: userspace driver that turns a Maschine MK1 into a class-style
// MIDI controller on Linux (ALSA sequencer), with LED and display feedback.
//
//   [DAW] <--ALSA seq--> [mk1-linux] <--libusb--> [MK1]
//
// Threads:
//   - libusb event thread: input transfers complete here -> mk1_input -> MIDI out
//   - main thread: MIDI in, LED/display flushing, hotplug (reopen on unplug)

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "mk1_display.h"
#include "mk1_input.h"
#include "mk1_leds.h"
#include "mk1_log.h"
#include "mk1_mackie.h"
#include "mk1_midi.h"
#include "mk1_proto.h"
#include "mk1_usb.h"

bool mk1_verbose = false;

// ---------------------------------------------------------------------------
// MIDI mapping
// ---------------------------------------------------------------------------

static const uint8_t k_button_cc[MK1_BTN_COUNT] = {
    [MK1_BTN_MUTE] = 20,      [MK1_BTN_SOLO] = 21,
    [MK1_BTN_SELECT] = 22,    [MK1_BTN_DUPLICATE] = 23,
    [MK1_BTN_NAVIGATE] = 24,  [MK1_BTN_PAD_MODE] = 25,
    [MK1_BTN_PATTERN] = 26,   [MK1_BTN_SCENE] = 27,

    [MK1_BTN_CONTROL] = 28,   [MK1_BTN_STEP] = 29,
    [MK1_BTN_BROWSE] = 30,    [MK1_BTN_SAMPLING] = 31,
    [MK1_BTN_LEFT] = 52,      [MK1_BTN_RIGHT] = 53,
    [MK1_BTN_SNAP] = 54,      [MK1_BTN_AUTO_WRITE] = 55,

    [MK1_BTN_SCREEN1] = 88,   [MK1_BTN_SCREEN2] = 89,
    [MK1_BTN_SCREEN3] = 90,   [MK1_BTN_SCREEN4] = 91,
    [MK1_BTN_SCREEN5] = 92,   [MK1_BTN_SCREEN6] = 93,
    [MK1_BTN_SCREEN7] = 94,   [MK1_BTN_SCREEN8] = 95,

    [MK1_BTN_RESTART] = 102,  [MK1_BTN_TRANSPORT_LEFT] = 103,
    [MK1_BTN_TRANSPORT_RIGHT] = 104, [MK1_BTN_GRID] = 105,
    [MK1_BTN_PLAY] = 106,     [MK1_BTN_REC] = 107,
    [MK1_BTN_ERASE] = 108,    [MK1_BTN_SHIFT] = 109,

    [MK1_BTN_GROUP_A] = 110,  [MK1_BTN_GROUP_B] = 111,
    [MK1_BTN_GROUP_C] = 112,  [MK1_BTN_GROUP_D] = 113,
    [MK1_BTN_GROUP_E] = 114,  [MK1_BTN_GROUP_F] = 115,
    [MK1_BTN_GROUP_G] = 116,  [MK1_BTN_GROUP_H] = 117,

    [MK1_BTN_NOTE_REPEAT] = 118,
};

static const uint8_t k_encoder_cc[MK1_ENC_COUNT] = {
    [MK1_ENC_SCREEN1] = 70, [MK1_ENC_SCREEN2] = 71, [MK1_ENC_SCREEN3] = 72,
    [MK1_ENC_SCREEN4] = 73, [MK1_ENC_SCREEN5] = 74, [MK1_ENC_SCREEN6] = 75,
    [MK1_ENC_SCREEN7] = 76, [MK1_ENC_SCREEN8] = 77,
    [MK1_ENC_VOLUME] = 85,  [MK1_ENC_TEMPO] = 86,   [MK1_ENC_SWING] = 87,
};

// SysEx for display text: F0 7D 4D 4B 31 <cmd> ... F7 (0x7D = non-commercial id)
static const uint8_t k_sysex_header[] = { 0xf0, 0x7d, 'M', 'K', '1' };
#define SYSEX_SET_LINE   0x01   // <display 0|1> <line 0..7> <ascii...>
#define SYSEX_CLEAR_TEXT 0x02   // <display 0|1>

#define TEXT_LINES 8
#define TEXT_COLS  42

// ---------------------------------------------------------------------------
// Configuration (environment variables, see README)
// ---------------------------------------------------------------------------

typedef struct {
    int    channel;            // 0-based
    int    pad_base_note;
    bool   aftertouch;
    int    velocity_max;
    double velocity_curve;
    int    fixed_velocity;     // 0 = dynamic
    bool   encoder_relative;
    int    encoder_divisor;
    bool   local_leds;
    int    backlight;
    bool   mackie;             // buttons/knobs drive the MK1 Mackie port
    int    display_fps;
    mk1_input_config_t input;
} config_t;

static int env_int(const char *name, int fallback, int lo, int hi)
{
    const char *s = getenv(name);
    char *end = NULL;
    if (!s || !*s) return fallback;
    long v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi) {
        MK1_LOG("ignoring %s=%s (expected %d..%d)", name, s, lo, hi);
        return fallback;
    }
    return (int)v;
}

static double env_double(const char *name, double fallback, double lo, double hi)
{
    const char *s = getenv(name);
    char *end = NULL;
    if (!s || !*s) return fallback;
    double v = strtod(s, &end);
    if (*end || v < lo || v > hi) {
        MK1_LOG("ignoring %s=%s (expected %g..%g)", name, s, lo, hi);
        return fallback;
    }
    return v;
}

static void load_config(config_t *cfg)
{
    const char *mode = getenv("MK1_ENCODER_MODE");
    const char *layer = getenv("MK1_MODE");

    cfg->channel         = env_int("MK1_MIDI_CHANNEL", 1, 1, 16) - 1;
    cfg->pad_base_note   = env_int("MK1_PAD_BASE_NOTE", 36, 0, 127 - 15);
    cfg->aftertouch      = env_int("MK1_AFTERTOUCH", 1, 0, 1) != 0;
    cfg->velocity_max    = env_int("MK1_VELOCITY_MAX", 3200, 400, MK1_PAD_MAX);
    cfg->velocity_curve  = env_double("MK1_VELOCITY_CURVE", 0.7, 0.1, 5.0);
    cfg->fixed_velocity  = env_int("MK1_FIXED_VELOCITY", 0, 0, 127);
    cfg->encoder_relative = mode && strcasecmp(mode, "relative") == 0;
    cfg->encoder_divisor = env_int("MK1_ENCODER_DIVISOR", 2, 1, 64);
    cfg->local_leds      = env_int("MK1_LOCAL_LEDS", 1, 0, 1) != 0;
    cfg->backlight       = env_int("MK1_BACKLIGHT", MK1_LED_BACKLIGHT_ON, 0, 127);
    cfg->mackie          = layer && strcasecmp(layer, "mackie") == 0;
    cfg->display_fps     = env_int("MK1_DISPLAY_FPS", 20, 1, 60);
    if (layer && !cfg->mackie && strcasecmp(layer, "midi") != 0) {
        MK1_LOG("ignoring MK1_MODE=%s (expected midi or mackie)", layer);
    }

    mk1_input_default_config(&cfg->input);
    cfg->input.pad_hit_on        = env_int("MK1_PAD_HIT_ON", cfg->input.pad_hit_on, 1, MK1_PAD_MAX);
    cfg->input.pad_hit_off       = env_int("MK1_PAD_HIT_OFF", cfg->input.pad_hit_off, 0, MK1_PAD_MAX);
    cfg->input.pad_pressure_step = env_int("MK1_PAD_PRESSURE", cfg->input.pad_pressure_step, 1, MK1_PAD_MAX);
    cfg->input.pad_debounce_ns   = (uint64_t)env_int("MK1_PAD_DEBOUNCE_MS", 10, 0, 1000) * 1000000ULL;
    cfg->input.pad_sustain       = env_int("MK1_PAD_SUSTAIN", cfg->input.pad_sustain, 1, 50);
    cfg->input.encoder_min_delta = env_int("MK1_ENCODER_MIN_DELTA", cfg->input.encoder_min_delta, 1, 64);

    if (cfg->input.pad_hit_off > cfg->input.pad_hit_on) {
        cfg->input.pad_hit_off = cfg->input.pad_hit_on;
    }
    if (cfg->velocity_max <= cfg->input.pad_hit_on) {
        cfg->velocity_max = cfg->input.pad_hit_on + 1;
    }
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

typedef struct {
    config_t         cfg;
    mk1_midi_t      *midi;
    libusb_context  *usb_ctx;
    mk1_usb_t       *usb;
    mk1_input_t      input;

    pthread_mutex_t  lock;          // guards everything below
    mk1_mcu_t        mcu;
    uint8_t          led_host[MK1_LED_COUNT];    // set by MIDI feedback
    uint8_t          led_local[MK1_LED_COUNT];   // pressed buttons / pads
    bool             leds_dirty;

    int              enc_value[MK1_ENC_COUNT];   // 0..127 virtual position
    int              enc_accum[MK1_ENC_COUNT];   // raw counts not yet sent
    char             status[TEXT_COLS + 1];      // last event, top line of right display
    char             host_text[MK1_DISPLAY_COUNT][TEXT_LINES][TEXT_COLS + 1];
    bool             host_text_active[MK1_DISPLAY_COUNT];
    bool             display_dirty[MK1_DISPLAY_COUNT];
} app_t;

static atomic_bool g_running = true;
static atomic_bool g_usb_events = true;   // outlives g_running so close can finish

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_running, false);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void set_status(app_t *app, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(app_t *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
    app->display_dirty[1] = true;
}

static void set_local_led(app_t *app, uint8_t slot, uint8_t value)
{
    if (!app->cfg.local_leds || slot >= MK1_LED_COUNT) return;
    if (app->led_local[slot] != value) {
        app->led_local[slot] = value;
        app->leds_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Input callbacks (libusb event thread)
// ---------------------------------------------------------------------------

static int pad_velocity(const config_t *cfg, int peak)
{
    if (cfg->fixed_velocity) return cfg->fixed_velocity;
    double span = (double)(cfg->velocity_max - cfg->input.pad_hit_on);
    double x = (double)(peak - cfg->input.pad_hit_on) / span;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    int v = 1 + (int)lround(126.0 * pow(x, cfg->velocity_curve));
    return v > 127 ? 127 : v;
}

static void cb_pad_hit(void *ctx, unsigned pad, int peak)
{
    app_t *app = ctx;
    int note = app->cfg.pad_base_note + (int)pad;
    int vel = pad_velocity(&app->cfg, peak);

    mk1_midi_note(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel, note, vel);
    MK1_DEBUG("pad %u hit peak=%d -> note %d vel %d", pad + 1, peak, note, vel);

    pthread_mutex_lock(&app->lock);
    set_local_led(app, mk1_pad_led_slot(pad), MK1_LED_BRIGHT);
    set_status(app, "Pad %u  note %d  vel %d", pad + 1, note, vel);
    pthread_mutex_unlock(&app->lock);
}

static void cb_pad_pressure(void *ctx, unsigned pad, int pressure)
{
    app_t *app = ctx;
    if (!app->cfg.aftertouch) return;
    int value = pressure * 127 / app->cfg.velocity_max;
    if (value > 127) value = 127;
    mk1_midi_poly_aftertouch(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel,
                             app->cfg.pad_base_note + (int)pad, value);
}

static void cb_pad_release(void *ctx, unsigned pad)
{
    app_t *app = ctx;
    mk1_midi_note(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel, app->cfg.pad_base_note + (int)pad, 0);

    pthread_mutex_lock(&app->lock);
    set_local_led(app, mk1_pad_led_slot(pad), 0);
    pthread_mutex_unlock(&app->lock);
}

static void mcu_send(void *ctx, const uint8_t *bytes, size_t len)
{
    app_t *app = ctx;
    mk1_midi_raw(app->midi, MK1_PORT_MACKIE, bytes, len);
}

static void cb_button(void *ctx, mk1_button_t button, bool pressed)
{
    app_t *app = ctx;

    if (app->cfg.mackie) {
        pthread_mutex_lock(&app->lock);
        bool handled = mk1_mcu_button(&app->mcu, button, pressed);
        if (handled) {
            set_local_led(app, mk1_button_led_slot(button), pressed ? MK1_LED_BRIGHT : 0);
            app->leds_dirty = true;   // screen LEDs follow the held modifier
            app->display_dirty[0] = app->display_dirty[1] = true;
            if (pressed) set_status(app, "%s (Mackie)", mk1_button_name(button));
        }
        pthread_mutex_unlock(&app->lock);
        MK1_DEBUG("button %s %s -> %s", mk1_button_name(button), pressed ? "down" : "up",
                  handled ? "Mackie" : "CC");
        if (handled) return;
    }

    mk1_midi_cc(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel, k_button_cc[button], pressed ? 127 : 0);
    MK1_DEBUG("button %s %s -> CC %d", mk1_button_name(button), pressed ? "down" : "up", k_button_cc[button]);

    pthread_mutex_lock(&app->lock);
    set_local_led(app, mk1_button_led_slot(button), pressed ? MK1_LED_BRIGHT : 0);
    if (pressed) set_status(app, "%s  CC %d", mk1_button_name(button), k_button_cc[button]);
    pthread_mutex_unlock(&app->lock);
}

static void cb_encoder(void *ctx, mk1_encoder_t enc, int delta)
{
    app_t *app = ctx;
    int steps, value, cc = k_encoder_cc[enc];

    pthread_mutex_lock(&app->lock);
    app->enc_accum[enc] += delta;
    steps = app->enc_accum[enc] / app->cfg.encoder_divisor;
    app->enc_accum[enc] -= steps * app->cfg.encoder_divisor;
    if (steps == 0) {
        pthread_mutex_unlock(&app->lock);
        return;
    }
    if (app->cfg.mackie) {
        unsigned changed = mk1_mcu_encoder(&app->mcu, enc, steps, now_ns());
        if (changed & MK1_MCU_CHANGED_DISPLAY) {
            app->display_dirty[0] = app->display_dirty[1] = true;
        }
        pthread_mutex_unlock(&app->lock);
        return;
    }
    value = app->enc_value[enc] + steps;
    value = value < 0 ? 0 : (value > 127 ? 127 : value);
    app->enc_value[enc] = value;
    if (enc >= MK1_ENC_SCREEN1) {
        app->display_dirty[(enc - MK1_ENC_SCREEN1) / 4] = true;
    }
    set_status(app, "%s  CC %d = %d", mk1_encoder_name(enc), cc, value);
    pthread_mutex_unlock(&app->lock);

    if (app->cfg.encoder_relative) {
        // Binary offset: 64 + delta (Bitwig "Relative (Bin Offset)", Reaper "Relative 2")
        int rel = 64 + (steps > 63 ? 63 : (steps < -63 ? -63 : steps));
        mk1_midi_cc(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel, cc, rel);
    } else {
        mk1_midi_cc(app->midi, MK1_PORT_CONTROLLER, app->cfg.channel, cc, value);
    }
}

static void usb_ep1_in(void *ctx, const uint8_t *data, size_t len)
{
    app_t *app = ctx;

    // DIN MIDI in: [0x06, port, len, bytes...] (snd-usb-caiaq layout)
    if (len >= 3 && data[0] == MK1_REPORT_MIDI_IN) {
        size_t n = data[2];
        if (n > len - 3) n = len - 3;
        mk1_midi_raw(app->midi, MK1_PORT_DIN, data + 3, n);
        return;
    }
    mk1_input_feed_ep1(&app->input, data, len);
}

static void usb_pads_in(void *ctx, const uint8_t *data, size_t len)
{
    app_t *app = ctx;
    mk1_input_feed_pads(&app->input, data, len, now_ns());
}

// ---------------------------------------------------------------------------
// MIDI feedback (main thread)
// ---------------------------------------------------------------------------

static void set_host_led(app_t *app, uint8_t slot, unsigned level)
{
    uint8_t v = mk1_led_level(level);
    if (slot < MK1_LED_COUNT && app->led_host[slot] != v) {
        app->led_host[slot] = v;
        app->leds_dirty = true;
    }
}

static void handle_sysex(app_t *app, const uint8_t *data, size_t len)
{
    size_t hdr = sizeof(k_sysex_header);
    if (len < hdr + 2 || memcmp(data, k_sysex_header, hdr) != 0) return;
    if (data[len - 1] == 0xf7) len--;

    uint8_t cmd = data[hdr];
    unsigned display = data[hdr + 1];
    if (display >= MK1_DISPLAY_COUNT) return;

    if (cmd == SYSEX_CLEAR_TEXT) {
        memset(app->host_text[display], 0, sizeof(app->host_text[display]));
        app->host_text_active[display] = false;
        app->display_dirty[display] = true;
    } else if (cmd == SYSEX_SET_LINE && len >= hdr + 3) {
        unsigned line = data[hdr + 2];
        if (line >= TEXT_LINES) return;
        size_t n = len - (hdr + 3);
        if (n > TEXT_COLS) n = TEXT_COLS;
        char *dst = app->host_text[display][line];
        for (size_t i = 0; i < n; i++) {
            uint8_t ch = data[hdr + 3 + i];
            dst[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
        }
        dst[n] = '\0';
        app->host_text_active[display] = true;
        app->display_dirty[display] = true;
    }
}

static void handle_controller_event(app_t *app, const snd_seq_event_t *ev)
{
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
    case SND_SEQ_EVENT_NOTEOFF: {
        int pad = ev->data.note.note - app->cfg.pad_base_note;
        int level = ev->type == SND_SEQ_EVENT_NOTEON ? ev->data.note.velocity : 0;
        if (ev->data.note.channel == app->cfg.channel && pad >= 0 && pad < MK1_PAD_COUNT) {
            set_host_led(app, mk1_pad_led_slot((unsigned)pad), (unsigned)level);
        }
        break;
    }
    case SND_SEQ_EVENT_CONTROLLER: {
        unsigned cc = ev->data.control.param;
        int value = ev->data.control.value;
        if (ev->data.control.channel != app->cfg.channel) break;
        for (unsigned b = 0; b < MK1_BTN_COUNT; b++) {
            if (k_button_cc[b] == cc) {
                set_host_led(app, mk1_button_led_slot((mk1_button_t)b), (unsigned)value);
            }
        }
        // In absolute mode the DAW can move our virtual knob position.
        if (!app->cfg.encoder_relative) {
            for (unsigned e = 0; e < MK1_ENC_COUNT; e++) {
                if (k_encoder_cc[e] == cc && value >= 0 && value <= 127) {
                    app->enc_value[e] = value;
                    if (e >= MK1_ENC_SCREEN1) app->display_dirty[(e - MK1_ENC_SCREEN1) / 4] = true;
                }
            }
        }
        break;
    }
    case SND_SEQ_EVENT_SYSEX:
        handle_sysex(app, ev->data.ext.ptr, ev->data.ext.len);
        break;
    default:
        break;
    }
}

static void handle_din_event(app_t *app, const snd_seq_event_t *ev)
{
    uint8_t bytes[256];
    long n = mk1_midi_event_to_bytes(app->midi, MK1_PORT_DIN, ev, bytes, sizeof(bytes));

    if (n <= 0 || !app->usb) return;
    // [0x07, port, len, bytes...]; EP1 packets are limited to 64 bytes.
    for (long off = 0; off < n; off += 61) {
        uint8_t packet[64];
        long chunk = n - off > 61 ? 61 : n - off;
        packet[0] = MK1_CMD_MIDI_WRITE;
        packet[1] = 0x00;
        packet[2] = (uint8_t)chunk;
        memcpy(packet + 3, bytes + off, (size_t)chunk);
        mk1_usb_write(app->usb, MK1_EP_CMD_OUT, packet, (size_t)chunk + 3);
    }
}

static void apply_mcu_changes(app_t *app, unsigned changed)
{
    if (changed & MK1_MCU_CHANGED_LEDS) app->leds_dirty = true;
    if (changed & MK1_MCU_CHANGED_DISPLAY) {
        app->display_dirty[0] = app->display_dirty[1] = true;
    }
}

static void handle_mackie_event(app_t *app, const snd_seq_event_t *ev)
{
    uint8_t bytes[512];
    long n = mk1_midi_event_to_bytes(app->midi, MK1_PORT_MACKIE, ev, bytes, sizeof(bytes));
    if (n <= 0) return;

    pthread_mutex_lock(&app->lock);
    apply_mcu_changes(app, mk1_mcu_host_message(&app->mcu, bytes, (size_t)n));
    pthread_mutex_unlock(&app->lock);
}

static void on_midi_event(void *ctx, mk1_midi_port_t port, const snd_seq_event_t *ev)
{
    app_t *app = ctx;
    if (port == MK1_PORT_DIN) {
        handle_din_event(app, ev);
        return;
    }
    if (port == MK1_PORT_MACKIE) {
        if (app->cfg.mackie) handle_mackie_event(app, ev);
        return;
    }
    pthread_mutex_lock(&app->lock);
    handle_controller_event(app, ev);
    pthread_mutex_unlock(&app->lock);
}

// ---------------------------------------------------------------------------
// Output flushing (main thread)
// ---------------------------------------------------------------------------

static void flush_leds(app_t *app)
{
    mk1_leds_t leds;
    uint8_t a[33], b[33];

    pthread_mutex_lock(&app->lock);
    if (!app->leds_dirty) {
        pthread_mutex_unlock(&app->lock);
        return;
    }
    for (int i = 0; i < MK1_LED_COUNT; i++) {
        leds.value[i] = app->led_host[i] > app->led_local[i] ? app->led_host[i] : app->led_local[i];
    }
    if (app->cfg.mackie) {
        // Button LEDs mirror the DAW's Mackie LED state instead of CC feedback.
        for (int btn = 0; btn < MK1_BTN_COUNT; btn++) {
            int level = mk1_mcu_button_led(&app->mcu, (mk1_button_t)btn);
            uint8_t slot = mk1_button_led_slot((mk1_button_t)btn);
            if (level < 0 || slot >= MK1_LED_COUNT) continue;
            uint8_t host = mk1_led_level((unsigned)level);
            leds.value[slot] = host > app->led_local[slot] ? host : app->led_local[slot];
        }
    }
    leds.value[MK1_LED_BACKLIGHT] = (uint8_t)app->cfg.backlight;
    app->leds_dirty = false;
    pthread_mutex_unlock(&app->lock);

    mk1_leds_build_packets(&leds, a, b);
    if (!mk1_usb_write(app->usb, MK1_EP_CMD_OUT, a, sizeof(a)) ||
        !mk1_usb_write(app->usb, MK1_EP_CMD_OUT, b, sizeof(b))) {
        pthread_mutex_lock(&app->lock);
        app->leds_dirty = true;
        pthread_mutex_unlock(&app->lock);
    }
}

// Default page: four knob columns per display with name, CC, value and a bar.
static void render_knob_page(app_t *app, unsigned display, mk1_canvas_t *c)
{
    const uint8_t on = MK1_DISPLAY_MAX_GRAY;
    char buf[TEXT_COLS + 1];

    mk1_canvas_clear(c, 0);
    mk1_canvas_fill_rect(c, 0, 0, MK1_DISPLAY_WIDTH, 9, on);
    if (display == 0) {
        mk1_canvas_text(c, 2, 1, "MASCHINE MK1", 1, 0);
        snprintf(buf, sizeof(buf), "%s ch%d", app->cfg.encoder_relative ? "rel" : "abs", app->cfg.channel + 1);
        mk1_canvas_text(c, MK1_DISPLAY_WIDTH - 2 - mk1_text_width(buf, 1), 1, buf, 1, 0);
    } else {
        mk1_canvas_text(c, 2, 1, app->status[0] ? app->status : "ready", 1, 0);
    }

    for (unsigned col = 0; col < 4; col++) {
        mk1_encoder_t enc = (mk1_encoder_t)(MK1_ENC_SCREEN1 + display * 4 + col);
        int x = (int)col * 64;
        int value = app->enc_value[enc];

        mk1_canvas_text(c, x + 3, 13, mk1_encoder_name(enc), 1, on);
        snprintf(buf, sizeof(buf), "CC%d", k_encoder_cc[enc]);
        mk1_canvas_text(c, x + 3, 23, buf, 1, on / 2);
        snprintf(buf, sizeof(buf), "%3d", value);
        mk1_canvas_text(c, x + 3, 34, buf, 2, on);
        mk1_canvas_frame_rect(c, x + 3, 54, 56, 8, on);
        mk1_canvas_fill_rect(c, x + 5, 56, value * 52 / 127, 4, on);
        if (col) mk1_canvas_fill_rect(c, x, 12, 1, 50, on / 3);
    }
}

static void render_text_page(app_t *app, unsigned display, mk1_canvas_t *c)
{
    mk1_canvas_clear(c, 0);
    for (int line = 0; line < TEXT_LINES; line++) {
        mk1_canvas_text(c, 1, line * 8, app->host_text[display][line], 1, MK1_DISPLAY_MAX_GRAY);
    }
}

static bool emit_ep8(void *ctx, const uint8_t *packet, size_t len)
{
    return mk1_usb_write(ctx, MK1_EP_DISPLAY_OUT, packet, len);
}

static void flush_displays(app_t *app)
{
    static mk1_canvas_t canvas;
    static uint8_t fb[MK1_DISPLAY_FB_BYTES];

    for (unsigned d = 0; d < MK1_DISPLAY_COUNT; d++) {
        pthread_mutex_lock(&app->lock);
        if (!app->display_dirty[d]) {
            pthread_mutex_unlock(&app->lock);
            continue;
        }
        app->display_dirty[d] = false;
        if (app->host_text_active[d]) {
            render_text_page(app, d, &canvas);
        } else if (app->cfg.mackie) {
            mk1_mcu_render(&app->mcu, d, &canvas);
        } else {
            render_knob_page(app, d, &canvas);
        }
        pthread_mutex_unlock(&app->lock);

        mk1_canvas_pack(&canvas, fb);
        mk1_display_send_frame(d, fb, emit_ep8, app->usb);
    }
}

static void blank_device(app_t *app)
{
    static uint8_t fb[MK1_DISPLAY_FB_BYTES];
    mk1_leds_t leds = { { 0 } };
    uint8_t a[33], b[33];

    mk1_leds_build_packets(&leds, a, b);
    mk1_usb_write(app->usb, MK1_EP_CMD_OUT, a, sizeof(a));
    mk1_usb_write(app->usb, MK1_EP_CMD_OUT, b, sizeof(b));
    for (unsigned d = 0; d < MK1_DISPLAY_COUNT; d++) {
        mk1_display_send_frame(d, fb, emit_ep8, app->usb);
    }
}

// ---------------------------------------------------------------------------
// Device lifecycle
// ---------------------------------------------------------------------------

static void *usb_event_thread(void *arg)
{
    app_t *app = arg;
    while (atomic_load(&g_usb_events)) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(app->usb_ctx, &tv, NULL);
    }
    return NULL;
}

static void device_connected(app_t *app)
{
    MK1_LOG("Maschine MK1 connected");
    if (!mk1_usb_init_hardware(app->usb)) {
        MK1_LOG("hardware init failed");
    }
    pthread_mutex_lock(&app->lock);
    memset(app->led_local, 0, sizeof(app->led_local));
    app->leds_dirty = true;
    app->display_dirty[0] = app->display_dirty[1] = true;
    set_status(app, "connected");
    pthread_mutex_unlock(&app->lock);
}

static void device_disconnected(app_t *app)
{
    MK1_LOG("Maschine MK1 disconnected");
    mk1_usb_close(app->usb);   // waits until no input callback is running
    app->usb = NULL;
    mk1_input_reset(&app->input);   // note-offs / CC 0 for anything still held
}

static void usage(const char *prog)
{
    printf("usage: %s [-v] [-h]\n"
           "  -v  verbose logging (pad/button/encoder events, USB details)\n"
           "  -h  this help\n"
           "Configuration is done through MK1_* environment variables; see README.md.\n",
           prog);
}

int main(int argc, char **argv)
{
    static app_t app;
    pthread_t event_thread;
    uint64_t last_open_attempt = 0, last_display_flush = 0;
    char last_error[256] = "";
    int opt;

    while ((opt = getopt(argc, argv, "vh")) != -1) {
        switch (opt) {
        case 'v': mk1_verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    load_config(&app.cfg);
    pthread_mutex_init(&app.lock, NULL);
    for (int e = 0; e < MK1_ENC_COUNT; e++) app.enc_value[e] = 64;

    mk1_input_callbacks_t callbacks = {
        .pad_hit = cb_pad_hit,
        .pad_pressure = cb_pad_pressure,
        .pad_release = cb_pad_release,
        .button = cb_button,
        .encoder = cb_encoder,
    };
    mk1_input_init(&app.input, &app.cfg.input, &callbacks, &app);

    app.midi = mk1_midi_open("Maschine MK1");
    if (!app.midi) return 1;
    MK1_LOG("ALSA sequencer client %d: ports 'MK1 Controller', 'MK1 DIN', 'MK1 Mackie' (%s mode)",
            mk1_midi_client_id(app.midi), app.cfg.mackie ? "Mackie" : "MIDI");
    mk1_mcu_init(&app.mcu, mcu_send, &app);

    int rc = libusb_init(&app.usb_ctx);
    if (rc != 0) {
        MK1_LOG("libusb_init failed: %s", libusb_error_name(rc));
        mk1_midi_close(app.midi);
        return 1;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pthread_create(&event_thread, NULL, usb_event_thread, &app);

    const mk1_usb_callbacks_t usb_cb = { .ep1_in = usb_ep1_in, .pads_in = usb_pads_in };

    while (atomic_load(&g_running)) {
        uint64_t now = now_ns();

        if (app.usb && !mk1_usb_alive(app.usb)) {
            device_disconnected(&app);
        }
        if (!app.usb && now - last_open_attempt > 1000000000ULL) {
            char err[256] = "";
            last_open_attempt = now;
            app.usb = mk1_usb_open(app.usb_ctx, &usb_cb, &app, err, sizeof(err));
            if (app.usb) {
                last_error[0] = '\0';
                device_connected(&app);
            } else if (strcmp(err, last_error) != 0) {
                MK1_LOG("%s - waiting for device", err);
                snprintf(last_error, sizeof(last_error), "%s", err);
            }
        }

        struct pollfd fds[8];
        int nfds = mk1_midi_poll_fds(app.midi, fds, 8);
        int ready = poll(fds, (nfds_t)nfds, 10);
        if (ready > 0) {
            mk1_midi_dispatch(app.midi, on_midi_event, &app);
        } else if (ready < 0 && errno != EINTR) {
            MK1_LOG("poll failed: %s", strerror(errno));
        }

        if (app.cfg.mackie) {
            pthread_mutex_lock(&app.lock);
            apply_mcu_changes(&app, mk1_mcu_tick(&app.mcu, now));
            pthread_mutex_unlock(&app.lock);
        }

        if (app.usb) {
            flush_leds(&app);
            // A full frame is ~22 EP8 transfers per display; cap the refresh rate.
            if (now - last_display_flush > 1000000000ULL / (uint64_t)app.cfg.display_fps) {
                last_display_flush = now;
                flush_displays(&app);
            }
        }
    }

    MK1_LOG("shutting down");
    if (app.usb) {
        blank_device(&app);
        mk1_usb_close(app.usb);
        app.usb = NULL;
        mk1_input_reset(&app.input);
    }
    atomic_store(&g_usb_events, false);
    pthread_join(event_thread, NULL);
    libusb_exit(app.usb_ctx);
    mk1_midi_close(app.midi);
    return 0;
}

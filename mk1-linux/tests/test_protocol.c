// Hardware-free tests for the MK1 protocol code (input decoding, LEDs, display).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mk1_display.h"
#include "mk1_input.h"
#include "mk1_leds.h"
#include "mk1_mackie.h"

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

// ---------------------------------------------------------------------------
// Event recorder
// ---------------------------------------------------------------------------

typedef struct { char kind; int a; int b; } event_t;
typedef struct { event_t ev[256]; int n; } recorder_t;

static void push(recorder_t *r, char kind, int a, int b)
{
    if (r->n < 256) r->ev[r->n++] = (event_t){ kind, a, b };
}
static void rec_hit(void *c, unsigned pad, int peak)      { push(c, 'H', (int)pad, peak); }
static void rec_pressure(void *c, unsigned pad, int p)    { push(c, 'P', (int)pad, p); }
static void rec_release(void *c, unsigned pad)            { push(c, 'R', (int)pad, 0); }
static void rec_button(void *c, mk1_button_t b, bool on)  { push(c, 'B', (int)b, on); }
static void rec_encoder(void *c, mk1_encoder_t e, int d)  { push(c, 'E', (int)e, d); }

static const mk1_input_callbacks_t k_callbacks = {
    .pad_hit = rec_hit, .pad_pressure = rec_pressure, .pad_release = rec_release,
    .button = rec_button, .encoder = rec_encoder,
};

static void setup(mk1_input_t *in, recorder_t *rec)
{
    mk1_input_config_t cfg;
    mk1_input_default_config(&cfg);
    memset(rec, 0, sizeof(*rec));
    mk1_input_init(in, &cfg, &k_callbacks, rec);
}

// ---------------------------------------------------------------------------
// Pads
// ---------------------------------------------------------------------------

// Pressure report: word i = (i << 12) | value; channel B differs slightly so
// the report is not mistaken for a scan table.
static void pad_report(uint8_t out[64], const int pressure[16], int resting)
{
    for (int i = 0; i < 16; i++) {
        uint16_t a = (uint16_t)((i << 12) | ((resting + pressure[i]) & 0x0fff));
        uint16_t b = (uint16_t)((i << 12) | ((resting + pressure[i] + 1) & 0x0fff));
        out[i * 2] = a & 0xff;        out[i * 2 + 1] = a >> 8;
        out[32 + i * 2] = b & 0xff;   out[32 + i * 2 + 1] = b >> 8;
    }
}

static void feed_pressure(mk1_input_t *in, int pad, int value, uint64_t t)
{
    int p[16] = { 0 };
    uint8_t r[64];
    p[pad] = value;
    pad_report(r, p, 20);
    mk1_input_feed_pads(in, r, sizeof(r), t);
}

static void test_pad_hit_and_release(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint64_t t = 1000000000ULL;
    setup(&in, &rec);

    feed_pressure(&in, 0, 0, t);           // baseline
    CHECK(rec.n == 0);
    CHECK(in.pad_baseline_set);

    feed_pressure(&in, 4, 500, t += 1000000);
    feed_pressure(&in, 4, 900, t += 1000000);
    CHECK(rec.n == 0);                      // sustain gate: 2 reports are not enough
    feed_pressure(&in, 4, 700, t += 1000000);
    CHECK(rec.n == 1 && rec.ev[0].kind == 'H' && rec.ev[0].a == 4);
    CHECK(rec.ev[0].b == 900);              // peak, not the current value

    feed_pressure(&in, 4, 750, t += 1000000);
    CHECK(rec.n == 1);                      // below pressure step
    feed_pressure(&in, 4, 1100, t += 1000000);
    CHECK(rec.n == 2 && rec.ev[1].kind == 'P' && rec.ev[1].b == 1100);

    feed_pressure(&in, 4, 200, t += 1000000);
    CHECK(rec.n == 3 && rec.ev[2].kind == 'P');   // still held: above hit_off (150)
    feed_pressure(&in, 4, 0, t += 1000000);
    CHECK(rec.n == 4 && rec.ev[3].kind == 'R' && rec.ev[3].a == 4);
}

static void test_pad_bounce_and_debounce(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint64_t t = 1000000000ULL;
    setup(&in, &rec);
    feed_pressure(&in, 0, 0, t);

    // Single-report spike: no hit.
    feed_pressure(&in, 2, 800, t += 1000000);
    feed_pressure(&in, 2, 0, t += 1000000);
    CHECK(rec.n == 0);

    // Real hit and release, then an immediate retrigger inside 10 ms.
    for (int i = 0; i < 3; i++) feed_pressure(&in, 2, 800, t += 1000000);
    feed_pressure(&in, 2, 0, t += 1000000);
    CHECK(rec.n == 2);
    for (int i = 0; i < 3; i++) feed_pressure(&in, 2, 800, t += 1000000);
    CHECK(rec.n == 2);                      // debounced
    feed_pressure(&in, 2, 0, t += 1000000);
    t += 20000000;
    for (int i = 0; i < 3; i++) feed_pressure(&in, 2, 800, t += 1000000);
    CHECK(rec.n == 3 && rec.ev[2].kind == 'H');
}

static void test_pad_scan_tables(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint64_t t = 1000000000ULL;
    uint8_t r[64];
    setup(&in, &rec);
    feed_pressure(&in, 0, 0, t);

    // Rotated scan table (phase 3) with large low bits: must be ignored.
    for (int i = 0; i < 16; i++) {
        uint16_t w = (uint16_t)((((i + 3) & 0xf) << 12) | 0x0abc);
        r[i * 2] = w & 0xff; r[i * 2 + 1] = w >> 8;
        r[32 + i * 2] = w & 0xff; r[32 + i * 2 + 1] = w >> 8;
    }
    for (int k = 0; k < 5; k++) mk1_input_feed_pads(&in, r, sizeof(r), t += 1000000);
    CHECK(rec.n == 0);

    // Phase-0 scan table may not start hits ...
    for (int i = 0; i < 16; i++) {
        uint16_t w = (uint16_t)((i << 12) | 0x0800);
        r[i * 2] = w & 0xff; r[i * 2 + 1] = w >> 8;
        r[32 + i * 2] = w & 0xff; r[32 + i * 2 + 1] = w >> 8;
    }
    for (int k = 0; k < 5; k++) mk1_input_feed_pads(&in, r, sizeof(r), t += 1000000);
    CHECK(rec.n == 0);

    // ... but it can release a held pad.
    for (int i = 0; i < 3; i++) feed_pressure(&in, 7, 1000, t += 1000000);
    CHECK(rec.n == 1);
    for (int i = 0; i < 16; i++) {
        uint16_t w = (uint16_t)((i << 12) | 20);   // resting level
        r[i * 2] = w & 0xff; r[i * 2 + 1] = w >> 8;
        r[32 + i * 2] = w & 0xff; r[32 + i * 2 + 1] = w >> 8;
    }
    mk1_input_feed_pads(&in, r, sizeof(r), t += 1000000);
    CHECK(rec.n == 2 && rec.ev[1].kind == 'R' && rec.ev[1].a == 7);
}

static void test_pad_multi_report_transfer(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint8_t buf[64 * 4];
    int p[16] = { 0 };
    setup(&in, &rec);

    pad_report(buf, p, 20);
    p[15] = 600;
    pad_report(buf + 64, p, 20);
    pad_report(buf + 128, p, 20);
    pad_report(buf + 192, p, 20);
    mk1_input_feed_pads(&in, buf, sizeof(buf), 1000000000ULL);
    CHECK(rec.n == 1 && rec.ev[0].kind == 'H' && rec.ev[0].a == 15);
}

static void test_reset_releases(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint64_t t = 1000000000ULL;
    uint8_t b[8] = { 0x04 };
    setup(&in, &rec);
    feed_pressure(&in, 0, 0, t);
    for (int i = 0; i < 3; i++) feed_pressure(&in, 9, 1000, t += 1000000);
    mk1_input_feed_ep1(&in, b, sizeof(b));
    b[6] = 0x02;   // Play down
    mk1_input_feed_ep1(&in, b, sizeof(b));
    CHECK(rec.n == 2);
    mk1_input_reset(&in);
    CHECK(rec.n == 4);
    CHECK(rec.ev[2].kind == 'R' && rec.ev[2].a == 9);
    CHECK(rec.ev[3].kind == 'B' && rec.ev[3].a == MK1_BTN_PLAY && rec.ev[3].b == 0);
    CHECK(!in.pad_baseline_set);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

static void test_buttons(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint8_t r[8] = { 0x04, 0, 0, 0, 0, 0, 0, 0 };
    setup(&in, &rec);

    r[1] = 0x01;                            // Mute held at startup: baseline only
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 0);

    r[1] = 0x00; r[3] = 0x80; r[5] = 0x01;  // Mute up, Group A + Screen 8 down
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 2);                      // Mute was never reported down: no release
    CHECK(rec.ev[0].a != MK1_BTN_MUTE && rec.ev[1].a != MK1_BTN_MUTE);
    CHECK(in.button_state[MK1_BTN_GROUP_A] && in.button_state[MK1_BTN_SCREEN8]);

    memset(&rec, 0, sizeof(rec));
    r[3] = 0x00;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 1 && rec.ev[0].a == MK1_BTN_GROUP_A && rec.ev[0].b == 0);

    // Wrong length / type is ignored.
    uint8_t junk[8] = { 0x05, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    mk1_input_feed_ep1(&in, junk, sizeof(junk));
    CHECK(rec.n == 1);
}

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------

static void test_encoders(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint8_t r[33];
    setup(&in, &rec);

    memset(r, 0x40, sizeof(r));
    r[0] = 0x02;
    r[4] = 0x40;                            // keep byte 4 out of the button ranges
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 0);

    // Volume (bytes 17/18), both below 128: x up, y down => increasing.
    r[17] = 0x44; r[18] = 0x3c;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 1 && rec.ev[0].a == MK1_ENC_VOLUME && rec.ev[0].b == 4);

    // Reverse direction.
    r[17] = 0x40; r[18] = 0x40;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 2 && rec.ev[1].b == -4);

    // Single-count jitter is filtered.
    r[1] = 0x41;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 2);

    // Crosstalk: Knob 1 moves by 10, Knob 2 by 3 in the same report.
    r[21] = 0x4a; r[15] = 0x43;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    CHECK(rec.n == 3 && rec.ev[2].a == MK1_ENC_SCREEN1);
}

static void test_len33_buttons(void)
{
    mk1_input_t in;
    recorder_t rec;
    uint8_t r[33];
    setup(&in, &rec);

    memset(r, 0, sizeof(r));
    r[0] = 0x02;
    r[4] = 0x55;
    mk1_input_feed_ep1(&in, r, sizeof(r));
    mk1_input_feed_ep1(&in, r, sizeof(r));
    r[4] = 0x75;                            // Knob 4 byte moved: treated as motion
    mk1_input_feed_ep1(&in, r, sizeof(r));
    int after_motion = rec.n;
    mk1_input_feed_ep1(&in, r, sizeof(r));  // stable 0x7x => Auto Write down
    CHECK(rec.n == after_motion + 1);
    CHECK(rec.ev[rec.n - 1].kind == 'B' && rec.ev[rec.n - 1].a == MK1_BTN_AUTO_WRITE && rec.ev[rec.n - 1].b == 1);
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

static void test_leds(void)
{
    mk1_leds_t leds;
    uint8_t a[33], b[33];

    for (int i = 0; i < MK1_LED_COUNT; i++) leds.value[i] = (uint8_t)(i + 1);
    mk1_leds_build_packets(&leds, a, b);
    CHECK(a[0] == 0x0c && a[1] == 0x00 && a[2] == 1 && a[32] == 31);
    CHECK(b[0] == 0x0c && b[1] == 0x1e && b[2] == 32 && b[32] == 62);

    // Pad 1 (bottom-left) is slot 15, pad 4 slot 12, pad 13 slot 3, pad 16 slot 0.
    CHECK(mk1_pad_led_slot(0) == 15);
    CHECK(mk1_pad_led_slot(3) == 12);
    CHECK(mk1_pad_led_slot(4) == 11);
    CHECK(mk1_pad_led_slot(12) == 3);
    CHECK(mk1_pad_led_slot(15) == 0);

    CHECK(mk1_button_led_slot(MK1_BTN_PLAY) == 29);
    CHECK(mk1_button_led_slot(MK1_BTN_GROUP_A) == 40);
    CHECK(mk1_button_led_slot(MK1_BTN_SCREEN1) == 56);

    // Every button has a distinct LED slot outside the pad range.
    bool used[MK1_LED_COUNT] = { false };
    for (int i = 0; i < MK1_BTN_COUNT; i++) {
        uint8_t s = mk1_button_led_slot((mk1_button_t)i);
        CHECK(s >= 16 && s < MK1_LED_COUNT && s != MK1_LED_BACKLIGHT && !used[s]);
        if (s < MK1_LED_COUNT) used[s] = true;
    }

    CHECK(mk1_led_level(0) == 0);
    CHECK(mk1_led_level(1) == MK1_LED_DIM);
    CHECK(mk1_led_level(64) == MK1_LED_MEDIUM);
    CHECK(mk1_led_level(127) == MK1_LED_BRIGHT);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

typedef struct { int packets; size_t pixel_bytes; bool ok; uint8_t stream[MK1_DISPLAY_FB_BYTES + 1]; } ep8_capture_t;

static bool capture_ep8(void *ctx, const uint8_t *p, size_t len)
{
    ep8_capture_t *cap = ctx;
    size_t payload = ((size_t)p[1] << 8) | p[2];
    if (payload + 3 != len || payload > MK1_EP8_MAX_PAYLOAD) cap->ok = false;
    cap->packets++;
    if (cap->packets <= 2) {
        // address window commands
        if (!(p[0] == 0x02 && (p[3] == 0x75 || p[3] == 0x15))) cap->ok = false;
        return true;
    }
    if (cap->packets == 3 && (p[0] != 0x02 || p[3] != MK1_ST7529_RAMWR)) cap->ok = false;
    if (cap->packets > 3 && p[0] != 0x03) cap->ok = false;
    if (cap->pixel_bytes + payload > sizeof(cap->stream)) {
        cap->ok = false;
        return false;
    }
    memcpy(cap->stream + cap->pixel_bytes, p + 3, payload);
    cap->pixel_bytes += payload;
    return true;
}

static void test_display(void)
{
    static mk1_canvas_t c;
    static uint8_t fb[MK1_DISPLAY_FB_BYTES];
    static ep8_capture_t cap;

    mk1_canvas_clear(&c, 0);
    c.px[0][0] = 0x1f; c.px[0][1] = 0x1f; c.px[0][2] = 0x1f;
    c.px[1][254] = 0x1f;
    mk1_canvas_pack(&c, fb);
    CHECK(fb[0] == 0xff && fb[1] == 0xdf);  // 11111 11111 11111 packed as a<<3|b>>2, b<<6|c
    CHECK(fb[MK1_DISPLAY_ROW_BYTES + 168] == 0x00 && fb[MK1_DISPLAY_ROW_BYTES + 169] == 0x1f);

    cap.ok = true;
    CHECK(mk1_display_send_frame(1, fb, capture_ep8, &cap));
    CHECK(cap.ok);
    CHECK(cap.pixel_bytes == MK1_DISPLAY_FB_BYTES + 1);
    CHECK(cap.packets == 2 + 22);           // ceil(10881 / 508) = 22 data chunks
    CHECK(memcmp(cap.stream + 1, fb, MK1_DISPLAY_FB_BYTES) == 0);

    mk1_canvas_clear(&c, 0);
    int x = mk1_canvas_text(&c, 0, 0, "A", 1, 0x1f);
    CHECK(x == 6);
    CHECK(c.px[0][1] == 0x1f && c.px[0][0] == 0);   // 'A' column 0 is 0x7e: top row off
    CHECK(mk1_text_width("ABC", 2) == 34);

    CHECK(mk1_display_init_cmd_count == 18);
    CHECK(mk1_display_init_cmds[mk1_display_init_cmd_count - 1].bytes[0] == 0xaf);
}

// ---------------------------------------------------------------------------
// Mackie Control
// ---------------------------------------------------------------------------

typedef struct { uint8_t bytes[64][24]; size_t len[64]; int n; } mcu_out_t;

static void mcu_capture(void *ctx, const uint8_t *bytes, size_t len)
{
    mcu_out_t *o = ctx;
    if (o->n < 64 && len <= 24) {
        memcpy(o->bytes[o->n], bytes, len);
        o->len[o->n++] = len;
    }
}

static bool sent(const mcu_out_t *o, int i, uint8_t a, uint8_t b, uint8_t c)
{
    return i < o->n && o->len[i] == 3 && o->bytes[i][0] == a && o->bytes[i][1] == b && o->bytes[i][2] == c;
}

static void host(mk1_mcu_t *m, uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t msg[3] = { a, b, c };
    mk1_mcu_host_message(m, msg, 3);
}

static void test_mcu_buttons(void)
{
    static mk1_mcu_t m;
    static mcu_out_t o;
    memset(&o, 0, sizeof(o));
    mk1_mcu_init(&m, mcu_capture, &o);

    // Screen 3 = select strip 3
    CHECK(mk1_mcu_button(&m, MK1_BTN_SCREEN3, true));
    CHECK(mk1_mcu_button(&m, MK1_BTN_SCREEN3, false));
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x1a, 0x7f) && sent(&o, 1, 0x90, 0x1a, 0x00));

    // Mute + Screen 1 = mute strip 1; the release matches the press even if
    // Mute is let go first.
    o.n = 0;
    mk1_mcu_button(&m, MK1_BTN_MUTE, true);
    mk1_mcu_button(&m, MK1_BTN_SCREEN1, true);
    mk1_mcu_button(&m, MK1_BTN_MUTE, false);
    mk1_mcu_button(&m, MK1_BTN_SCREEN1, false);
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x10, 0x7f) && sent(&o, 1, 0x90, 0x10, 0x00));

    // Rec on its own = transport record (sent on release)
    o.n = 0;
    mk1_mcu_button(&m, MK1_BTN_REC, true);
    CHECK(o.n == 0);
    mk1_mcu_button(&m, MK1_BTN_REC, false);
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x5f, 0x7f) && sent(&o, 1, 0x90, 0x5f, 0x00));

    // Rec + Screen 2 = arm strip 2, no transport record
    o.n = 0;
    mk1_mcu_button(&m, MK1_BTN_REC, true);
    mk1_mcu_button(&m, MK1_BTN_SCREEN2, true);
    mk1_mcu_button(&m, MK1_BTN_SCREEN2, false);
    mk1_mcu_button(&m, MK1_BTN_REC, false);
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x01, 0x7f));

    // Shift + Group A = F1, plain Group A = Track assign
    o.n = 0;
    mk1_mcu_button(&m, MK1_BTN_SHIFT, true);
    mk1_mcu_button(&m, MK1_BTN_GROUP_A, true);
    mk1_mcu_button(&m, MK1_BTN_SHIFT, false);
    mk1_mcu_button(&m, MK1_BTN_GROUP_A, false);
    mk1_mcu_button(&m, MK1_BTN_GROUP_A, true);
    CHECK(o.n == 3 && sent(&o, 0, 0x90, 0x36, 0x7f) && sent(&o, 1, 0x90, 0x36, 0x00) &&
          sent(&o, 2, 0x90, 0x28, 0x7f));

    // Buttons without an MCU function fall through to the CC layer.
    o.n = 0;
    CHECK(!mk1_mcu_button(&m, MK1_BTN_PATTERN, true));
    CHECK(!mk1_mcu_button(&m, MK1_BTN_PATTERN, false));
    CHECK(o.n == 0);
}

static void test_mcu_encoders(void)
{
    static mk1_mcu_t m;
    static mcu_out_t o;
    uint64_t t = 1000000000ULL;
    memset(&o, 0, sizeof(o));
    mk1_mcu_init(&m, mcu_capture, &o);

    mk1_mcu_encoder(&m, MK1_ENC_SCREEN1, 3, t);
    mk1_mcu_encoder(&m, MK1_ENC_SCREEN8, -2, t);
    mk1_mcu_encoder(&m, MK1_ENC_TEMPO, 1, t);
    CHECK(sent(&o, 0, 0xb0, 0x10, 0x03) && sent(&o, 1, 0xb0, 0x17, 0x42) && sent(&o, 2, 0xb0, 0x3c, 0x01));

    // Volume = master fader: touch, then pitch bend on channel 9; touch
    // released after the knob stops.
    o.n = 0;
    mk1_mcu_encoder(&m, MK1_ENC_VOLUME, 4, t);
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x70, 0x7f) && sent(&o, 1, 0xe8, 0x00, 0x04));
    mk1_mcu_tick(&m, t + 100000000ULL);
    CHECK(o.n == 2);
    mk1_mcu_tick(&m, t + 500000000ULL);
    CHECK(o.n == 3 && sent(&o, 2, 0x90, 0x70, 0x00));

    // Swing = fader of the strip the DAW reports as selected.
    o.n = 0;
    host(&m, 0x90, 0x1a, 0x7f);   // strip 3 selected
    mk1_mcu_encoder(&m, MK1_ENC_SWING, -1, t);
    CHECK(o.n == 2 && sent(&o, 0, 0x90, 0x6a, 0x7f) && o.bytes[1][0] == 0xe2);
}

static void test_mcu_host(void)
{
    static mk1_mcu_t m;
    static mcu_out_t o;
    static mk1_canvas_t c;
    memset(&o, 0, sizeof(o));
    mk1_mcu_init(&m, mcu_capture, &o);

    // Handshake
    const uint8_t query[] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xf7 };
    mk1_mcu_host_message(&m, query, sizeof(query));
    CHECK(o.n == 1 && o.len[0] == 18 && o.bytes[0][5] == 0x01 && o.bytes[0][17] == 0xf7);

    // LCD text at offset 7 (strip 2, upper row) and 56 (strip 1, lower row)
    const uint8_t lcd1[] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 7, 'B', 'a', 's', 's', 0xf7 };
    const uint8_t lcd2[] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 56, '-', '6', 'd', 'B', 0xf7 };
    CHECK(mk1_mcu_host_message(&m, lcd1, sizeof(lcd1)) & MK1_MCU_CHANGED_DISPLAY);
    mk1_mcu_host_message(&m, lcd2, sizeof(lcd2));
    CHECK(memcmp(&m.lcd[0][7], "Bass", 4) == 0 && memcmp(&m.lcd[1][0], "-6dB", 4) == 0);

    // LEDs
    host(&m, 0x90, 0x5e, 0x7f);   // Play
    host(&m, 0x90, 0x12, 0x7f);   // mute strip 3
    host(&m, 0x90, 0x5f, 0x01);   // record blinking
    CHECK(mk1_mcu_button_led(&m, MK1_BTN_PLAY) == 127);
    CHECK(mk1_mcu_button_led(&m, MK1_BTN_REC) == 64);
    CHECK(mk1_mcu_button_led(&m, MK1_BTN_SCREEN3) == 0);   // shows select
    mk1_mcu_button(&m, MK1_BTN_MUTE, true);
    CHECK(mk1_mcu_button_led(&m, MK1_BTN_SCREEN3) == 127); // shows mute while held
    mk1_mcu_button(&m, MK1_BTN_MUTE, false);
    CHECK(mk1_mcu_button_led(&m, MK1_BTN_PATTERN) == -1);

    // V-Pot ring, fader, meter + decay
    host(&m, 0xb0, 0x31, 0x16);
    host(&m, 0xe1, 0x7f, 0x7f);
    host(&m, 0xd0, 0x25, 0x00);
    CHECK(m.vpot_ring[1] == 0x16 && m.fader[1] == 16383 && m.meter[2] == 5);
    mk1_mcu_tick(&m, 2000000000ULL);
    CHECK(m.meter[2] == 4);

    // Rendering: selected strip 2 gets an inverted name bar.
    host(&m, 0x90, 0x19, 0x7f);
    mk1_mcu_render(&m, 0, &c);
    CHECK(c.px[0][64 + 2] == MK1_DISPLAY_MAX_GRAY && c.px[0][2] == 0);
    mk1_mcu_render(&m, 1, &c);

    // Reset
    const uint8_t reset[] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x63, 0xf7 };
    mk1_mcu_host_message(&m, reset, sizeof(reset));
    CHECK(m.note[0x5e] == 0 && m.lcd[0][7] == ' ');
}

int main(void)
{
    test_pad_hit_and_release();
    test_pad_bounce_and_debounce();
    test_pad_scan_tables();
    test_pad_multi_report_transfer();
    test_reset_releases();
    test_buttons();
    test_encoders();
    test_len33_buttons();
    test_leds();
    test_display();
    test_mcu_buttons();
    test_mcu_encoders();
    test_mcu_host();

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all protocol tests passed\n");
    return 0;
}

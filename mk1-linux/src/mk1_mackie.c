#include "mk1_mackie.h"

#include <stdio.h>
#include <string.h>

// MCU note numbers (Mackie Control protocol, as used by Logic/Reaper/Ardour/Bitwig)
#define MCU_REC_ARM       0x00   // + strip
#define MCU_SOLO          0x08
#define MCU_MUTE          0x10
#define MCU_SELECT        0x18
#define MCU_VPOT_PUSH     0x20
#define MCU_ASSIGN_TRACK  0x28
#define MCU_ASSIGN_SEND   0x29
#define MCU_ASSIGN_PAN    0x2a
#define MCU_ASSIGN_PLUGIN 0x2b
#define MCU_ASSIGN_EQ     0x2c
#define MCU_ASSIGN_INSTR  0x2d
#define MCU_BANK_LEFT     0x2e
#define MCU_BANK_RIGHT    0x2f
#define MCU_CHANNEL_LEFT  0x30
#define MCU_CHANNEL_RIGHT 0x31
#define MCU_FLIP          0x32
#define MCU_GLOBAL_VIEW   0x33
#define MCU_F1            0x36
#define MCU_OPTION        0x47
#define MCU_READ          0x4a
#define MCU_WRITE         0x4b
#define MCU_SAVE          0x50
#define MCU_UNDO          0x51
#define MCU_ENTER         0x53
#define MCU_MARKER        0x54
#define MCU_NUDGE         0x55
#define MCU_CYCLE         0x56
#define MCU_CLICK         0x59
#define MCU_REWIND        0x5b
#define MCU_FAST_FWD      0x5c
#define MCU_STOP          0x5d
#define MCU_PLAY          0x5e
#define MCU_RECORD        0x5f
#define MCU_ZOOM          0x64
#define MCU_SCRUB         0x65
#define MCU_FADER_TOUCH   0x68   // + fader (0..8)

#define MCU_CC_VPOT       0x10   // device -> host, + strip
#define MCU_CC_JOG        0x3c
#define MCU_CC_VPOT_RING  0x30   // host -> device, + strip

#define NONE              0xff
#define TOUCH_HOLD_NS     400000000ULL
#define METER_DECAY_NS    300000000ULL

// MK1 button -> MCU note (plain / with Shift held). Screen buttons, Mute,
// Solo, Rec and Shift are handled separately.
static const struct { uint8_t plain, shifted; } k_map[MK1_BTN_COUNT] = {
    [MK1_BTN_PLAY]            = { MCU_PLAY,         MCU_PLAY },
    [MK1_BTN_RESTART]         = { MCU_STOP,         MCU_STOP },
    [MK1_BTN_TRANSPORT_LEFT]  = { MCU_REWIND,       MCU_MARKER },
    [MK1_BTN_TRANSPORT_RIGHT] = { MCU_FAST_FWD,     MCU_NUDGE },
    [MK1_BTN_GRID]            = { MCU_CYCLE,        MCU_CLICK },
    [MK1_BTN_ERASE]           = { MCU_UNDO,         MCU_SAVE },
    [MK1_BTN_LEFT]            = { MCU_BANK_LEFT,    MCU_CHANNEL_LEFT },
    [MK1_BTN_RIGHT]           = { MCU_BANK_RIGHT,   MCU_CHANNEL_RIGHT },
    [MK1_BTN_GROUP_A]         = { MCU_ASSIGN_TRACK, MCU_F1 + 0 },
    [MK1_BTN_GROUP_B]         = { MCU_ASSIGN_SEND,  MCU_F1 + 1 },
    [MK1_BTN_GROUP_C]         = { MCU_ASSIGN_PAN,   MCU_F1 + 2 },
    [MK1_BTN_GROUP_D]         = { MCU_ASSIGN_PLUGIN, MCU_F1 + 3 },
    [MK1_BTN_GROUP_E]         = { MCU_ASSIGN_EQ,    MCU_F1 + 4 },
    [MK1_BTN_GROUP_F]         = { MCU_ASSIGN_INSTR, MCU_F1 + 5 },
    [MK1_BTN_GROUP_G]         = { MCU_FLIP,         MCU_F1 + 6 },
    [MK1_BTN_GROUP_H]         = { MCU_GLOBAL_VIEW,  MCU_F1 + 7 },
    [MK1_BTN_AUTO_WRITE]      = { MCU_WRITE,        MCU_READ },
    [MK1_BTN_SNAP]            = { MCU_MARKER,       MCU_MARKER },
    [MK1_BTN_STEP]            = { MCU_SCRUB,        MCU_ZOOM },
    [MK1_BTN_BROWSE]          = { MCU_ENTER,        MCU_ENTER },
    [MK1_BTN_CONTROL]         = { MCU_OPTION,       MCU_OPTION },
};

static bool is_mapped(mk1_button_t b)
{
    return k_map[b].plain != 0;   // note 0 (rec arm 1) is never a k_map target
}

static int screen_index(mk1_button_t b)
{
    return (b >= MK1_BTN_SCREEN1 && b <= MK1_BTN_SCREEN8) ? (int)(b - MK1_BTN_SCREEN1) : -1;
}

static void send3(mk1_mcu_t *m, uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t msg[3] = { a, b, c };
    m->send(m->ctx, msg, sizeof(msg));
}

static void send_note(mk1_mcu_t *m, uint8_t note, bool on)
{
    send3(m, 0x90, note, on ? 0x7f : 0x00);
}

void mk1_mcu_init(mk1_mcu_t *m, mk1_mcu_send_fn send, void *ctx)
{
    memset(m, 0, sizeof(*m));
    m->send = send;
    m->ctx = ctx;
    memset(m->lcd, ' ', sizeof(m->lcd));
    memset(m->pressed_note, NONE, sizeof(m->pressed_note));
    m->assignment[0] = m->assignment[1] = ' ';
    for (int i = 0; i < MK1_MCU_FADERS; i++) m->fader[i] = 0;
}

// ---------------------------------------------------------------------------
// MK1 -> DAW
// ---------------------------------------------------------------------------

bool mk1_mcu_button(mk1_mcu_t *m, mk1_button_t b, bool pressed)
{
    int strip = screen_index(b);

    switch (b) {
    case MK1_BTN_SHIFT:
        m->shift = pressed;
        return true;
    case MK1_BTN_MUTE:
        m->mute_held = pressed;
        return true;
    case MK1_BTN_SOLO:
        m->solo_held = pressed;
        return true;
    case MK1_BTN_REC:
        // Rec doubles as the arm modifier: Rec + Screen N arms strip N, Rec on
        // its own (released without a Screen button) is transport record.
        if (pressed) {
            m->rec_held = true;
            m->rec_combined = false;
        } else {
            m->rec_held = false;
            if (!m->rec_combined) {
                send_note(m, MCU_RECORD, true);
                send_note(m, MCU_RECORD, false);
            }
        }
        return true;
    default:
        break;
    }

    uint8_t note;
    if (!pressed) {
        note = m->pressed_note[b];
        if (note == NONE) return strip >= 0 || is_mapped(b);
        m->pressed_note[b] = NONE;
        send_note(m, note, false);
        return true;
    }

    if (strip >= 0) {
        if (m->rec_held) {
            note = (uint8_t)(MCU_REC_ARM + strip);
            m->rec_combined = true;
        } else if (m->solo_held) {
            note = (uint8_t)(MCU_SOLO + strip);
        } else if (m->mute_held) {
            note = (uint8_t)(MCU_MUTE + strip);
        } else if (m->shift) {
            note = (uint8_t)(MCU_VPOT_PUSH + strip);
        } else {
            note = (uint8_t)(MCU_SELECT + strip);
        }
    } else if (is_mapped(b)) {
        note = m->shift ? k_map[b].shifted : k_map[b].plain;
    } else {
        return false;
    }

    m->pressed_note[b] = note;
    send_note(m, note, true);
    return true;
}

static uint8_t relative_value(int steps)
{
    // MCU encoders: bit 6 = counter-clockwise, bits 0-5 = number of ticks.
    if (steps > 63) steps = 63;
    if (steps < -63) steps = -63;
    return steps >= 0 ? (uint8_t)steps : (uint8_t)(0x40 | -steps);
}

static int selected_strip(const mk1_mcu_t *m)
{
    for (int i = 0; i < MK1_MCU_STRIPS; i++) {
        if (m->note[MCU_SELECT + i]) return i;
    }
    return -1;
}

static unsigned move_fader(mk1_mcu_t *m, int fader, int steps, uint64_t now_ns)
{
    int v = (int)m->fader[fader] + steps * 128;   // 128 steps across the full range
    v = v < 0 ? 0 : (v > 16383 ? 16383 : v);

    if (!m->touched[fader]) {
        m->touched[fader] = true;
        send_note(m, (uint8_t)(MCU_FADER_TOUCH + fader), true);
    }
    m->touch_until_ns[fader] = now_ns + TOUCH_HOLD_NS;
    m->fader[fader] = (uint16_t)v;
    send3(m, (uint8_t)(0xe0 | fader), (uint8_t)(v & 0x7f), (uint8_t)(v >> 7));
    return MK1_MCU_CHANGED_DISPLAY;
}

unsigned mk1_mcu_encoder(mk1_mcu_t *m, mk1_encoder_t e, int steps, uint64_t now_ns)
{
    if (steps == 0) return 0;

    if (e >= MK1_ENC_SCREEN1 && e <= MK1_ENC_SCREEN8) {
        send3(m, 0xb0, (uint8_t)(MCU_CC_VPOT + (e - MK1_ENC_SCREEN1)), relative_value(steps));
        return 0;
    }
    switch (e) {
    case MK1_ENC_TEMPO:
        send3(m, 0xb0, MCU_CC_JOG, relative_value(steps));
        return 0;
    case MK1_ENC_VOLUME:
        return move_fader(m, MK1_MCU_MASTER, steps, now_ns);
    case MK1_ENC_SWING: {
        int strip = selected_strip(m);
        return move_fader(m, strip >= 0 ? strip : 0, steps, now_ns);
    }
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// DAW -> MK1
// ---------------------------------------------------------------------------

static const uint8_t k_serial[7] = { 'M', 'K', '1', 'L', 'N', 'X', '1' };

static unsigned handle_sysex(mk1_mcu_t *m, const uint8_t *msg, size_t len)
{
    // F0 00 00 66 <model> <cmd> ... F7; model 0x14 = MCU, 0x10 = Logic Control
    if (len < 7 || msg[1] != 0x00 || msg[2] != 0x00 || msg[3] != 0x66) return 0;
    if (msg[4] != 0x14 && msg[4] != 0x10) return 0;
    if (msg[len - 1] == 0xf7) len--;

    const uint8_t *data = msg + 6;
    size_t n = len - 6;

    switch (msg[5]) {
    case 0x00: {   // device query -> host connection query (serial + challenge)
        uint8_t reply[18] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x01 };
        memcpy(reply + 6, k_serial, 7);
        memcpy(reply + 13, "abcd", 4);
        reply[17] = 0xf7;
        m->send(m->ctx, reply, sizeof(reply));
        return 0;
    }
    case 0x02: {   // host connection reply -> confirmation
        uint8_t reply[14] = { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x03 };
        memcpy(reply + 6, k_serial, 7);
        reply[13] = 0xf7;
        m->send(m->ctx, reply, sizeof(reply));
        return 0;
    }
    case 0x12: {   // LCD: offset, ASCII
        if (n < 1) return 0;
        size_t pos = data[0];
        for (size_t i = 1; i < n && pos < 2 * MK1_MCU_LCD_COLS; i++, pos++) {
            uint8_t ch = data[i];
            m->lcd[pos / MK1_MCU_LCD_COLS][pos % MK1_MCU_LCD_COLS] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
        }
        return MK1_MCU_CHANGED_DISPLAY;
    }
    case 0x62:     // all LEDs off
        memset(m->note, 0, sizeof(m->note));
        memset(m->vpot_ring, 0, sizeof(m->vpot_ring));
        return MK1_MCU_CHANGED_DISPLAY | MK1_MCU_CHANGED_LEDS;
    case 0x63: {   // reset
        mk1_mcu_send_fn send = m->send;
        void *ctx = m->ctx;
        mk1_mcu_init(m, send, ctx);
        return MK1_MCU_CHANGED_DISPLAY | MK1_MCU_CHANGED_LEDS;
    }
    default:
        return 0;
    }
}

unsigned mk1_mcu_host_message(mk1_mcu_t *m, const uint8_t *msg, size_t len)
{
    if (len == 0) return 0;
    if (msg[0] == 0xf0) return handle_sysex(m, msg, len);
    if (len < 2) return 0;

    uint8_t status = msg[0] & 0xf0, ch = msg[0] & 0x0f;
    switch (status) {
    case 0x80:
    case 0x90: {
        if (len < 3) return 0;
        uint8_t note = msg[1] & 0x7f;
        uint8_t vel = status == 0x80 ? 0 : msg[2];
        if (m->note[note] == vel) return 0;
        m->note[note] = vel;
        return MK1_MCU_CHANGED_LEDS | (note < MCU_VPOT_PUSH ? MK1_MCU_CHANGED_DISPLAY : 0);
    }
    case 0xb0: {
        if (len < 3) return 0;
        uint8_t cc = msg[1], v = msg[2];
        if (cc >= MCU_CC_VPOT_RING && cc < MCU_CC_VPOT_RING + MK1_MCU_STRIPS) {
            m->vpot_ring[cc - MCU_CC_VPOT_RING] = v;
            return MK1_MCU_CHANGED_DISPLAY;
        }
        if (cc == 0x4a || cc == 0x4b) {   // assignment display, right-to-left
            char c = (char)(v & 0x3f);
            c = (char)(c < 0x20 ? c + 0x40 : c);
            m->assignment[cc == 0x4b ? 0 : 1] = c;
            return MK1_MCU_CHANGED_DISPLAY;
        }
        return 0;
    }
    case 0xd0: {   // meter: (strip << 4) | level
        unsigned strip = msg[1] >> 4, level = msg[1] & 0x0f;
        if (strip >= MK1_MCU_STRIPS) return 0;
        if (level <= MK1_MCU_METER_MAX) m->meter[strip] = (uint8_t)level;
        else if (level == 0x0e) m->overload[strip] = true;
        else if (level == 0x0f) m->overload[strip] = false;
        return MK1_MCU_CHANGED_DISPLAY;
    }
    case 0xe0: {
        if (len < 3 || ch >= MK1_MCU_FADERS || m->touched[ch]) return 0;
        m->fader[ch] = (uint16_t)((msg[1] & 0x7f) | ((msg[2] & 0x7f) << 7));
        return MK1_MCU_CHANGED_DISPLAY;
    }
    default:
        return 0;
    }
}

unsigned mk1_mcu_tick(mk1_mcu_t *m, uint64_t now_ns)
{
    unsigned changed = 0;

    for (int f = 0; f < MK1_MCU_FADERS; f++) {
        if (m->touched[f] && now_ns >= m->touch_until_ns[f]) {
            m->touched[f] = false;
            send_note(m, (uint8_t)(MCU_FADER_TOUCH + f), false);
        }
    }

    // A real MCU lets meters fall on its own; the DAW only sends new peaks.
    if (now_ns - m->meter_decay_ns >= METER_DECAY_NS) {
        m->meter_decay_ns = now_ns;
        for (int s = 0; s < MK1_MCU_STRIPS; s++) {
            if (m->meter[s]) {
                m->meter[s]--;
                changed |= MK1_MCU_CHANGED_DISPLAY;
            }
        }
    }
    return changed;
}

static int note_level(uint8_t v)
{
    return v == 0 ? 0 : (v == 1 ? 64 : 127);
}

int mk1_mcu_button_led(const mk1_mcu_t *m, mk1_button_t b)
{
    int strip = screen_index(b);
    if (strip >= 0) {
        // Screen buttons show whatever the held modifier would toggle.
        uint8_t base = m->rec_held ? MCU_REC_ARM : m->solo_held ? MCU_SOLO
                     : m->mute_held ? MCU_MUTE : MCU_SELECT;
        return note_level(m->note[base + strip]);
    }
    if (b == MK1_BTN_REC) return note_level(m->note[MCU_RECORD]);
    if (is_mapped(b)) return note_level(m->note[k_map[b].plain]);
    return -1;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

static void draw_ring(mk1_canvas_t *c, int x, int y, uint8_t ring)
{
    int pos = ring & 0x0f, mode = (ring >> 4) & 0x03;
    const uint8_t on = MK1_DISPLAY_MAX_GRAY, off = 4;

    for (int i = 1; i <= 11; i++) {
        bool lit = false;
        if (pos >= 1 && pos <= 11) {
            switch (mode) {
            case 0: lit = i == pos; break;                                    // dot
            case 1: lit = pos < 6 ? (i >= pos && i <= 6) : (i >= 6 && i <= pos); break; // boost/cut
            case 2: lit = i <= pos; break;                                    // wrap
            case 3: lit = i >= 7 - pos && i <= 5 + pos; break;               // spread
            }
        }
        mk1_canvas_fill_rect(c, x + (i - 1) * 5, y, 4, 4, lit ? on : off);
    }
    if (ring & 0x40) mk1_canvas_fill_rect(c, x + 25, y + 5, 4, 2, on);   // center LED
}

static void draw_flag(mk1_canvas_t *c, int x, int y, const char *label, bool active)
{
    const uint8_t on = MK1_DISPLAY_MAX_GRAY;
    if (active) {
        mk1_canvas_fill_rect(c, x, y, 9, 9, on);
        mk1_canvas_text(c, x + 2, y + 1, label, 1, 0);
    } else {
        mk1_canvas_frame_rect(c, x, y, 9, 9, on / 3);
        mk1_canvas_text(c, x + 2, y + 1, label, 1, on / 2);
    }
}

void mk1_mcu_render(const mk1_mcu_t *m, unsigned display, mk1_canvas_t *c)
{
    const uint8_t on = MK1_DISPLAY_MAX_GRAY;
    char text[8];

    mk1_canvas_clear(c, 0);
    for (int col = 0; col < 4; col++) {
        int strip = (int)display * 4 + col;
        int x = col * 64;
        bool selected = m->note[MCU_SELECT + strip] != 0;

        if (col) mk1_canvas_fill_rect(c, x, 0, 1, 64, on / 3);

        // LCD: 7 characters per strip in each of the two rows
        memcpy(text, &m->lcd[0][strip * 7], 7);
        text[7] = '\0';
        if (selected) mk1_canvas_fill_rect(c, x + 1, 0, 63, 9, on);
        mk1_canvas_text(c, x + 3, 1, text, 1, selected ? 0 : on);
        memcpy(text, &m->lcd[1][strip * 7], 7);
        mk1_canvas_text(c, x + 3, 11, text, 1, on);

        draw_ring(c, x + 4, 22, m->vpot_ring[strip]);

        // fader position
        mk1_canvas_frame_rect(c, x + 3, 32, 57, 7, on / 2);
        mk1_canvas_fill_rect(c, x + 5, 34, m->fader[strip] * 53 / 16383, 3, on);

        // meter
        int w = m->meter[strip] * 53 / MK1_MCU_METER_MAX;
        mk1_canvas_fill_rect(c, x + 5, 43, w, 4, on * 2 / 3);
        if (m->overload[strip]) mk1_canvas_fill_rect(c, x + 56, 42, 3, 6, on);

        draw_flag(c, x + 4, 52, "R", m->note[MCU_REC_ARM + strip] != 0);
        draw_flag(c, x + 16, 52, "S", m->note[MCU_SOLO + strip] != 0);
        draw_flag(c, x + 28, 52, "M", m->note[MCU_MUTE + strip] != 0);
    }

    // Two-character assignment display in the bottom-right corner of the right panel.
    if (display == 1) {
        snprintf(text, sizeof(text), "%c%c", m->assignment[0], m->assignment[1]);
        mk1_canvas_text(c, 3 * 64 + 42, 53, text, 1, on);
    }
}

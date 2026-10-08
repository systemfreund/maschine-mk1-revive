// Software canvas for one MK1 display plus the ST7529 packing/chunking logic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mk1_proto.h"

typedef struct {
    uint8_t px[MK1_DISPLAY_HEIGHT][MK1_DISPLAY_WIDTH];   // 0..MK1_DISPLAY_MAX_GRAY
} mk1_canvas_t;

#define MK1_FONT_W 5
#define MK1_FONT_H 7

void mk1_canvas_clear(mk1_canvas_t *c, uint8_t gray);
void mk1_canvas_fill_rect(mk1_canvas_t *c, int x, int y, int w, int h, uint8_t gray);
void mk1_canvas_frame_rect(mk1_canvas_t *c, int x, int y, int w, int h, uint8_t gray);
// Draws ASCII text with the built-in 5x7 font; returns the x after the last glyph.
int  mk1_canvas_text(mk1_canvas_t *c, int x, int y, const char *text, int scale, uint8_t gray);
int  mk1_text_width(const char *text, int scale);

// Pack 3 pixels into 2 bytes per ST7529 column address (5-bit gray each).
void mk1_canvas_pack(const mk1_canvas_t *c, uint8_t fb[MK1_DISPLAY_FB_BYTES]);

// Display init commands (sent once per panel after the device is opened).
typedef struct { uint8_t len; uint8_t bytes[4]; } mk1_display_cmd_t;
extern const mk1_display_cmd_t mk1_display_init_cmds[];
extern const size_t            mk1_display_init_cmd_count;

// Split a framebuffer write into EP8 transfers: address window, then RAMWR +
// pixels in <=508-byte payload chunks. emit() is called once per transfer
// with the complete EP8 packet ([idx, len_hi, len_lo, payload...]).
typedef bool (*mk1_ep8_emit_fn)(void *ctx, const uint8_t *packet, size_t len);
bool mk1_display_send_frame(unsigned display, const uint8_t fb[MK1_DISPLAY_FB_BYTES],
                            mk1_ep8_emit_fn emit, void *ctx);
bool mk1_display_send_cmd(unsigned display, const uint8_t *cmd, size_t len,
                          mk1_ep8_emit_fn emit, void *ctx);

// LED state for the MK1 and the two DIMM_LEDS packets that carry it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mk1_proto.h"

typedef struct {
    uint8_t value[MK1_LED_COUNT];
} mk1_leds_t;

// LED slot for a button, or MK1_LED_UNUSED if the button has no LED.
uint8_t mk1_button_led_slot(mk1_button_t button);

// Snap an arbitrary 0-127 level to the firmware's three brightness tiers.
uint8_t mk1_led_level(unsigned level);

// Build { 0x0c, 0x00, slots[0..30] } and { 0x0c, 0x1e, slots[31..61] }.
void mk1_leds_build_packets(const mk1_leds_t *leds, uint8_t packet_a[33], uint8_t packet_b[33]);

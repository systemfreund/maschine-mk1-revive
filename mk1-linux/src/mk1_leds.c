#include "mk1_leds.h"

#include <string.h>

// Slot numbers follow CABL's MaschineMK1::Led enum.
static const uint8_t k_button_led[MK1_BTN_COUNT] = {
    [MK1_BTN_MUTE] = 16,       [MK1_BTN_SOLO] = 17,
    [MK1_BTN_SELECT] = 18,     [MK1_BTN_DUPLICATE] = 19,
    [MK1_BTN_NAVIGATE] = 20,   [MK1_BTN_PAD_MODE] = 21,
    [MK1_BTN_PATTERN] = 22,    [MK1_BTN_SCENE] = 23,
    [MK1_BTN_SHIFT] = 24,      [MK1_BTN_ERASE] = 25,
    [MK1_BTN_GRID] = 26,       [MK1_BTN_TRANSPORT_RIGHT] = 27,
    [MK1_BTN_REC] = 28,        [MK1_BTN_PLAY] = 29,
    [MK1_BTN_TRANSPORT_LEFT] = 31, [MK1_BTN_RESTART] = 32,
    [MK1_BTN_GROUP_H] = 33,    [MK1_BTN_GROUP_G] = 34,
    [MK1_BTN_GROUP_D] = 35,    [MK1_BTN_GROUP_C] = 36,
    [MK1_BTN_GROUP_F] = 37,    [MK1_BTN_GROUP_E] = 38,
    [MK1_BTN_GROUP_B] = 39,    [MK1_BTN_GROUP_A] = 40,
    [MK1_BTN_AUTO_WRITE] = 41, [MK1_BTN_SNAP] = 42,
    [MK1_BTN_RIGHT] = 43,      [MK1_BTN_LEFT] = 44,
    [MK1_BTN_SAMPLING] = 45,   [MK1_BTN_BROWSE] = 46,
    [MK1_BTN_STEP] = 47,       [MK1_BTN_CONTROL] = 48,
    [MK1_BTN_SCREEN8] = 49,    [MK1_BTN_SCREEN7] = 50,
    [MK1_BTN_SCREEN6] = 51,    [MK1_BTN_SCREEN5] = 52,
    [MK1_BTN_SCREEN4] = 53,    [MK1_BTN_SCREEN3] = 54,
    [MK1_BTN_SCREEN2] = 55,    [MK1_BTN_SCREEN1] = 56,
    [MK1_BTN_NOTE_REPEAT] = 57,
};

uint8_t mk1_button_led_slot(mk1_button_t button)
{
    return (button < MK1_BTN_COUNT) ? k_button_led[button] : MK1_LED_UNUSED;
}

uint8_t mk1_led_level(unsigned level)
{
    if (level == 0) return 0;
    if (level < 43) return MK1_LED_DIM;
    if (level < 86) return MK1_LED_MEDIUM;
    return MK1_LED_BRIGHT;
}

void mk1_leds_build_packets(const mk1_leds_t *leds, uint8_t packet_a[33], uint8_t packet_b[33])
{
    packet_a[0] = MK1_CMD_DIMM_LEDS;
    packet_a[1] = 0x00;
    memcpy(packet_a + 2, leds->value, MK1_LED_BLOCK_A_LEN);

    packet_b[0] = MK1_CMD_DIMM_LEDS;
    packet_b[1] = 0x1e;
    memcpy(packet_b + 2, leds->value + MK1_LED_BLOCK_A_LEN, MK1_LED_COUNT - MK1_LED_BLOCK_A_LEN);
}

// ALSA sequencer ports exposed by the daemon.
#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alsa/asoundlib.h>

typedef enum {
    MK1_PORT_CONTROLLER = 0,   // pads, buttons, encoders; LED/display feedback in
    MK1_PORT_DIN,              // the MK1's 5-pin MIDI In/Out sockets
    MK1_PORT_MACKIE,           // Mackie Control emulation (MK1_MODE=mackie)
    MK1_PORT_COUNT
} mk1_midi_port_t;

typedef struct mk1_midi mk1_midi_t;

// Called for every event an application sends to one of our ports.
typedef void (*mk1_midi_handler_t)(void *ctx, mk1_midi_port_t port, const snd_seq_event_t *ev);

mk1_midi_t *mk1_midi_open(const char *client_name);
void        mk1_midi_close(mk1_midi_t *midi);
int         mk1_midi_client_id(const mk1_midi_t *midi);

int  mk1_midi_poll_fds(mk1_midi_t *midi, struct pollfd *fds, int max);
void mk1_midi_dispatch(mk1_midi_t *midi, mk1_midi_handler_t handler, void *ctx);

// Thread-safe senders (called from the USB event thread).
void mk1_midi_note(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int note, int velocity);
void mk1_midi_poly_aftertouch(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int note, int value);
void mk1_midi_cc(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int cc, int value);
// Raw MIDI bytes (DIN input, Mackie output), parsed into sequencer events.
void mk1_midi_raw(mk1_midi_t *midi, mk1_midi_port_t port, const uint8_t *data, size_t len);

// Converts a sequencer event received on `port` back to raw MIDI bytes;
// returns the byte count. Main thread only.
long mk1_midi_event_to_bytes(mk1_midi_t *midi, mk1_midi_port_t port, const snd_seq_event_t *ev,
                             uint8_t *buf, size_t len);

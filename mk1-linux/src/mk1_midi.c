#include "mk1_midi.h"

#include <pthread.h>
#include <stdlib.h>

#include "mk1_log.h"

struct mk1_midi {
    snd_seq_t        *seq;
    int               port[MK1_PORT_COUNT];
    snd_midi_event_t *din_parser;    // raw bytes -> events (DIN in)
    snd_midi_event_t *din_encoder;   // events -> raw bytes (DIN out)
    pthread_mutex_t   out_lock;
};

static const char *const k_port_names[MK1_PORT_COUNT] = {
    [MK1_PORT_CONTROLLER] = "MK1 Controller",
    [MK1_PORT_DIN]        = "MK1 DIN",
};

mk1_midi_t *mk1_midi_open(const char *client_name)
{
    mk1_midi_t *midi = calloc(1, sizeof(*midi));
    int rc;

    if (!midi) return NULL;
    rc = snd_seq_open(&midi->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (rc < 0) {
        MK1_LOG("cannot open ALSA sequencer: %s", snd_strerror(rc));
        free(midi);
        return NULL;
    }
    snd_seq_set_client_name(midi->seq, client_name);

    for (int p = 0; p < MK1_PORT_COUNT; p++) {
        midi->port[p] = snd_seq_create_simple_port(
            midi->seq, k_port_names[p],
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE |
            SND_SEQ_PORT_TYPE_APPLICATION);
        if (midi->port[p] < 0) {
            MK1_LOG("cannot create port '%s': %s", k_port_names[p], snd_strerror(midi->port[p]));
            mk1_midi_close(midi);
            return NULL;
        }
    }

    if (snd_midi_event_new(256, &midi->din_parser) < 0 ||
        snd_midi_event_new(256, &midi->din_encoder) < 0) {
        mk1_midi_close(midi);
        return NULL;
    }
    snd_midi_event_no_status(midi->din_encoder, 1);   // no running status on the wire
    pthread_mutex_init(&midi->out_lock, NULL);
    return midi;
}

void mk1_midi_close(mk1_midi_t *midi)
{
    if (!midi) return;
    if (midi->din_parser) snd_midi_event_free(midi->din_parser);
    if (midi->din_encoder) snd_midi_event_free(midi->din_encoder);
    if (midi->seq) snd_seq_close(midi->seq);
    pthread_mutex_destroy(&midi->out_lock);
    free(midi);
}

int mk1_midi_client_id(const mk1_midi_t *midi)
{
    return snd_seq_client_id(midi->seq);
}

int mk1_midi_poll_fds(mk1_midi_t *midi, struct pollfd *fds, int max)
{
    return snd_seq_poll_descriptors(midi->seq, fds, (unsigned)max, POLLIN);
}

static mk1_midi_port_t port_index(const mk1_midi_t *midi, int seq_port)
{
    for (int p = 0; p < MK1_PORT_COUNT; p++) {
        if (midi->port[p] == seq_port) return (mk1_midi_port_t)p;
    }
    return MK1_PORT_COUNT;
}

void mk1_midi_dispatch(mk1_midi_t *midi, mk1_midi_handler_t handler, void *ctx)
{
    snd_seq_event_t *ev = NULL;
    int rc;

    while ((rc = snd_seq_event_input(midi->seq, &ev)) >= 0 || rc == -ENOSPC) {
        if (rc == -ENOSPC) {
            MK1_LOG("ALSA input queue overrun");
            continue;
        }
        mk1_midi_port_t port = port_index(midi, ev->dest.port);
        if (port < MK1_PORT_COUNT) {
            handler(ctx, port, ev);
        }
    }
}

static void send_event(mk1_midi_t *midi, mk1_midi_port_t port, snd_seq_event_t *ev)
{
    snd_seq_ev_set_source(ev, midi->port[port]);
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    pthread_mutex_lock(&midi->out_lock);
    snd_seq_event_output_direct(midi->seq, ev);
    pthread_mutex_unlock(&midi->out_lock);
}

void mk1_midi_note(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int note, int velocity)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    if (velocity > 0) {
        snd_seq_ev_set_noteon(&ev, channel, note, velocity);
    } else {
        snd_seq_ev_set_noteoff(&ev, channel, note, 0);
    }
    send_event(midi, port, &ev);
}

void mk1_midi_poly_aftertouch(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int note, int value)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_keypress(&ev, channel, note, value);
    send_event(midi, port, &ev);
}

void mk1_midi_cc(mk1_midi_t *midi, mk1_midi_port_t port, int channel, int cc, int value)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, channel, cc, value);
    send_event(midi, port, &ev);
}

void mk1_midi_raw(mk1_midi_t *midi, mk1_midi_port_t port, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (snd_midi_event_encode_byte(midi->din_parser, data[i], &ev) == 1) {
            send_event(midi, port, &ev);
        }
    }
}

long mk1_midi_event_to_bytes(mk1_midi_t *midi, const snd_seq_event_t *ev, uint8_t *buf, size_t len)
{
    return snd_midi_event_decode(midi->din_encoder, buf, (long)len, ev);
}

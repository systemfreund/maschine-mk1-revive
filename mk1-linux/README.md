# mk1-linux

Userspace driver that makes the Native Instruments **Maschine MK1** usable as a
MIDI controller in Linux DAWs (Bitwig, Reaper, Ardour, Renoise, …), including
LED feedback and the two displays.

```
[DAW] <──ALSA sequencer──> [mk1-linux] <──libusb──> [Maschine MK1]
```

It reuses the protocol work of the macOS bridge in this repository (caiaq init,
LED layout, ST7529 display format, pad/encoder filtering) and replaces IOKit
with libusb and CoreMIDI with the ALSA sequencer. The NIHardwareAgent/IPC part
of the macOS bridge is not needed: no Maschine software is involved.

> **Status: prototype.** It builds, and the protocol logic is unit-tested.
> It has not yet been tested on real hardware under Linux. Please report
> what works and what doesn't.

## What it does

| MK1 control | MIDI (port `MK1 Controller`, default channel 1) |
|---|---|
| 16 pads | Note on/off, notes 36–51 (pad 1 = bottom-left), velocity from the hit strength |
| Pad pressure while held | Polyphonic aftertouch |
| 41 buttons | CC, 127 = pressed, 0 = released (table below) |
| 8 display knobs | CC 70–77 |
| Volume / Tempo / Swing | CC 85 / 86 / 87 |
| DIN MIDI In/Out sockets | passed through on the port `MK1 DIN` |
| Optional: Mackie Control | knobs/buttons as an MCU on the port `MK1 Mackie` (see [Mackie Control mode](#mackie-control-mode)) |

Feedback from the DAW to the `MK1 Controller` port:

- **Note on/off** on a pad note lights that pad's rubber LED (velocity = brightness).
- **CC** on a button's CC number sets that button's LED (0 = off; 1–42 dim,
  43–85 medium, 86–127 bright). The firmware only has those three levels.
- **CC** on a knob's CC number (absolute mode) updates the value shown on the display.
- **SysEx** puts text on a display (see below).

Without feedback, pressed pads and buttons light up while they are held
(`MK1_LOCAL_LEDS=0` turns that off).

The displays show the eight knobs (name, CC, value, bar). The top line of the
right display shows the last event. Once the DAW or a script sends text via
SysEx, that display switches to a free-text page with 8 lines of 42 characters.

### Button CC numbers

| Button | CC | Button | CC | Button | CC |
|---|---|---|---|---|---|
| Mute | 20 | Control | 28 | Restart | 102 |
| Solo | 21 | Step | 29 | Transport < | 103 |
| Select | 22 | Browse | 30 | Transport > | 104 |
| Duplicate | 23 | Sampling | 31 | Grid | 105 |
| Navigate | 24 | Left | 52 | Play | 106 |
| Pad Mode | 25 | Right | 53 | Rec | 107 |
| Pattern | 26 | Snap | 54 | Erase | 108 |
| Scene | 27 | Auto Write | 55 | Shift | 109 |
| Screen 1–8 | 88–95 | Group A–H | 110–117 | Note Repeat | 118 |

### Display SysEx

Manufacturer ID `0x7D` (non-commercial/educational):

```
F0 7D 4D 4B 31 01 <display> <line> <ASCII text…> F7   set one line (display 0 = left, 1 = right; line 0–7)
F0 7D 4D 4B 31 02 <display> F7                         back to the knob page
```

Example with `aseqsend` from alsa-utils (port 0 of the client is `MK1 Controller`):

```sh
aseqsend -p "Maschine MK1:0" F0 7D 4D 4B 31 01 00 00 48 65 6C 6C 6F F7   # "Hello" on the left display
```

## Mackie Control mode

With `MK1_MODE=mackie` the MK1 behaves like a Mackie Control Universal (MCU)
on the port **MK1 Mackie**. DAWs with built-in MCU support (Bitwig, Reaper,
Ardour, Tracktion/Waveform, …) then map the knobs and buttons to the mixer
automatically and show track names, values, pan rings, fader positions and
meters on the MK1 displays: strips 1–4 on the left display, 5–8 on the right.

Pads keep sending notes on `MK1 Controller`. Buttons without an MCU function
(Pattern, Scene, Pad Mode, Navigate, Duplicate, Select, Sampling, Note Repeat)
also stay on `MK1 Controller` as CCs, so they can still be MIDI-learned.

| MK1 | MCU function | With Shift |
|---|---|---|
| Knob 1–8 | V-Pot 1–8 | |
| Volume | Master fader | |
| Swing | Fader of the selected track | |
| Tempo | Jog wheel | |
| Screen button 1–8 | Select track | V-Pot push |
| Mute + Screen 1–8 | Mute track | |
| Solo + Screen 1–8 | Solo track | |
| Rec + Screen 1–8 | Arm track | |
| Rec (on its own) | Record | |
| Play | Play | |
| Restart | Stop | |
| Transport < / > | Rewind / Fast forward | Marker / Nudge |
| Grid | Cycle (loop) | Click |
| Erase | Undo | Save |
| Left / Right (next to the displays) | Bank left / right | Channel left / right |
| Group A–F | Track, Send, Pan, Plugin, EQ, Instrument | F1–F6 |
| Group G / H | Flip / Global view | F7 / F8 |
| Auto Write | Write | Read |
| Snap | Marker | |
| Step | Scrub | Zoom |
| Browse | Enter | |
| Control | Option | |

The screen buttons light up for selected tracks. While Mute, Solo or Rec is
held, they show the mute, solo or arm state of the tracks instead.

Setup:

- **Bitwig**: Settings → Controllers → Add controller → Mackie → Mackie Control.
  Input and output: *MK1 Mackie*.
- **Reaper**: Preferences → Control/OSC/Web → Add → Mackie Control Universal.
  MIDI input and output: *MK1 Mackie*.
- **Ardour**: Window → Preferences → Control Surfaces → Mackie, device type
  *Mackie Control*, then select *MK1 Mackie* as input and output.

Run it with `MK1_MODE=mackie mk1-linux -v`, or put
`Environment=MK1_MODE=mackie` in the systemd drop-in (see Configuration).

## Build

Dependencies: a C compiler, `make`, `pkg-config`, libusb-1.0 and ALSA development files.

```sh
# Debian/Ubuntu
sudo apt install build-essential pkg-config libusb-1.0-0-dev libasound2-dev
# Fedora
sudo dnf install gcc make pkgconf-pkg-config libusb1-devel alsa-lib-devel
# Arch
sudo pacman -S base-devel libusb alsa-lib

make
make test      # protocol unit tests, no hardware needed
```

## Install

```sh
sudo make install                      # /usr/local/bin/mk1-linux, udev rule, systemd user unit
sudo udevadm control --reload-rules && sudo udevadm trigger
# then re-plug the MK1

mk1-linux -v                           # try it in the foreground first
systemctl --user enable --now mk1-linux   # then run it in the background at login
```

The udev rule gives the logged-in user access to the device. Without it you
would have to run `mk1-linux` as root.

In the DAW, enable the MIDI input **Maschine MK1 / MK1 Controller**. Also enable
it as an output if you want LED or display feedback. With PipeWire/JACK, the
port appears through the ALSA sequencer bridge (`a2jmidid` or PipeWire's ALSA
MIDI support).

### The kernel driver `snd-usb-caiaq`

Linux binds the MK1 to `snd-usb-caiaq` by default. `mk1-linux` detaches it
automatically while it runs and hands the device back when it exits. If you
never want the kernel driver, you can blacklist it:

```sh
echo "blacklist snd_usb_caiaq" | sudo tee /etc/modprobe.d/mk1-linux.conf
```

(This also affects other NI caiaq devices such as the Kore, Audio Kontrol 1 and
Traktor Kontrol X1.)

## Configuration

All settings are environment variables. With systemd, put them in a drop-in:
`systemctl --user edit mk1-linux`, then add `[Service]` followed by lines such as
`Environment=MK1_ENCODER_MODE=relative`.

| Variable | Default | Meaning |
|---|---|---|
| `MK1_MIDI_CHANNEL` | `1` | MIDI channel 1–16 |
| `MK1_PAD_BASE_NOTE` | `36` | Note of pad 1 |
| `MK1_AFTERTOUCH` | `1` | Send poly aftertouch for held pads |
| `MK1_FIXED_VELOCITY` | `0` | 1–127 = always this velocity |
| `MK1_VELOCITY_MAX` | `3200` | Pad pressure (0–4095) that gives velocity 127 |
| `MK1_VELOCITY_CURVE` | `0.7` | Exponent; < 1 = more sensitive, > 1 = harder |
| `MK1_ENCODER_MODE` | `absolute` | `absolute` (0–127) or `relative` (64 ± n, "binary offset") |
| `MK1_ENCODER_DIVISOR` | `2` | Raw encoder counts per MIDI step (higher = slower) |
| `MK1_LOCAL_LEDS` | `1` | Light pads/buttons while pressed |
| `MK1_BACKLIGHT` | `92` | Display backlight level (0 = off) |
| `MK1_MODE` | `midi` | `midi` (plain CCs) or `mackie` (Mackie Control, see above) |
| `MK1_DISPLAY_FPS` | `20` | Maximum display refresh rate |
| `MK1_PAD_HIT_ON` | `300` | Pad pressure that starts a hit |
| `MK1_PAD_HIT_OFF` | `150` | Pad pressure below which a pad is released |
| `MK1_PAD_PRESSURE` | `200` | Minimum pressure change for an aftertouch update |
| `MK1_PAD_DEBOUNCE_MS` | `10` | Quiet time after release before a pad can hit again |
| `MK1_PAD_SUSTAIN` | `3` | Reports above `HIT_ON` before a hit fires (filters bounce) |
| `MK1_ENCODER_MIN_DELTA` | `2` | Encoder changes below this are treated as jitter |

The pad and encoder filters use the same defaults as the macOS bridge.

Relative mode: in Bitwig choose "Relative (Bin Offset)", in Reaper "Relative 2".
Ardour's Generic MIDI binding calls it `enc-b`.

## Code layout

| File | Purpose |
|---|---|
| `src/mk1_proto.h` | Endpoints, commands, button/encoder IDs, LED and display constants |
| `src/mk1_input.c` | Decodes pads (baseline, sustain gate, debounce), buttons and encoders. No I/O. |
| `src/mk1_leds.c` | LED slot layout (CABL order) and the two `DIMM_LEDS` packets |
| `src/mk1_mackie.c` | Mackie Control layer: button/knob → MCU messages, DAW state (LCD, LEDs, rings, faders, meters), display page |
| `src/mk1_display.c` | Canvas, 5×7 font, ST7529 3-pixels-per-2-bytes packing, EP8 chunking, init sequence |
| `src/mk1_usb.c` | libusb: open/claim, async EP1/EP4 readers, writes, caiaq/display init |
| `src/mk1_midi.c` | ALSA sequencer client with the `MK1 Controller`, `MK1 DIN` and `MK1 Mackie` ports |
| `src/main.c` | MIDI mapping, feedback, display pages, hotplug loop |
| `tests/test_protocol.c` | Unit tests for input decoding, LED packets, display framing and the Mackie layer |

## Troubleshooting

- **`cannot open MK1: LIBUSB_ERROR_ACCESS`**: the udev rule is missing or the
  device wasn't re-plugged after installing it.
- **`cannot claim interface: LIBUSB_ERROR_BUSY`**: another program (or a second
  `mk1-linux`) holds the device.
- **Pads trigger too easily or too hard**: adjust `MK1_PAD_HIT_ON`,
  `MK1_VELOCITY_MAX` and `MK1_VELOCITY_CURVE`. `mk1-linux -v` logs the peak
  pressure and resulting velocity of every hit.
- **Knobs too fast or too slow**: `MK1_ENCODER_DIVISOR`.

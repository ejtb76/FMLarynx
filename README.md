# FMLarynx

**A formant synthesizer firmware for the M-VAVE FM-1.**

FMLarynx turns the FM-1 into a monophonic "talking" synth: a sawtooth is shaped by a filter modelled on the human
vocal tract, so each note sounds like a sung vowel. You glide between vowels with two encoders, wobble them with two
LFOs, and play it from the keys, over USB-MIDI, or from a built-in 303-style 16-step sequencer with slides.

It is a port of FormantSynth, a VST3 plugin, to the FM-1's chip, and is built on
[Dinghy](https://github.com/zvenson/dinghy) by zvenson.

> **Status: prototype, runs on the FM-1.** Tested on the device: the sound engine, all knobs, both LFOs, the
> sequencer and its LEDs, the pattern kept in flash. The package identity is still
> Dinghy's (`FM-1_990`); its own comes before the first release.

## The sound

One voice, three formants:

- **Source:** a sawtooth, harmonically rich, with a short attack (~5 ms) and release (~45 ms).
- **Filter:** three resonant band-pass filters in parallel, tuned to the first three formants (F1, F2, F3) of a vowel.
- **Vowel plane:** the five vowels **i, e, a, o, u** sit on an X/Y plane. Anywhere between them, the formant
  frequencies blend smoothly, so turning X and Y morphs one vowel into the next.

  ```
  Y
  1.0          a
  0.5    e           o
  0.0    i           u
        0.0   0.5   1.0  X
  ```

- **Voice:** moves the formants from an adult male vocal tract to a female/child one.
- **Size:** scales all formants up or down (0.5x to 2x), from a giant's throat to a tiny one.
- **Resonance:** how narrow and ringing the formants are. Output is compensated so the volume stays roughly level as
  you turn it up.

Everything runs in integer maths (the chip has no FPU); the audio routine uses well under its time budget.

## Controls

No menus: every parameter has its own encoder, and the **LFO** button turns knobs 1 to 4 into the LFO controls for
as long as you hold it.

### Encoders

| Control | Normally | While holding **LFO** | Range |
| --- | --- | --- | --- |
| **KNOB 1** | Size | LFO X rate | 500 to 2000 (0.5x to 2x) / 0 to 1000 (0 to 8 Hz) |
| **KNOB 2** | Resonance | LFO X depth | 10 to 500 (Q 1 to 50) / 0 to 1000 |
| **KNOB 3** | Voice (male to female/child) | LFO Y rate | 0 to 1000 / 0 to 1000 (0 to 8 Hz) |
| **KNOB 4** | Level | LFO Y depth | 0 to 1000 / 0 to 1000 |
| **ALGORITHM** | Vowel X | | 0 to 1000 |
| **SELECT** | Vowel Y (see the sequencer for its other jobs) | | 0 to 1000 |
| **PRESETS** | Jump to a vowel: i, e, a, o, u | | |
| **MASTER** | Volume | | |

LFO X moves the vowel along X, LFO Y along Y, each around where the encoders put it. The screen shows the name and
value of the last control you turned.

### Buttons and keys

| Control | What it does |
| --- | --- |
| **Keys** | Play the voice (27 keys, F3 to G5 at octave 0). Monophonic: the last note played wins. |
| **OCT- / OCT+** | Octave, -2 to +2. The button lights when you're off centre. |
| **LFO** | Hold to set the LFOs with knobs 1 to 4. Glows softly as a reminder, lights up while held. |
| **REC** | Sequencer record mode on/off. |
| **PLAY** | Start/stop the sequencer. |
| **GLO** | Hold while recording a step to make it a slide. |
| **SEQ** | Hold and turn **SELECT** to set the tempo. |

## Sequencer

A 16-step pattern sequencer in the spirit of the TB-303, so both hands are free for the knobs while it plays.
Each step is a 16th note.

**Recording a pattern**

1. Press **REC**. Playback stops and the cursor goes to step 1. The 16 white keys now show the 16 steps.
2. Turn **SELECT** to move the cursor to a step.
3. Press a key: its note goes into that step and the cursor moves on to the next one. Hold **GLO** while pressing
   the key to make that step a slide.
4. Press **REC** again to leave record mode.

You hear every note as you enter it. A step without a note is a rest. To change a step, move the cursor to it and
record over it.

**Slides:** a slide step changes pitch without restarting the envelope, so it glides legato out of the step before
it. Other steps retrigger the note.

**The lights in record mode**

| White key | Means |
| --- | --- |
| Off | Empty step (a rest) |
| Soft | The cursor |
| Medium | A step with a note in it |
| Full | The note sounding right now |
| **GLO** button full | The step under the cursor is a slide (it glows softly otherwise) |

**Playing:** press **PLAY**. Hold **SEQ** and turn **SELECT** to set the tempo (40 to 300 BPM, 120 by default).

**MIDI clock:** when a DAW or another device sends MIDI clock over USB, the sequencer follows it, and MIDI
start, stop and continue start and stop it. If the clock stops for half a second, it carries on at its own tempo.

**Saved:** the pattern, the tempo and the octave are kept in flash and come back after power-off. They are saved two
seconds after your last change, once nothing is sounding, because writing to flash briefly interrupts the sound. Stop
the sequencer for a moment before you switch off, so your last changes are saved.

## MIDI and audio over USB

- **MIDI in:** notes on any channel play the voice, with velocity. MIDI clock drives the sequencer.
- **MIDI out:** notes from the keys and from the sequencer.
- **Audio:** the FM-1 shows up as a 16-bit stereo audio input (44.1 or 48 kHz), so you can record it straight into
  a DAW.

## Installing

Installing custom firmware is at your own risk. You can always go back to the official firmware or another one with
the [FM-1 Firmware Hub](https://fm1.designburgapps.com) in Chrome or Edge.

Build it, then install over USB:

```sh
./build.sh                                       # -> build/fmlarynx.fwsc
python3 tools/fm1_install.py build/fmlarynx.fwsc   # asks before it writes, then restarts into the new build
python3 tools/fm1_install.py --info              # what runs on the connected FM-1
```

The build needs Linux on x86-64 (or Windows with WSL, or macOS with Docker), Python 3 with Pillow, the JieLi
toolchain and three files of the JieLi AC79 SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                           # JieLi's clang for the chip, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

`tests/run_host.sh` runs the same firmware on your computer, writing the sound to a WAV and the screen to images.

**If something goes wrong:** hold **OCT-** while switching on to reach the USB rescue mode (no sound, only the
updater), and install a working build from there. If the firmware crashes twice at boot, the FM-1 goes there by
itself.

## For developers

The whole synth is in [firmware/src/dinghy.c](firmware/src/dinghy.c). Everything underneath it (boot, USB, updates,
the audio interrupt, settings) is Dinghy's platform: see [docs/API.md](docs/API.md) for what it offers and
[docs/RULES.md](docs/RULES.md) for the chip's rules (no FPU, no 64-bit division, the audio time budget), which the
build checks for you.

## License and credits

GPL-3.0-only: you may change and share it, and whoever gets a firmware built from it gets its source too. Details
and third-party parts in [LICENSING.md](LICENSING.md).

- **FMLarynx** (the formant synth, sequencer and controls in `dinghy.c`): Erik Jan ten Brinke
- **[Dinghy](https://github.com/zvenson/dinghy)** (the app skeleton, the wiring `platform.c`, host mode, docs): zvenson
- **The platform:** Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
  ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1)
  (isod89)

"M-VAVE" and "FM-1" belong to their owners. FMLarynx is not made or endorsed by them.

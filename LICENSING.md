# Licensing

FMLarynx is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

It is built on **fm1-base** (Charles Vestal) and **felucca-bsp** (in `base/`, a git submodule), which are under the
Mozilla Public License 2.0, file by file: their files stay MPL-2.0 inside this GPL-3.0 firmware. Much of them comes
from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (@kurogedelic), Hügelton Instruments, with his
permission ([felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4)). Their own `LICENSING.md` and
`CREDITS.md` list their parts and their third-party code.

Bluetooth MIDI, when built in, is fm1-base's own stack (`ble_stack = "own"`): nothing of JieLi's closed Bluetooth
libraries is linked, so the firmware stays GPL-3.0.

FMLarynx 0.1 to 0.3 were built on [Dinghy](https://github.com/zvenson/dinghy) (GPL-3.0-only); those versions stay
under their licence, in this repository's history.

## Who made what

| | |
| --- | --- |
| `base/` (fm1-base, with felucca-bsp in `base/bsp/`): the platform (boot, the audio interrupt, USB, MIDI, the screen and fonts, the panel, storage, updates, safe mode, the screen reader, Bluetooth), the host simulator, the browser emulator, the build and install tools | Charles Vestal, Leo Kuroshita (Hügelton Instruments) and their contributors; MPL-2.0 (see `base/LICENSING.md`) |
| `app/app.c`: the formant synth, sequencer, screen and effects | Presethead; it grew from the app skeleton of [Dinghy](https://github.com/zvenson/dinghy) by zvenson (GPL-3.0-only) |
| `app/larynx_mul.h`, `app/fm1.toml`, `app/words.tsv`, `app/tests/`, `build.sh`, `tests/`, the docs | Presethead |
| `app/app.h` | from fm1-base's template (Charles Vestal, MPL-2.0), with FMLarynx's changes (Presethead); stays MPL-2.0 |
| The formant filter's design | follows [MouthTowel](https://github.com/nickb808-dev/er301-MouthTowel) by nickb808-dev (the design, not its code) |

What the move to fm1-base dropped (fm1-base and felucca-bsp have their own): Dinghy's platform (`firmware/hal/`,
`firmware/loader/`, `crt0.S`, `app.ld`, and in `firmware/src/` `platform.c`, `usb.c`, `ota.c`, `recovery.c`, `lcd.c`,
`gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), its tools (`tools/`), its host mode (`tests/host.c`), its docs
(`docs/API.md`, `MAKE-IT-YOURS.md`, `RULES.md`) and the Dinghy logo.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font, rendered by fm1-base's `tools/gen_font.py` at build time | SIL OFL 1.1 | `base/assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: the files the package needs are read from your SDK checkout at build time; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. FMLarynx is not made or endorsed by them.

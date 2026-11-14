#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build FMLarynx with fm1-base (base/, a submodule: git submodule update --init --recursive).
#   ./build.sh                    app/build/fmlarynx.fwsc (the firmware package)
#   ./build.sh --release X.Y      app/build/fmlarynx-X.Y.fwsc
#   ./build.sh --host             the host simulator (app/build/host/fmlarynx_host)
#   ./build.sh --web              the browser emulator
# JIELI_TOOLCHAIN, AC79_SDK and PYTHON override the defaults (~/.jieli/toolchain, ~/fw-AC79_AIoT_SDK, python3).
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
export X0X_FONTSET=terminus                    # Terminus, as FMLarynx had on Dinghy (fm1-base's tools/gen_font.py)
PY="${PYTHON:-python3}"
[ -f base/tools/build.py ] || { echo "build.sh: no fm1-base in base/ (git submodule update --init --recursive)"; exit 1; }
mkdir -p build
"$PY" -I tools/make_splash.py assets/splash.png build/splash.inc   # the splash (tools/splash_art.py draws it)
exec "$PY" base/tools/build.py app "$@"

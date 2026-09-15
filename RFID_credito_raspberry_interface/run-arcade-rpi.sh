#!/bin/sh
# Mesa no Raspberry pode escolher um driver GL instavel. Esta flag usa llvmpipe/CPU.
set -eu
cd "$(dirname "$0")"
[ -x ./RFID_arcade ] || sh ./build-arcade.sh rpi
exec env LIBGL_ALWAYS_SOFTWARE=1 ./RFID_arcade

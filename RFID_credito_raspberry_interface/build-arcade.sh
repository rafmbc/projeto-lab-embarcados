#!/bin/sh
# Uso: sh build-arcade.sh demo | sh build-arcade.sh rpi
set -eu
cd "$(dirname "$0")"
mode=${1:-demo}
common='-std=c11 -Wall -Wextra -Wpedantic -I include -lraylib -lm -lpthread -ldl'
case "$mode" in
  demo) gcc arcade.c -DRFID_DUMMY $common -o RFID_arcade ;;
  rpi)  gcc src/*/*.c arcade.c $common -lwiringPi -o RFID_arcade ;;
  *) echo "Uso: $0 [demo|rpi]" >&2; exit 2 ;;
esac

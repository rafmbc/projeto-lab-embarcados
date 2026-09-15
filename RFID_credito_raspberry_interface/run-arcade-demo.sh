#!/bin/sh
set -eu
cd "$(dirname "$0")"
[ -x ./RFID_arcade ] || sh ./build-arcade.sh demo
exec ./RFID_arcade

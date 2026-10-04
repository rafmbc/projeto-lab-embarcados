#!/bin/sh
set -eu
cd "$(dirname "$0")"
common='-std=c11 -Wall -Wextra -Wpedantic -I include -lraylib -lm -lpthread -ldl'
gcc src/*/*.c arcade.c $common -lwiringPi -o RFID_arcade

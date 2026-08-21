#!/bin/bash
# SuperStation One / Retro Remake -- animated intro screensaver launcher
#
# Fills the entire screen with the SuperStation One / Retro Remake boot
# intro animation, scaled to fit (letterboxed if needed), on a black
# background. Returns to whatever was running before (Console Mode, the
# MiSTer menu, etc.) the moment you press any key or controller button.
#
# This does NOT modify Console Mode, SuperStation One, or any of their
# files -- it's a standalone program that only touches /dev/fb0 while
# it's running.
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/intro_screensaver"
CONF="$DIR/ssone_screensaver.conf"

ASPECT="4x3"
[ -f "$CONF" ] && . "$CONF"

if [ "$ASPECT" = "16x9" ]; then
    ASSET="$DIR/intro_frames_169.bin"
else
    ASSET="$DIR/intro_frames_4x3.bin"
fi

if [ ! -x "$BIN" ]; then
    echo "intro_screensaver binary not found or not executable at: $BIN"
    exit 1
fi
if [ ! -f "$ASSET" ]; then
    echo "frame data not found at: $ASSET"
    exit 1
fi

exec "$BIN" "$ASSET"

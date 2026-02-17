#!/bin/sh
# SHORTCUT.pak — Bridge emulator for tool shortcuts.
# Receives path to a marker file, reads the real tool path, execs it.
TARGET=$(cat "$1")
if [ -x "$TARGET/launch.sh" ]; then
    exec "$TARGET/launch.sh"
fi

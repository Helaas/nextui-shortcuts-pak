#!/bin/sh
# SHORTCUT.pak — Bridge emulator for tool shortcuts.
# Old format: $1 is the target file containing the real tool path.
# New format: $1 is the tool's .pak directory path (NextUI resolves it
#             from the .m3u, which now contains the relative tool path
#             so the Game Tracker records the real tool name).
if [ -f "$1" ]; then
    TARGET=$(cat "$1")
elif [ -d "$1" ]; then
    TARGET="$1"
else
    exit 1
fi
cd "$TARGET" || exit 1
TARGET=$(pwd -P)
cd "$TARGET" || exit 1
if [ -x ./launch.sh ]; then
    exec ./launch.sh
fi

#!/usr/bin/env bash
# Launch the Threading Model Visualiser on Linux / macOS.
# Optionally set QT_DIR to a custom Qt kit (the dir containing lib/), e.g.
#   QT_DIR=~/Qt/6.9.2/gcc_64 ./run.sh
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$DIR/build"

# Add a user-specified Qt to the loader path (harmless if Qt is system-wide).
if [ -n "${QT_DIR:-}" ]; then
    export LD_LIBRARY_PATH="$QT_DIR/lib:${LD_LIBRARY_PATH:-}"     # Linux
    export DYLD_LIBRARY_PATH="$QT_DIR/lib:${DYLD_LIBRARY_PATH:-}" # macOS
fi

# macOS produces an .app bundle; Linux a plain binary.
if [ -d "$BUILD/ThreadingViz.app" ]; then
    exec open "$BUILD/ThreadingViz.app"
elif [ -x "$BUILD/ThreadingViz" ]; then
    exec "$BUILD/ThreadingViz" "$@"
else
    echo "ThreadingViz not built yet. Build it first:" >&2
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=\"<your-qt-kit-dir>\"" >&2
    echo "  cmake --build build" >&2
    exit 1
fi

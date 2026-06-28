#!/usr/bin/env bash
# Launch the Threading Model Visualiser on Linux / macOS.
#
# If Qt is installed in a standard location (system package, or already on your
# library path) you don't need to set anything. Otherwise point QT_DIR at your
# Qt kit so its libraries are found at runtime, e.g.
#   QT_DIR=~/Qt/6.9.2/gcc_64    ./run.sh    # Linux
#   QT_DIR=~/Qt/6.9.2/macos     ./run.sh    # macOS
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -n "$QT_DIR" ]; then
  export LD_LIBRARY_PATH="$QT_DIR/lib:$LD_LIBRARY_PATH"      # Linux
  export DYLD_LIBRARY_PATH="$QT_DIR/lib:$DYLD_LIBRARY_PATH"  # macOS
fi

if [ -d "$here/build/ThreadingViz.app" ]; then
  open "$here/build/ThreadingViz.app"          # macOS .app bundle
else
  exec "$here/build/ThreadingViz"              # Linux binary
fi

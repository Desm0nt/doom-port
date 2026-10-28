#!/bin/bash
# DOOM Mobile: prepare the game files from your own dumps in one command (all the work is in tools/setup_doom.py,
# which also runs directly on Windows: python tools/setup_doom.py ...).
#
#   ./setup.sh check                                              what is installed / prepared, and the next step (changes nothing)
#   ./setup.sh <base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys> [--push <adb serial>] [--dry-run] [--work DIR]
#   ./setup.sh push <adb serial>                                  copy the prepared folder to the phone (resumes)
#
# Supported: DOOM (2016) for Nintendo Switch, base 0100416004C00000 + update 0100416004C00800 v262144.
# Packages and keys are only read; nothing leaves your machine except what --push copies to your own phone.
# Output: <work>/DOOM Mobile/{romfs, dm/out, dm/shaders} = /sdcard/Download/DOOM Mobile on the phone.
# See docs/doom/GAME_FILES.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-}
if [ -z "$PY" ]; then   # first interpreter that actually runs Python 3 (Windows has a python3 store stub)
  for c in python3 python py; do command -v "$c" > /dev/null 2>&1 && "$c" -c "import sys; sys.exit(sys.version_info[0] != 3)" > /dev/null 2>&1 && { PY=$c; break; }; done
fi
[ -n "$PY" ] || { echo "Python 3 is required (set PYTHON=...)" >&2; exit 1; }
exec "$PY" "$HERE/tools/setup_doom.py" "$@"

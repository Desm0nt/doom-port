#!/bin/bash
# DOOM Mobile setup for Linux / macOS: prepares the game folder for the phone from your own DOOM (Switch) dumps.
#   ./setup.sh                  finds the base game, the update v262144 and prod.keys in put-game-files-here/ (or the
#                               current folder), under any names, and prepares everything
#   ./setup.sh FILES...         the same with explicit files (NSP / NSZ / XCI / XCZ + prod.keys), in any order
#   ./setup.sh check            what is installed / found / prepared, and the next step (changes nothing)
#   ./setup.sh push [serial]    copy the prepared folder to the phone over adb
#   more options: ./setup.sh --help
# Python packages go into a private virtual environment (tools/.runtime/venv), the .NET 10 SDK into tools/.runtime/dotnet
# when it is missing (asked first; DOOM_SETUP_YES=1 answers yes). Nothing is installed system-wide.
# All the work is done by tools/setup_doom.py (it also runs directly: python3 tools/setup_doom.py ...); guide: docs/INSTALL.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RT="$HERE/tools/.runtime"
VENV="$RT/venv"
PKGS=(cryptography zstandard lz4 'capstone<6')
NEED_VER='import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
NEED_ALL='import sys; assert sys.version_info >= (3, 10); import cryptography, zstandard, lz4, capstone'

venv_py() { if [ -x "$VENV/bin/python" ]; then echo "$VENV/bin/python"; else echo "$VENV/Scripts/python.exe"; fi; }   # Scripts/: Git Bash

ask() {
  if [ "${DOOM_SETUP_YES:-}" = 1 ]; then echo "$1 [Y/n] y (DOOM_SETUP_YES=1)"; return 0; fi
  local a
  if ! { read -r -p "$1 [Y/n] " a < /dev/tty; } 2> /dev/null; then echo "$1 [Y/n] -> no (no terminal; DOOM_SETUP_YES=1 accepts)"; return 1; fi
  case "$a" in ''|y|Y|yes|Yes|YES) return 0;; *) return 1;; esac
}

hint() {   # how to get Python 3.10+ with venv on this system
  echo "Install Python 3.10 or newer with its venv module, then run ./setup.sh again:" >&2
  if [ "$(uname -s)" = Darwin ]; then
    echo "  brew install python@3.12        (or the installer from https://www.python.org/downloads/macos/)" >&2
  elif command -v apt-get > /dev/null 2>&1; then echo "  sudo apt install python3 python3-venv python3-pip" >&2
  elif command -v dnf > /dev/null 2>&1; then echo "  sudo dnf install python3 python3-pip" >&2
  elif command -v pacman > /dev/null 2>&1; then echo "  sudo pacman -S python python-pip" >&2
  elif command -v zypper > /dev/null 2>&1; then echo "  sudo zypper install python3 python3-pip" >&2
  else echo "  use your system's package manager, or https://www.python.org/downloads/" >&2
  fi
  echo "  (or point PYTHON=/path/to/python3 at one you already have)" >&2
}

find_python() {   # first interpreter that runs Python 3.10+ (Windows has a python3 store stub that does not)
  local c
  for c in ${PYTHON:-} python3.13 python3.12 python3.11 python3.10 python3 python; do
    command -v "$c" > /dev/null 2>&1 && "$c" -c "$NEED_VER" > /dev/null 2>&1 && { echo "$c"; return 0; }
  done
  return 1
}

VPY=$(venv_py)
if "$VPY" -c "$NEED_ALL" > /dev/null 2>&1; then exec "$VPY" "$HERE/tools/setup_doom.py" "$@"; fi

case "${1:-}" in
  check|help|-h|--help)   # report with whatever Python there is; setup itself adds what is missing
    PY=$(find_python) || { echo "== Python 3.10+: not found" >&2; hint; exit 1; }
    echo "== Python: $(command -v "$PY") (setup will add the missing packages in tools/.runtime/venv)"
    exec "$PY" "$HERE/tools/setup_doom.py" "$@";;
esac

echo "== Preparing Python for setup (only the first time)"
PY=$(find_python) || { hint; exit 1; }
echo "   found Python: $(command -v "$PY") ($("$PY" -c 'import sys; print(sys.version.split()[0])'))"
if ! "$VPY" -c "$NEED_VER" > /dev/null 2>&1; then
  rm -rf "$VENV"
  echo "   creating a virtual environment in $VENV"
  if ! "$PY" -m venv "$VENV" || ! "$(venv_py)" -m pip --version > /dev/null 2>&1; then
    rm -rf "$VENV"
    echo "The Python venv module (with pip) is missing." >&2; hint; exit 1
  fi
  VPY=$(venv_py)
fi
echo "   Setup needs these Python packages from PyPI (~15 MB): ${PKGS[*]}"
echo "   They go into $VENV only."
ask "   Install them now?" || { echo "Setup cannot run without them; run ./setup.sh again and answer Y." >&2; exit 1; }
"$VPY" -m pip install --disable-pip-version-check "${PKGS[@]}" || { echo "pip could not install the packages (see above); check the internet connection and run ./setup.sh again." >&2; exit 1; }
"$VPY" -c "$NEED_ALL" || exit 1
exec "$VPY" "$HERE/tools/setup_doom.py" "$@"

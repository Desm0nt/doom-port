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
# Started from a file manager (double-click), setup opens a terminal window and keeps it open at the end ("Press Enter");
# DOOM_SETUP_PAUSE=0 turns that off, DOOM_SETUP_PAUSE=1 forces the pause. DOOM_SETUP_YES=1 and CI never pause.
#
# The next two lines run under sh / dash / zsh too and end with '#' so that Windows line endings (CRLF) do not break them:
# they restart the script with bash, and remove CRLF from this file (a ZIP copied through a Windows editor or tool).
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@" #
if [ -f "$0" ] && grep -q "$(printf '\r')" "$0" 2> /dev/null; then echo "setup.sh: removing Windows line endings (CRLF) from $0" >&2; tr -d '\r' < "$0" > "$0.lf.tmp" && cat "$0.lf.tmp" > "$0"; rm -f "$0.lf.tmp"; if grep -q "$(printf '\r')" "$0"; then echo "STOP: could not fix $0; run:  sed -i 's/\r\$//' setup.sh   and start it again" >&2; exit 1; fi; exec bash "$0" "$@"; fi #
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RT="$HERE/tools/.runtime"
VENV="$RT/venv"
PKGS=(cryptography zstandard lz4 'capstone<6')
NEED_VER='import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
NEED_ALL='import sys; assert sys.version_info >= (3, 10); import cryptography, zstandard, lz4, capstone'

venv_py() {   # Scripts/python.exe only for Git Bash / MSYS (under WSL a venv made by setup.bat must not be used)
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) if [ -x "$VENV/Scripts/python.exe" ]; then echo "$VENV/Scripts/python.exe"; return; fi;;
  esac
  echo "$VENV/bin/python"
}

# ---------------------------------------------------------------- terminal window: open one, keep it open
parent_name() {   # name of the process that started this script (bash, zsh, konsole, dolphin, ...)
  local n=
  [ -r "/proc/$PPID/comm" ] && n=$(cat "/proc/$PPID/comm" 2> /dev/null)
  [ -n "$n" ] || n=$(ps -o comm= -p "$PPID" 2> /dev/null)
  [ -n "$n" ] || n=$(ps -p "$PPID" 2> /dev/null | awk 'NR == 2 { print $NF }')   # Git Bash / MSYS ps
  n=${n##*/}; n=${n#-}; echo "${n%.exe}"
}

from_shell() {   # typed into a shell (or sudo / tmux / ...), not started by a file manager or a terminal emulator
  case "$(parent_name)" in
    bash|sh|dash|zsh|fish|ksh|ksh93|mksh|oksh|pdksh|yash|ash|busybox|tcsh|csh|nu|xonsh|elvish|pwsh|powershell|\
    sudo|doas|su|tmux*|screen|script|make|xargs|env|nohup|timeout|time|watch|strace|python*) return 0;;
  esac
  return 1
}

PAUSE=0
case "${DOOM_SETUP_PAUSE:-}" in
  1) PAUSE=1;;
  0) ;;
  *) if [ "${DOOM_SETUP_YES:-}" != 1 ] && [ -z "${CI:-}" ] && [ -t 0 ] && [ -t 1 ] && ! from_shell; then PAUSE=1; fi;;
esac

finish() {   # exit with code $1; a window opened by a double-click stays until Enter
  if [ "$PAUSE" = 1 ]; then
    echo
    [ "$1" = 0 ] || echo "Setup did not finish (exit code $1): read the message above." >&2
    read -r -p "Press Enter to close this window. " _ || true
  fi
  exit "$1"
}

open_terminal() {   # no terminal at all (a file manager's "Run" / "Execute"): show setup in a terminal window instead
  local t rc mark="${TMPDIR:-/tmp}/doom-setup-started.$$"   # the copy in the terminal creates it: started, whatever its exit code
  local cmd=(env DOOM_SETUP_IN_TERMINAL="$mark" DOOM_SETUP_PAUSE=1 bash "$HERE/setup.sh" "$@")
  rm -f "$mark"
  for t in ${TERMINAL:-} x-terminal-emulator konsole gnome-terminal xfce4-terminal mate-terminal terminator ptyxis kgx \
           alacritty kitty foot wezterm xterm; do
    command -v "$t" > /dev/null 2>&1 || continue
    case "$t" in
      gnome-terminal|ptyxis|kgx) "$t" -- "${cmd[@]}";;
      xfce4-terminal|mate-terminal|terminator) "$t" -x "${cmd[@]}";;
      kitty|foot) "$t" "${cmd[@]}";;
      wezterm) "$t" start -- "${cmd[@]}";;
      *) "$t" -e "${cmd[@]}";;
    esac > /dev/null 2>&1
    rc=$?
    if [ "$rc" = 0 ] || [ -e "$mark" ]; then rm -f "$mark"; exit 0; fi
  done
}
case "${DOOM_SETUP_IN_TERMINAL:-}" in /*) : > "$DOOM_SETUP_IN_TERMINAL" 2> /dev/null;; esac

if [ ! -t 0 ] && [ ! -t 1 ] && [ -z "${DOOM_SETUP_IN_TERMINAL:-}" ] && [ "${DOOM_SETUP_PAUSE:-}" != 0 ] \
   && [ "${DOOM_SETUP_YES:-}" != 1 ] && [ -z "${CI:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; } \
   && ! from_shell; then
  open_terminal "$@"   # returns only when no terminal emulator could be started: continue without one
fi

run_setup() {   # $1 = Python; the rest goes to tools/setup_doom.py
  local py=$1; shift
  if [ "$PAUSE" = 1 ]; then "$py" "$HERE/tools/setup_doom.py" "$@"; finish $?; fi
  exec "$py" "$HERE/tools/setup_doom.py" "$@"
}

# ---------------------------------------------------------------- Python
ask() {
  if [ "${DOOM_SETUP_YES:-}" = 1 ]; then echo "$1 [Y/n] y (DOOM_SETUP_YES=1)"; return 0; fi
  local a
  if ! { read -r -p "$1 [Y/n] " a < /dev/tty; } 2> /dev/null; then echo "$1 [Y/n] -> no (no terminal; DOOM_SETUP_YES=1 accepts)"; return 1; fi
  case "$a" in ''|y|Y|yes|Yes|YES) return 0;; *) return 1;; esac
}

py_version() { "$1" -c 'import sys; print(sys.version.split()[0])' 2> /dev/null; }

hint() {   # how to get Python 3.10+ with venv on this system
  local v
  echo "Install Python 3.10 or newer with its venv module, then run ./setup.sh again:" >&2
  if [ "$(uname -s)" = Darwin ]; then
    echo "  brew install python@3.12        (or the installer from https://www.python.org/downloads/macos/)" >&2
  elif command -v apt-get > /dev/null 2>&1; then
    v=$(${PY:-python3} -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2> /dev/null)
    echo "  sudo apt install python3 python3-venv python3-pip${v:+   (for Python $v: python$v-venv)}" >&2
  elif command -v dnf > /dev/null 2>&1; then echo "  sudo dnf install python3 python3-pip" >&2
  elif command -v pacman > /dev/null 2>&1; then echo "  sudo pacman -S python python-pip" >&2
  elif command -v zypper > /dev/null 2>&1; then echo "  sudo zypper install python3 python3-pip" >&2
  else echo "  use your system's package manager, or https://www.python.org/downloads/" >&2
  fi
  echo "  (or point PYTHON=/path/to/python3 at one you already have)" >&2
}

PY= OLD=
find_python() {   # sets PY: the first Python 3.10+, one with venv + ensurepip preferred (Windows' python3 store stub fails)
  local c v first=
  for c in ${PYTHON:-} python3.13 python3.12 python3.11 python3.10 python3 python; do
    command -v "$c" > /dev/null 2>&1 || continue
    if ! "$c" -c "$NEED_VER" > /dev/null 2>&1; then
      v=$(py_version "$c"); [ -z "$v" ] || case "$OLD" in *" $c $v,"*) ;; *) OLD="$OLD $c $v,";; esac
      continue
    fi
    if [ "$c" = "${PYTHON:-}" ] || "$c" -c 'import venv, ensurepip' > /dev/null 2>&1; then PY=$c; return 0; fi
    [ -n "$first" ] || first=$c
  done
  PY=$first; [ -n "$PY" ]
}

no_python() {
  echo "STOP: Python 3.10 or newer was not found${OLD:+ (only older ones:${OLD%,})}." >&2; hint; finish 1
}

fetch() {   # url file
  if command -v curl > /dev/null 2>&1; then curl -fsSL -o "$2" "$1" && return 0; fi
  if command -v wget > /dev/null 2>&1; then wget -q -O "$2" "$1" && return 0; fi
  "$PY" -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
}

make_venv() {   # $VENV with pip; without ensurepip (Debian / Ubuntu lacking python3-venv) pip comes from get-pip.py
  local log="$RT/venv.log"
  rm -rf "$VENV"; mkdir -p "$RT"
  echo "   creating a virtual environment in $VENV"
  if "$PY" -m venv "$VENV" > "$log" 2>&1 && "$(venv_py)" -m pip --version > /dev/null 2>&1; then return 0; fi
  rm -rf "$VENV"
  echo "   this Python cannot make a venv with pip (no ensurepip; on Debian / Ubuntu it is the python3-venv package)"
  echo "   trying a venv without pip + pip from https://bootstrap.pypa.io/get-pip.py"
  if "$PY" -m venv --without-pip "$VENV" >> "$log" 2>&1 && fetch https://bootstrap.pypa.io/get-pip.py "$RT/get-pip.py" \
     && "$(venv_py)" "$RT/get-pip.py" --disable-pip-version-check --no-warn-script-location -q >> "$log" 2>&1 \
     && "$(venv_py)" -m pip --version > /dev/null 2>&1; then
    rm -f "$RT/get-pip.py"; return 0
  fi
  rm -rf "$VENV" "$RT/get-pip.py"
  echo "STOP: could not create the Python virtual environment in $VENV with $PY ($(py_version "$PY"))." >&2
  echo "Python said:" >&2; tail -n 5 "$log" | sed 's/^/  /' >&2
  hint; finish 1
}

VPY=$(venv_py)
if "$VPY" -c "$NEED_ALL" > /dev/null 2>&1; then run_setup "$VPY" "$@"; fi

case "${1:-}" in
  check|help|-h|--help)   # report with whatever Python there is; setup itself adds what is missing
    find_python || { echo "== Python 3.10+: not found${OLD:+ (only older ones:${OLD%,})}" >&2; hint; finish 1; }
    echo "== Python: $(command -v "$PY") (setup will add the missing packages in tools/.runtime/venv)"
    run_setup "$PY" "$@";;
esac

echo "== Preparing Python for setup (only the first time)"
find_python || no_python
echo "   found Python: $(command -v "$PY") ($(py_version "$PY"))"
echo "   Setup needs these Python packages from PyPI (~15 MB): ${PKGS[*]}"
echo "   They go into $VENV only."
ask "   Install them now?" || { echo "STOP: setup cannot run without them; run ./setup.sh again and answer Y." >&2; finish 1; }
"$(venv_py)" -c "$NEED_VER" > /dev/null 2>&1 || make_venv
VPY=$(venv_py)
"$VPY" -m pip install --disable-pip-version-check "${PKGS[@]}" || { echo "STOP: pip could not install the packages (see above); check the internet connection and run ./setup.sh again." >&2; finish 1; }
"$VPY" -c "$NEED_ALL" || { echo "STOP: the packages were installed but do not import (see above)." >&2; finish 1; }
run_setup "$VPY" "$@"

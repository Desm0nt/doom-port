@echo off
rem DOOM Mobile: prepare the game files from your own dumps (Windows entry point; same arguments as setup.sh).
rem   setup.bat check
rem   setup.bat <base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys> [--push <adb serial>] [--dry-run] [--work DIR]
rem   setup.bat push <adb serial>
rem Paths with spaces must be quoted. All the work is done by tools\setup_doom.py; see docs\doom\GAME_FILES.md.
setlocal EnableExtensions
chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "HERE=%~dp0"
set "PYEXE="
set "PYARG="
if defined PYTHON set "PYEXE=%PYTHON%"
if not defined PYEXE (
  py -3 -c "import sys" >nul 2>&1 && (set "PYEXE=py" & set "PYARG=-3")
)
if not defined PYEXE (
  python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1 && set "PYEXE=python"
)
if not defined PYEXE (
  echo Python 3 was not found. Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^)
  echo or set PYTHON=C:\path\to\python.exe and run this again.
  exit /b 1
)
"%PYEXE%" %PYARG% -c "import cryptography, zstandard, lz4, capstone" >nul 2>&1
if errorlevel 1 (
  echo Some Python packages are missing. Install them with:
  echo   "%PYEXE%" %PYARG% -m pip install cryptography zstandard lz4 capstone
  if /i not "%~1"=="check" exit /b 1
)
"%PYEXE%" %PYARG% "%HERE%tools\setup_doom.py" %*
exit /b %ERRORLEVEL%

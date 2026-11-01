@echo off
rem DOOM Mobile setup for Windows: prepares the game folder for the phone from your own DOOM (Switch) dumps.
rem   setup.bat                   double-click: finds the base game, the update v262144 and prod.keys in
rem                               put-game-files-here\ (or this folder), under any names, and prepares everything
rem   setup.bat FILES...          the same with explicit files (or drop them onto setup.bat), in any order
rem   setup.bat check             what is installed / found / prepared, and the next step (changes nothing)
rem   setup.bat push [serial]     copy the prepared folder to the phone over adb
rem   more options: setup.bat --help
rem Python 3.10+ with its packages and the .NET 10 SDK are provided in tools\.runtime when missing (asked first;
rem DOOM_SETUP_YES=1 answers yes). All the work is done by tools\setup_doom.py; guide: docs\INSTALL.md.
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "HERE=%~dp0"
set "RT=%HERE%tools\.runtime"
set "MODE=install"
if /i "%~1"=="check" set "MODE=check"
if /i "%~1"=="--help" set "MODE=check"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\bootstrap.ps1" -Mode %MODE%
if errorlevel 1 goto failed
set "PYEXE=%RT%\venv\Scripts\python.exe"
"%PYEXE%" -c "import sys" >nul 2>&1 || set "PYEXE=%RT%\python\python.exe"
"%PYEXE%" -c "import sys" >nul 2>&1 && goto run
rem check mode before the first setup: any installed Python 3 will do for the report
set "PYEXE=py"
py -3 -c "import sys" >nul 2>&1 && goto run_py
set "PYEXE=python"
python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1 && goto run
echo Python is not available yet: run setup.bat without arguments, it offers to download a private copy.
goto failed
:run_py
py -3 "%HERE%tools\setup_doom.py" %*
set "RC=%ERRORLEVEL%"
goto end
:run
"%PYEXE%" "%HERE%tools\setup_doom.py" %*
set "RC=%ERRORLEVEL%"
goto end
:failed
set "RC=1"
:end
if /i "%~1"=="check" exit /b %RC%
if /i "%~1"=="push" exit /b %RC%
if /i "%~1"=="--help" exit /b %RC%
echo.
pause
exit /b %RC%

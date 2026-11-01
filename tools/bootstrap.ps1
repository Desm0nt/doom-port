# DOOM Mobile: Python for setup.bat (Windows PowerShell 5.1+). Finds Python 3.10+ or offers a private copy, and installs
# the packages setup needs; everything goes into tools\.runtime (nothing system-wide, no admin rights):
#   tools\.runtime\venv     a virtual environment on top of an installed Python 3.10+ (py launcher, PATH or $env:PYTHON)
#   tools\.runtime\python   a private Python when none is installed: python.org's official NuGet package ("python" on
#                           nuget.org, a complete Python without installer or registry entries)
# Packages from PyPI: cryptography, zstandard, lz4, capstone<6 (the patcher is checked with capstone 5).
# Every download is asked first (DOOM_SETUP_YES=1 answers yes). A second run reuses what is there and needs no network.
#   -Mode install   (default) make sure a ready Python exists; exit code 0 when tools\.runtime has one
#   -Mode check     only report, change nothing
param([ValidateSet('install', 'check')][string]$Mode = 'install')

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'      # Invoke-WebRequest is very slow with its progress bar in PowerShell 5.1
$Runtime = Join-Path $PSScriptRoot '.runtime'
$Venv = Join-Path $Runtime 'venv'
$VenvPy = Join-Path $Venv 'Scripts\python.exe'
$Private = Join-Path $Runtime 'python'
$PrivatePy = Join-Path $Private 'python.exe'
$PyVersion = '3.12.10'
$NuGetUrl = "https://api.nuget.org/v3-flatcontainer/python/$PyVersion/python.$PyVersion.nupkg"
$Packages = @('cryptography', 'zstandard', 'lz4', 'capstone<6')
$NeedVersion = 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
$NeedAll = 'import sys; assert sys.version_info >= (3, 10); import cryptography, zstandard, lz4, capstone'
$Yes = $env:DOOM_SETUP_YES -eq '1'

function Say($text) { Write-Host "== $text" }

function Ask($question) {
    if ($Yes) { Write-Host "$question [Y/n] y (DOOM_SETUP_YES=1)"; return $true }
    $a = Read-Host "$question [Y/n]"
    return ($null -ne $a) -and ($a.Trim() -match '^(|y|yes)$')
}

function Test-Python($exe, [string[]]$pre, $code) {
    # True when "$exe $pre -c $code" runs and exits 0 (a missing file or the Store stub just fails).
    if (-not $exe) { return $false }
    try { & $exe @pre -c $code *> $null; return ($LASTEXITCODE -eq 0) } catch { return $false }
}

function Get-Executable($exe, [string[]]$pre) {
    try { $p = & $exe @pre -c 'import sys; print(sys.executable)' 2> $null } catch { return $null }
    if ($LASTEXITCODE -eq 0 -and $p) { return ([string]$p).Trim() }
    return $null
}

function Find-BasePython {
    # An installed Python 3.10+: $env:PYTHON, the private copy, the py launcher, python on PATH.
    if ($env:PYTHON -and (Test-Python $env:PYTHON @() $NeedVersion)) { return (Get-Executable $env:PYTHON @()) }
    if (Test-Python $PrivatePy @() $NeedVersion) { return $PrivatePy }
    if ((Get-Command py -ErrorAction SilentlyContinue) -and (Test-Python 'py' @('-3') $NeedVersion)) { return (Get-Executable 'py' @('-3')) }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd -and (Test-Python 'python' @() $NeedVersion)) { return (Get-Executable 'python' @()) }
    return $null
}

function Get-Download($url, $dst, $what) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Write-Host "   downloading $what ..."
    try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $dst; return $true }
    catch { Write-Host "   download failed: $($_.Exception.Message)"; return $false }
}

function Install-PrivatePython {
    Write-Host ''
    Write-Host "   Setup can download a private Python $PyVersion (python.org's official NuGet package, ~15 MB) into"
    Write-Host "   $Private"
    Write-Host '   It is only used by this folder: no installer, no registry entries, nothing system-wide.'
    if (-not (Ask '   Download it now?')) { return $false }
    New-Item -ItemType Directory -Force $Runtime | Out-Null
    $zip = Join-Path $Runtime "python.$PyVersion.zip"
    $tmp = Join-Path $Runtime 'python-unpack'
    if (-not (Get-Download $NuGetUrl $zip "Python $PyVersion from nuget.org")) { return $false }
    Remove-Item -Recurse -Force $tmp, $Private -ErrorAction SilentlyContinue
    try { Expand-Archive -Path $zip -DestinationPath $tmp -Force } catch { Write-Host "   unpacking failed: $($_.Exception.Message)"; return $false }
    Move-Item (Join-Path $tmp 'tools') $Private
    Remove-Item -Recurse -Force $tmp, $zip -ErrorAction SilentlyContinue
    return (Test-Python $PrivatePy @() $NeedVersion)
}

function Install-Packages($py) {
    Write-Host ''
    Write-Host "   Setup needs these Python packages from PyPI (~15 MB): $($Packages -join ', ')"
    Write-Host "   They go into tools\.runtime only (for $py)."
    if (-not (Ask '   Install them now?')) { return $false }
    if (-not (Test-Python $py @() 'import pip')) { & $py -m ensurepip --default-pip *> $null }
    & $py -m pip install --disable-pip-version-check --no-warn-script-location --only-binary=:all: @Packages
    return (Test-Python $py @() $NeedAll)
}

function Use-Base($base) {
    # The private Python gets the packages itself; an installed one gets a venv in tools\.runtime\venv.
    if ($base -eq $PrivatePy) { return (Install-Packages $PrivatePy) }
    if (-not (Test-Python $VenvPy @() $NeedVersion)) {
        Remove-Item -Recurse -Force $Venv -ErrorAction SilentlyContinue
        Say "creating a virtual environment in $Venv (from $base)"
        & $base -m venv $Venv
        if (-not (Test-Python $VenvPy @() $NeedVersion)) { Write-Host '   could not create the virtual environment'; return $false }
    }
    return (Install-Packages $VenvPy)
}

# ---- already prepared?
if (Test-Python $VenvPy @() $NeedAll) { if ($Mode -eq 'check') { Say "Python: $VenvPy (ready)" }; exit 0 }
if (Test-Python $PrivatePy @() $NeedAll) { if ($Mode -eq 'check') { Say "Python: $PrivatePy (ready)" }; exit 0 }

$base = Find-BasePython
if ($Mode -eq 'check') {
    if ($base) { Say "Python: $base (setup will add the missing packages in tools\.runtime)" }
    else { Say 'Python 3.10+: not found (setup will offer a private copy in tools\.runtime\python)' }
    exit 0
}

Say 'Preparing Python for setup (only the first time)'
if ($base) {
    Write-Host "   found Python: $base"
    if (Use-Base $base) { exit 0 }
    if ($base -ne $PrivatePy) {
        Write-Host '   The installed Python did not work out; a private copy usually does.'
        if ((Install-PrivatePython) -and (Use-Base $PrivatePy)) { exit 0 }
    }
} else {
    Write-Host '   Python 3.10 or newer was not found.'
    if ((Install-PrivatePython) -and (Use-Base $PrivatePy)) { exit 0 }
}
Write-Host ''
Write-Host 'Python is not ready. Run setup.bat again (and answer Y), or install Python 3.12 yourself:'
Write-Host '  https://www.python.org/downloads/windows/  (tick "Add python.exe to PATH")   or:   winget install -e --id Python.Python.3.12'
exit 2

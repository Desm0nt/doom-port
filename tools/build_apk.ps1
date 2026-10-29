# Build libdm.so (DM_GAME=doom) and the adrenotools hook libraries, package the Android app around them and sign it.
# Usage (from anywhere): powershell -File tools/build_apk.ps1 [-Install <adb serial>] [-Validation]
#
# Needs a configured native build tree in work/build-android (see README.md, "Building"), and:
#   JAVA_HOME        JDK 17+ (Gradle's Android plugin)
#   ANDROID_HOME     Android SDK with build-tools (apksigner) and platform-tools (adb)
#   GRADLE           Gradle launcher (default: gradle on PATH)
#   PYTHON           Python 3 used by the Gradle string generator (default: python on PATH)
#   DM_KEYSTORE      signing keystore (default: the Android debug keystore, ~/.android/debug.keystore)
#   DM_KEYSTORE_PASS keystore and key password (default: android); DM_KEY_ALIAS (default: androiddebugkey)
#   DM_BUILD_TOOLS   build-tools version to sign with (default: the newest installed)
# Keystores never go into this repository (.gitignore excludes *.keystore / *.jks).
param([string]$Install = '', [switch]$Validation)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
function Env-Or([string]$name, [string]$default) { $v = [Environment]::GetEnvironmentVariable($name); if ($v) { $v } else { $default } }
if (-not $env:ANDROID_HOME) { throw 'Set ANDROID_HOME to the Android SDK' }
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$env:PYTHONUTF8 = '1'
$gradle = Env-Or 'GRADLE' 'gradle'
$python = Env-Or 'PYTHON' 'python'
$keystore = Env-Or 'DM_KEYSTORE' (Join-Path $HOME '.android/debug.keystore')
$kspass = Env-Or 'DM_KEYSTORE_PASS' 'android'
$alias = Env-Or 'DM_KEY_ALIAS' 'androiddebugkey'
$bt = Env-Or 'DM_BUILD_TOOLS' ((Get-ChildItem "$env:ANDROID_HOME/build-tools" -Directory | Sort-Object { [version]($_.Name -replace '[^0-9.].*$', '') } | Select-Object -Last 1).Name)
$apksigner = Join-Path $env:ANDROID_HOME "build-tools/$bt/apksigner$(if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.bat' })"
$adb = Join-Path $env:ANDROID_HOME 'platform-tools/adb'
if (-not (Test-Path work/build-android/CMakeCache.txt)) { throw 'Configure the native build first: cmake -S dm/android -B work/build-android ... -DDM_GAME=doom (README.md, Building)' }
New-Item -ItemType Directory -Force work/logs, work/jni/arm64-v8a | Out-Null

& cmake --build work/build-android --target dm *> work/logs/native-dm.log
if ($LASTEXITCODE) { throw 'Native build failed (work/logs/native-dm.log)' }
Copy-Item work/build-android/libdm.so work/jni/arm64-v8a/libdm.so
# adrenotools hook libraries (loaded from nativeLibraryDir when a custom Vulkan driver such as Turnip is chosen)
& cmake --build work/build-android --target main_hook file_redirect_hook gsl_alloc_hook hook_impl *> work/logs/native-hooks.log
if ($LASTEXITCODE) { throw 'Hook build failed (work/logs/native-hooks.log)' }
Get-ChildItem work/build-android/adrenotools -Recurse -Filter '*.so' | Copy-Item -Destination work/jni/arm64-v8a
# -Validation: package the Khronos validation layer for DM_VK_VALIDATION=1 runs. Download the Android release of
# Vulkan-ValidationLayers yourself and put arm64-v8a/libVkLayer_khronos_validation.so under work/vvl (not redistributed here).
$vvl = 'work/jni/arm64-v8a/libVkLayer_khronos_validation.so'
if ($Validation) {
    $layer = Get-ChildItem work/vvl -Recurse -Filter libVkLayer_khronos_validation.so | Where-Object { $_.FullName -match 'arm64-v8a' } | Select-Object -First 1
    if (-not $layer) { throw 'No arm64-v8a libVkLayer_khronos_validation.so under work/vvl' }
    Copy-Item $layer.FullName $vvl
} elseif (Test-Path $vvl) { Remove-Item $vvl }

$jni = (Resolve-Path work/jni).Path -replace '\\', '/'
# Default Vulkan driver bundled as assets: Turnip (Mesa, MIT) fetched by tools/fetch_turnip.py with a pinned SHA-256;
# without it the APK simply uses the system driver.
& $python tools/fetch_turnip.py *> work/logs/fetch-turnip.log
$drv = if (Test-Path work/driver-assets/drivers) { '-PdmDriverAssets=' + ((Resolve-Path work/driver-assets).Path -replace '\', '/') } else { '-PdmNoDriverAssets=1' }
& $gradle -p dm/android :app:assembleDebug '-PdmGame=doom' '-PdmAppId=org.doom.port' "-PdmPython=$python" "-PdmPrebuiltJni=$jni" --console plain $drv *> work/logs/gradle-apk.log
if ($LASTEXITCODE) { throw 'APK build failed (work/logs/gradle-apk.log)' }
if (-not (Test-Path $keystore)) { throw "Keystore not found: $keystore (set DM_KEYSTORE)" }
& $apksigner sign --ks $keystore --ks-pass "pass:$kspass" --key-pass "pass:$kspass" --ks-key-alias $alias --out work/DOOM-port-debug.apk dm/android/app/build/outputs/apk/debug/app-debug.apk
if ($LASTEXITCODE) { throw 'APK signing failed' }
Get-Item work/DOOM-port-debug.apk | Select-Object FullName, Length
if ($Install) {
    & $adb -s $Install install -r work/DOOM-port-debug.apk
    if ($LASTEXITCODE) { throw 'APK installation failed' }
}

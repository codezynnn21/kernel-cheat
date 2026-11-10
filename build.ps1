# build.ps1 — Windows build + Magisk module packaging.
# Needs: Android NDK r26+, CMake, Ninja (both ship with the Android SDK).
#   .\build.ps1 -Ndk "C:\Users\you\AppData\Local\Android\Sdk\ndk\27.2.12479018"
param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

if (-not $Sdk) { $Sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
if (-not $Ndk) {
    $ndkRoot = Join-Path $Sdk "ndk"
    if (Test-Path $ndkRoot) {
        $Ndk = (Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending |
                Select-Object -First 1).FullName
    }
}
if (-not $Ndk -or -not (Test-Path $Ndk)) {
    throw "no NDK found — pass -Ndk <path> or set ANDROID_NDK_HOME"
}
Write-Host "NDK: $Ndk"

# cmake/ninja prefer the SDK-bundled copies
$tools = @()
$cmakeDir = Join-Path $Sdk "cmake"
if (Test-Path $cmakeDir) {
    $c = Get-ChildItem $cmakeDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($c) { $tools += (Join-Path $c.FullName "bin") }
}
$cmake = $null; $ninja = $null
foreach ($dir in $tools) {
    if (-not $cmake -and (Test-Path (Join-Path $dir "cmake.exe"))) { $cmake = Join-Path $dir "cmake.exe" }
    if (-not $ninja -and (Test-Path (Join-Path $dir "ninja.exe"))) { $ninja = Join-Path $dir "ninja.exe" }
}
if (-not $cmake) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $ninja) { $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $cmake) { throw "cmake not found — install it from the Android SDK (SDK Manager > CMake)" }
if (-not $ninja) { throw "ninja not found — install it from the Android SDK (SDK Manager > CMake)" }

Remove-Item -Recurse -Force $BuildDir, "out" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "out" | Out-Null

& $cmake -S . -B $BuildDir -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    -DANDROID_ABI=arm64-v8a `
    -DANDROID_PLATFORM=android-24 `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$stage = Join-Path $Root "out\module"
New-Item -ItemType Directory -Force -Path (Join-Path $stage "bin"), (Join-Path $stage "assets") | Out-Null
Copy-Item "magisk\*" $stage -Recurse -Force
Copy-Item (Join-Path $BuildDir "out\codm-inject") (Join-Path $stage "bin\codm-inject") -Force
Copy-Item (Join-Path $BuildDir "out\libcodm.so") (Join-Path $stage "assets\libcodm.so") -Force

$zip = Join-Path $Root "out\codm_overlay.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)

Get-Item $zip | Select-Object FullName, Length
Write-Host "flash the zip in Magisk, reboot, launch CODM"

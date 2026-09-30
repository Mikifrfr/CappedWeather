# Launch CappedWeather from the CMake build tree on Windows.
# Sets PATH to the Qt + vcpkg runtime DLLs and QT_PLUGIN_PATH to the Qt
# plugins, then starts build/<Config>/CappedWeather.exe so no manual
# PATH setup or windeployqt step is required first.
#
# Usage:
#   tools/launch-cappedweather.ps1 [-Config Release] [-BuildDir build] [-- exe args...]
param(
    [string]$Config = "Release",
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "build"),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArgs
)

$ErrorActionPreference = "Stop"

# Detect Qt: prefer the standard aqt Qt install, fall back to CMAKE_PREFIX_PATH.
$QtDir = "C:/Qt/6.8.3/msvc2022_64"
if (-not (Test-Path (Join-Path $QtDir "bin/windeployqt.exe"))) {
    if ($env:CMAKE_PREFIX_PATH -and (Test-Path (Join-Path $env:CMAKE_PREFIX_PATH "bin/windeployqt.exe"))) {
        $QtDir = $env:CMAKE_PREFIX_PATH
    }
    else {
        Write-Warning "Qt not found at $QtDir and CMAKE_PREFIX_PATH is unset or has no bin/windeployqt.exe."
    }
}
$QtBin = Join-Path $QtDir "bin"
$QtPlugins = Join-Path $QtDir "plugins"

# Detect vcpkg runtime DLLs (netCDF/HDF5/bz2/zlib when enabled).
$VcpkgBin = $null
$vcpkgCandidates = @("C:/vcpkg/installed/x64-windows/bin")
if ($env:VCPKG_ROOT) {
    $vcpkgCandidates = @((Join-Path $env:VCPKG_ROOT "installed/x64-windows/bin")) + $vcpkgCandidates
}
if ($env:VCPKG_INSTALLATION_ROOT) {
    $vcpkgCandidates = @((Join-Path $env:VCPKG_INSTALLATION_ROOT "installed/x64-windows/bin")) + $vcpkgCandidates
}
foreach ($candidate in $vcpkgCandidates) {
    if ($candidate -and (Test-Path $candidate)) { $VcpkgBin = $candidate; break }
}

$pathParts = @()
if ((Test-Path $QtBin)) { $pathParts += $QtBin }
if ($VcpkgBin) { $pathParts += $VcpkgBin }
if ($pathParts.Count -gt 0) {
    $env:PATH = ($pathParts -join ";") + ";" + $env:PATH
}
if (Test-Path $QtPlugins) {
    $env:QT_PLUGIN_PATH = $QtPlugins
}

$exe = Join-Path $BuildDir (Join-Path $Config "CappedWeather.exe")
if (-not (Test-Path $exe)) {
    throw "CappedWeather.exe not found at $exe. Build first: cmake --build $BuildDir --config $Config"
}

Write-Host "Starting $exe"
if ($AppArgs.Count -gt 0) {
    & $exe @AppArgs
}
else {
    & $exe
}

#!/bin/sh
# Assemble a portable Linux AppDir for CappedWeather and tar it as
# CappedWeather-<version>-Linux-x86_64.tgz containing top-level AppDir/
# (README promise: extract and run ./AppDir/AppRun).
#
# Usage: sh packaging/linux-appdir.sh [build_dir] [output_tgz]
#
# Local WSL verification (Ubuntu, Qt at /root/Qt): the repo is private and
# WSL root has no GitHub credentials, so `git pull` inside WSL silently
# goes nowhere. Sync from Windows instead, then build in WSL:
#   git archive --format=tar -o /tmp/main.tar main   # on Windows
#   # in WSL: wipe tree except .git, extract tarball over it,
#   # cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release && build && ctest
#
# Expects the CappedWeather target already built in <build_dir> with Qt 6
# (CMAKE_PREFIX_PATH pointing at the aqt gcc_64 install) plus system
# netCDF-C and HDF5. Bundles Qt + shared deps with linuxdeploy +
# linuxdeploy-plugin-qt, which are downloaded to the cache dir unless
# LINUXDEPLOY_BIN / LINUXDEPLOY_PLUGIN_QT_BIN are set to extracted AppRun
# binaries. linuxdeploy AppImages are *extracted* (not executed), so no FUSE
# is needed on CI runners.
set -eu

BUILD_DIR="${1:-build}"
OUT_TGZ="${2:-}"
VERSION="$(sed -n 's/^project(CappedWeather VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
if [ -z "$OUT_TGZ" ]; then
    OUT_TGZ="CappedWeather-${VERSION}-Linux-x86_64.tgz"
fi

if [ -x "${BUILD_DIR}/CappedWeather" ]; then
    BINARY="${BUILD_DIR}/CappedWeather"
elif [ -x "${BUILD_DIR}/Release/CappedWeather" ]; then
    BINARY="${BUILD_DIR}/Release/CappedWeather"
else
    echo "error: CappedWeather binary not found in ${BUILD_DIR}" >&2
    exit 1
fi

CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/cappedweather"
mkdir -p "$CACHE_DIR"
DEPLOY_BIN="${LINUXDEPLOY_BIN:-$CACHE_DIR/linuxdeploy-root/AppRun}"
PLUGIN_BIN="${LINUXDEPLOY_PLUGIN_QT_BIN:-$CACHE_DIR/linuxdeploy-plugin-qt-root/AppRun}"

download_and_extract() {
    # $1 = url, $2 = extracted dir, $3 = AppImage path
    if [ ! -x "$2/AppRun" ]; then
        echo "Fetching $1 ..."
        curl -fsSL -o "$3" "$1"
        chmod +x "$3"
        rm -rf "$2"
        (cd "$CACHE_DIR" && "$3" --appimage-extract >/dev/null)
        mv "$CACHE_DIR/squashfs-root" "$2"
    fi
}

download_and_extract \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
    "$CACHE_DIR/linuxdeploy-root" \
    "$CACHE_DIR/linuxdeploy-x86_64.AppImage"
download_and_extract \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "$CACHE_DIR/linuxdeploy-plugin-qt-root" \
    "$CACHE_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"

rm -rf AppDir
mkdir -p AppDir

# linuxdeploy resolves the qt plugin from PATH as linuxdeploy-plugin-qt.
PLUGIN_DIR="$CACHE_DIR/bin"
mkdir -p "$PLUGIN_DIR"
cp -f "$PLUGIN_BIN" "$PLUGIN_DIR/linuxdeploy-plugin-qt"
chmod +x "$PLUGIN_DIR/linuxdeploy-plugin-qt"
export PATH="$PLUGIN_DIR:$PATH"

# Deploy Qt plugins for the app (the plugin reads QTDIR/qmldir from the
# Qt installation used at build time via CMake prefix / qmake on PATH).
"$DEPLOY_BIN" --appdir AppDir \
    -e "$BINARY" \
    -d packaging/CappedWeather.desktop \
    -i packaging/CappedWeather.png \
    --plugin qt

# AGPL-3.0-or-later: license text travels with the binary.
cp -f LICENSE AppDir/LICENSE
cp -f packaging/CappedWeather.desktop AppDir/CappedWeather.desktop

tar czf "$OUT_TGZ" AppDir
echo "Wrote $OUT_TGZ"

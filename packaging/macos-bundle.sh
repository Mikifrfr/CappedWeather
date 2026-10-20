#!/bin/sh
# Assemble a macOS arm64 .app bundle for CappedWeather and tar it as
# CappedWeather-<version>-macOS-arm64-preview.tgz containing a top-level
# CappedWeather.app (extract, then right-click > Open on the app).
#
# Usage: sh packaging/macos-bundle.sh [build_dir] [output_tgz]
#
# macOS only: needs otool/lipo/install_name_tool/codesign/sips/iconutil from
# the Xcode Command Line Tools, plus a <build_dir> configured with
#   -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
#   -DCMAKE_PREFIX_PATH=<aqt Qt 6.8.3 macos dir>
#   -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   -DVCPKG_TARGET_TRIPLET=arm64-osx
# so Qt, netCDF-C and HDF5 are all real (a stub build is not packageable:
# CAPPEDWEATHER_REQUIRE_NETCDF/HDF5=ON is the default and fails fast).
#
# Steps, in order (each one is a check, not a hope):
#   1. `cmake --install` into dist/: Qt's deploy script (macdeployqt) puts
#      Qt frameworks and plugins into Contents/Frameworks.
#   2. Fail if Qt is still referenced by absolute path (deploy didn't run).
#   3. Copy vcpkg's netCDF/HDF5 dylib closure into Contents/Frameworks and
#      rewrite every absolute install name to @rpath, adding
#      @executable_path/../Frameworks (app) and @loader_path (dylibs). vcpkg's
#      arm64-osx triplet links statically, so with the release recipe there is
#      usually nothing to copy here — the step exists so a dynamic triplet (or
#      Homebrew netcdf/hdf5) cannot silently produce a bundle that only works
#      on a machine with those libraries installed.
#   4. Fail if any dependency still resolves outside the bundle.
#   5. Icon from packaging/CappedWeather.png, LICENSE into Resources.
#   6. Ad-hoc signature: arm64 refuses unsigned images and install_name_tool
#      invalidates the link-time signature. Not notarized (no Developer ID),
#      so a downloaded copy needs right-click > Open or
#      `xattr -dr com.apple.quarantine CappedWeather.app`.
#   7. tar.gz + sha256.
#
# The resulting artifact is a PREVIEW until it has been launched on a Mac and
# shown a first echo from a real station (docs/packaging-macos.md has the
# checklist). Do not drop the -preview suffix before that has happened.
set -eu

BUILD_DIR="${1:-build}"
VERSION="$(sed -n 's/^project(CappedWeather VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
OUT_TGZ="${2:-CappedWeather-${VERSION}-macOS-arm64-preview.tgz}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "error: macos-bundle.sh runs on macOS only (needs otool/codesign)" >&2
    exit 1
fi

DIST_DIR="${DIST_DIR:-dist}"
APP="${DIST_DIR}/CappedWeather.app"
BIN="${APP}/Contents/MacOS/CappedWeather"
FW="${APP}/Contents/Frameworks"

# --- 1. install tree + Qt deploy -------------------------------------------
rm -rf "$DIST_DIR"
cmake --install "$BUILD_DIR" --prefix "$DIST_DIR"
if [ ! -x "$BIN" ]; then
    echo "error: no app bundle at ${APP} after cmake --install" >&2
    exit 1
fi
mkdir -p "$FW"

ARCHS="$(lipo -archs "$BIN")"
case " $ARCHS " in
    *" arm64 "*) ;;
    *)
        echo "error: ${BIN} is not arm64 (archs: ${ARCHS})" >&2
        echo "       Configure with -DCMAKE_OSX_ARCHITECTURES=arm64." >&2
        exit 1
        ;;
esac

# --- 2. Qt must have been deployed -----------------------------------------
if otool -L "$BIN" | grep -qE '^[[:space:]]+/.*/Qt(Core|Gui|Widgets)\.framework/'; then
    echo "error: Qt frameworks are still referenced by absolute path." >&2
    echo "       Configure with CAPPEDWEATHER_DEPLOY_QT=ON (the default) so the" >&2
    echo "       Qt deploy script runs during cmake --install." >&2
    exit 1
fi

# --- 3. bundle the non-system dylib closure --------------------------------
# Absolute non-system dependencies of a Mach-O image, one path per line.
absolute_deps() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' |
        grep -v -E '^(/usr/lib/|/System/|@rpath/|@executable_path/|@loader_path/)' || true
}

SEEN="${DIST_DIR}/.bundled-dylibs"
QUEUE="${DIST_DIR}/.dylib-queue"
: > "$SEEN"
: > "$QUEUE"

bundle_dylib() {
    src="$1"
    base="$(basename "$src")"
    if grep -qxF "$base" "$SEEN"; then
        return 0
    fi
    if [ ! -f "$src" ]; then
        echo "error: dependency ${src} does not exist" >&2
        return 1
    fi
    echo "$base" >> "$SEEN"
    cp -f "$src" "$FW/$base"
    chmod u+w "$FW/$base"
    install_name_tool -id "@rpath/$base" "$FW/$base"
    echo "$FW/$base" >> "$QUEUE"
}

queue_image() {
    image="$1"
    absolute_deps "$image" | while IFS= read -r dep; do
        base="$(basename "$dep")"
        bundle_dylib "$dep"
        install_name_tool -change "$dep" "@rpath/$base" "$image"
    done
}

queue_image "$BIN"
while [ -s "$QUEUE" ]; do
    current="$(head -n 1 "$QUEUE")"
    # BSD sed: -i takes an explicit (empty) backup suffix.
    sed -i '' '1d' "$QUEUE"
    queue_image "$current"
done

add_rpath() {
    image="$1"
    path="$2"
    if otool -l "$image" | grep -A2 LC_RPATH | grep -qF "path ${path} "; then
        return 0
    fi
    install_name_tool -add_rpath "$path" "$image"
}

add_rpath "$BIN" "@executable_path/../Frameworks"
for lib in "$FW"/*.dylib; do
    [ -e "$lib" ] || continue
    add_rpath "$lib" "@loader_path"
done

# --- 4. fail-closed audit --------------------------------------------------
OFFENDERS="${DIST_DIR}/.unbundled-deps"
: > "$OFFENDERS"
audit_image() {
    absolute_deps "$1" | while IFS= read -r dep; do
        echo "$1 -> $dep" >> "$OFFENDERS"
    done
}
audit_image "$BIN"
for lib in "$FW"/*.dylib; do
    [ -e "$lib" ] || continue
    audit_image "$lib"
done
if [ -s "$OFFENDERS" ]; then
    echo "error: the bundle still references libraries outside the app" >&2
    echo "       (this copy would work on the build machine and fail on a clean Mac):" >&2
    cat "$OFFENDERS" >&2
    exit 1
fi

# --- 5. icon + license -----------------------------------------------------
ICONSET="${DIST_DIR}/CappedWeather.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" packaging/CappedWeather.png \
        --out "${ICONSET}/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" packaging/CappedWeather.png \
        --out "${ICONSET}/icon_${size}x${size}@2x.png" >/dev/null
done
mkdir -p "${APP}/Contents/Resources"
iconutil -c icns "$ICONSET" -o "${APP}/Contents/Resources/CappedWeather.icns"
rm -rf "$ICONSET"
PLIST="${APP}/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string CappedWeather.icns" "$PLIST" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile CappedWeather.icns" "$PLIST"
cp -f LICENSE "${APP}/Contents/Resources/LICENSE"

# --- 6. ad-hoc signature ---------------------------------------------------
# --deep is deprecated for signed distribution builds but is exactly right
# for stamping an ad-hoc signature over already-signed nested code.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# --- 7. tarball ------------------------------------------------------------
rm -f "$OUT_TGZ"
COPYFILE_DISABLE=1 tar czf "$OUT_TGZ" -C "$DIST_DIR" CappedWeather.app
rm -f "$QUEUE" "$SEEN" "$OFFENDERS"
echo "Wrote ${OUT_TGZ} ($(du -h "$OUT_TGZ" | cut -f1))"
shasum -a 256 "$OUT_TGZ"

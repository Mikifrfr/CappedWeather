# macOS packaging (arm64, preview)

Status: **preview**. The tag job builds, audits, signs and smoke-launches an
arm64 `.app` bundle on a GitHub macOS runner, but no one has yet launched that
artifact on a Mac and confirmed a first echo from a real station. Until that
happens the asset keeps its `-preview` suffix and README does not describe
macOS as supported. This file is the record of what was checked, how, and what
is still open.

## Artifact

`CappedWeather-<version>-macOS-arm64-preview.tgz`

- One top-level `CappedWeather.app`; extract, then right-click > Open (ad-hoc
  signed, not notarized: a plain double-click is refused by Gatekeeper on first
  launch). `xattr -dr com.apple.quarantine CappedWeather.app` also works.
- Apple Silicon only. `CMAKE_OSX_ARCHITECTURES=arm64` and vcpkg triplet
  `arm64-osx`. A universal build would need every dependency built twice and
  `lipo`'d; vcpkg has no universal triplet, so that is a separate project.
- Qt 6.8.3 `clang_64` (aqt mac desktop package, which is universal) + Qt
  Multimedia, netCDF-C and HDF5 from vcpkg `arm64-osx`, which links **static**
  by default, so netCDF/HDF5 are inside the executable, not loose dylibs.
- Contents: `Contents/MacOS/CappedWeather`, Qt frameworks/plugins from Qt's
  deploy script (macdeployqt), `Contents/Resources/CappedWeather.icns`,
  `Contents/Resources/LICENSE`.

## Recipe

`.github/workflows/release.yml`, job `macos` (`runs-on: macos-latest`, asserted
`uname -m = arm64`):

1. aqt installs Qt 6.8.3 for `mac desktop clang_64` + `qtmultimedia` into
   `$GITHUB_WORKSPACE/Qt`, inside a venv (Homebrew python3 is PEP 668 managed).
2. vcpkg installs `netcdf-c:arm64-osx` and `hdf5:arm64-osx`.
3. Configure with `CMAKE_OSX_ARCHITECTURES=arm64`,
   `CMAKE_OSX_DEPLOYMENT_TARGET=12.0`, the vcpkg toolchain and
   `CAPPEDWEATHER_REQUIRE_NETCDF/HDF5=ON` (the default: a stub build fails
   configuration instead of shipping without the live readers).
4. Build, then `ctest -C Release` (core + UI smoke, offscreen).
5. `sh packaging/macos-bundle.sh build`: `cmake --install` into `dist/` (Qt
   deploy script runs macdeployqt), Qt-still-absolute check, arm64 check,
   non-system dylib closure bundled and rewritten to `@rpath`, fail-closed
   audit of every remaining load command, icon + LICENSE, ad-hoc
   `codesign --force --deep --sign -`, `codesign --verify --deep --strict`,
   then `tar czf` + sha256.
6. Smoke launch: the packaged binary is started with
   `QT_QPA_PLATFORM=offscreen` and must stay alive 25 s (a missing dylib kills
   it in under a second; exit code 0 or SIGTERM from the step passes, anything
   else fails the job).
7. Upload the tgz as a build artifact, and attach it to the GitHub Release on a
   `v*` tag.

## What the job proves, and what it does not

Proves, on real hardware-in-the-cloud: configuration sees real netCDF/HDF5,
the product tests pass on macOS, the bundle's load commands close over what is
inside the bundle, the ad-hoc signature verifies, and the packaged executable
gets past dyld and stays up.

Does **not** prove: that the window is usable on a real Mac session, that the
TLS/HTTP path works through the bundled Qt network stack, that a station
connects, or that a volume decodes. It also says nothing about Gatekeeper's
quarantine flow, because the runner never quarantines the file.

A green job is therefore **not** a release. The standing rule in this repo is
that no release ships without a smoke launch and a first echo from a real
station; a process that stays alive for 25 s offscreen does not meet it.

## Promotion checklist (preview -> supported)

Run on an Apple Silicon Mac with network access, using the asset from the tag:

1. `shasum -a 256 CappedWeather-<ver>-macOS-arm64-preview.tgz` and compare with
   the value printed by the release job.
2. Extract, `xattr -dr com.apple.quarantine CappedWeather.app`, launch it.
3. Pick a station, press LIVE, and wait for the status line to report
   `received 1 complete volume`; confirm echo paints on the canvas.
4. Record in this file: version, station id, UTC timestamp, and whether
   overlays matched the Windows build. `build/cappedweather_live_gui_probe
   <out.png>` prints `live_probe=passed radar=<id> ...` and saves a screenshot
   if a scripted confirmation is preferred (build it from the same tag on that
   Mac).
5. Only then: drop `-preview` from the default output name in
   `packaging/macos-bundle.sh` (and the README download line), and change the
   TODO entry from preview to shipped.

## Cross-checks requested with this work

- **Update discovery needs no platform token.** `src/app/update_check.cpp`
  reads only `tag_name`, `prerelease`, `html_url` and `published_at` from
  `/releases/latest`; it never inspects or downloads assets. A macOS asset (or
  a Flatpak one) changes nothing: the newer-version notice opens the verified
  releases page and the user picks the artifact. No download, no install, per
  the discovery-only rule. The header now says this explicitly.
- **`packaging/` was carrying a Linux-only install rule into macOS.** CMake
  installed `CappedWeather.desktop` and the XDG icon on every non-Windows
  prefix, which includes a `.app` install tree. Gated to `UNIX AND NOT APPLE`.
  `linux-appdir.sh` stays Linux. No Windows-specific files leak into the macOS
  path; the `qschannelbackend.dll` copy and the `RUNTIME_DEPENDENCIES` install
  were already `WIN32`-only.
- **CPack is not used for macOS.** A CPack TGZ of the raw install tree would
  carry undeployed dylibs and an unsigned bundle; `macos-bundle.sh` owns the
  macOS artifact, and `CMakeLists.txt` says so.

## Verification log

- 2026-10-20: `actionlint 1.7.12` clean on `.github/workflows/release.yml` and
  `ci.yml`; `bash -n` / `sh -n` clean on `packaging/macos-bundle.sh`.
- 2026-10-20: Qt package identity confirmed against
  `download.qt.io/online/qtsdkrepository/mac_x64/desktop/qt6_683/qt6_683/Updates.xml`
  (`qt.qt6.683.clang_64`, `qt.qt6.683.addons.qtmultimedia`) and the package
  directory listing (`qtbase-MacOS-MacOS_14-Clang-MacOS-MacOS_14-X86_64-ARM64.7z`:
  universal, so the arm64 app links Qt's arm64 slice). aqt's `clang_64` arch
  and `-m qtmultimedia` are the right names for 6.8.3.
- 2026-10-20: vcpkg `arm64-osx.cmake` read from microsoft/vcpkg: arm64 Darwin,
  `VCPKG_LIBRARY_LINKAGE static`, hence no netCDF/HDF5 dylibs to bundle and
  the closure step is a guard rather than an expected copy.
- **Not yet run:** the `macos` job itself has not executed on a GitHub runner
  as of this commit. The first tag run is the first hardware evidence; treat
  anything it reports about the bundle as unconfirmed until the checklist above
  is done on a Mac.

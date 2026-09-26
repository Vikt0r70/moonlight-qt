# SeatHub software bill of materials

Every third-party dependency in the shipped SeatHub client package, with the version actually
present in the artifact, its license, and where its source lives.

This list was not written from memory. Each version was read off the binary that ships:

| How | Used for |
|-----|----------|
| Windows file-version resource (`(Get-Item x.dll).VersionInfo`) | Qt modules, FFmpeg, SDL2, SDL2_ttf, dav1d, libplacebo, OpenSSL |
| A version string compiled into the binary | Opus (`libopus 1.5.2`), dav1d (`1.4.3-0-ge9986de`) |
| A license string compiled into the binary | FFmpeg (`libavcodec license: LGPL version 2.1 or later`) |
| `git submodule status` in the fork | the statically linked libraries |
| The deploy folder's own file list | what is shipped and what is not |

Where a version genuinely is not recorded by the artifact, this file says so rather than supplying a
plausible one. That applies to `discord-rpc` below.

The payload is the deploy folder the installer packages (`build/deploy-x64-release`): 46 DLLs, the
client executable, and one data file.

## Shipped in the package

### Qt 6.11.2

- **License:** LGPL-3.0-only (Qt's own dual-license: LGPL-3.0 or a commercial license)
- **Source:** https://download.qt.io/official_releases/qt/6.11/6.11.2/single/
- **Linked:** dynamically — every module ships as a DLL, which is what LGPL-3.0 section 4 requires
  for a customer to be able to replace it.
- **Modules:** Core, Gui, Network, OpenGL, Qml, QmlMeta, QmlModels, QmlWorkerScript, Quick,
  QuickControls2 (+ Basic, BasicStyleImpl, FluentWinUI3StyleImpl, Impl, Material,
  MaterialStyleImpl), QuickEffects, QuickLayouts, QuickShapes, QuickTemplates2, Svg, WebSockets.
- **Plugins:** `platforms/`, `imageformats/`, `iconengines/`, `networkinformation/`, `tls/`,
  `qml/`, `translations/`.
- **`opengl32sw.dll`** is Qt's bundled software OpenGL implementation, same license and source.
- Qt itself is not modified by this fork: no Qt file appears in the fork's diff against upstream, and
  none is patched in the build.

### FFmpeg

- **Components and versions:** `avcodec-61.dll` 61.3.100, `avutil-59.dll` 59.8.100,
  `swscale-8.dll` 8.1.100 — all report company "FFmpeg Project".
- **License: LGPL-2.1-or-later.** Taken from the binaries themselves, not from FFmpeg's project
  page: both DLLs carry the string `libavcodec license: LGPL version 2.1 or later` /
  `libavutil license: LGPL version 2.1 or later`. This matters, because an FFmpeg built with
  `--enable-gpl` would be GPL-2.0-or-later instead, and the two are not interchangeable here.
- **Source (project):** https://ffmpeg.org/
- **Source (this build):** the prebuilt binaries come from moonlight-qt's `libs` prebuilts
  repository, https://github.com/moonlight-stream/moonlight-qt-prebuilts, pinned in this fork at
  commit `a27d6a7`.
- **Linked:** dynamically.

### Dav1d (AV1 decoder)

- **Version:** library 1.4.3 (the DLL's own `dav1d_version` string is `1.4.3-0-ge9986de`; its
  Windows file-version resource says 7.0.0, which is dav1d's *API* major version, not the release).
- **License:** BSD-2-Clause
- **Source:** https://code.videolan.org/videolan/dav1d
- **Linked:** dynamically (`dav1d.dll`).

### libplacebo

- **Version:** v7.349.0 (file-version resource; the file name `libplacebo-349.dll` carries the same
  number)
- **License:** LGPL-2.1-or-later
- **Source:** https://code.videolan.org/videolan/libplacebo
- **Linked:** dynamically.

### Opus

- **Version:** 1.5.2 (the DLL carries the string `libopus 1.5.2`)
- **License:** BSD-3-Clause
- **Source:** https://opus-codec.org/
- **Linked:** dynamically (`opus.dll`).

### OpenSSL

- **Version:** 3.3.2 (`libcrypto-3-x64.dll`, `libssl-3-x64.dll`, both reporting "The OpenSSL
  Project")
- **License:** Apache-2.0
- **Source:** https://www.openssl.org/source/
- **Linked:** dynamically.

### SDL2

- **Version:** 2.31.0
- **License:** Zlib
- **Source:** https://github.com/libsdl-org/SDL
- **Linked:** dynamically (`SDL2.dll`).

### SDL2_ttf

- **Version:** 2.22.0
- **License:** Zlib
- **Source:** https://github.com/libsdl-org/SDL_ttf
- **Linked:** dynamically (`SDL2_ttf.dll`).

### SDL_GameControllerDB

- **Version:** no release numbering; pinned at commit `e5a5fa2` in this fork's submodules.
- **Ships as:** the data file `gamecontrollerdb.txt` (527 KB), not a library.
- **License:** Zlib
- **Source:** https://github.com/gabomdq/SDL_GameControllerDB

### discord-rpc

- **Version: not recorded by the artifact.** The DLL carries no version resource and no version
  string; its only build metadata is the path `C:\projects\moonlight-deps\build\discord-rpc\...`,
  which identifies the prebuilts repository below, not a release. Recorded as version
  `not recorded; prebuilts libs@a27d6a7` rather than guessed.
- **License:** MIT
- **Source:** https://github.com/discord/discord-rpc, shipped as a prebuilt from
  https://github.com/moonlight-stream/moonlight-qt-prebuilts at `libs` commit `a27d6a7`
- **Linked:** dynamically (`discord-rpc.dll`).

### sentry-native

- **Version:** 0.17.1 (06.3.1 D-18 pin; `seathub-ops/pins.yaml` `sentry_native`)
- **License:** MIT
- **Source:** https://github.com/getsentry/sentry-native, release source zip
  `https://github.com/getsentry/sentry-native/releases/download/0.17.1/sentry-native.zip`, SHA-256
  checked against the pin before extraction (`scripts/build-seathub.ps1`).
- **Linked:** dynamically (`sentry.dll`), built with `SENTRY_BACKEND=crashpad`,
  `SENTRY_TRANSPORT=winhttp`, `SENTRY_INTEGRATION_QT=OFF`, `SENTRY_BUILD_RUNTIMESTATIC=ON`.
- **Ships as:** `sentry.dll` beside `SeatHub.exe`. No `.pdb` shipped (D-12-pdb: symbols upload to
  Sentry's own CI step, never to a customer PC).

### crashpad (getsentry fork, bundled in the sentry-native release zip)

- **Version:** vendored at `external/crashpad` inside the sentry-native 0.17.1 zip; no independent
  release numbering of its own (a `getsentry/crashpad` fork of Chromium's crashpad, per its own
  `README.getsentry.md`).
- **License:** Apache-2.0
- **Source:** https://github.com/getsentry/crashpad
- **Linked:** the out-of-process handler and its WER helper module, both built from this source:
  `crashpad_handler.exe`, `crashpad_wer.dll`.
- **Ships as:** `crashpad_handler.exe`, `crashpad_wer.dll` beside `SeatHub.exe`. No `.pdb` shipped.

### mini_chromium (crashpad's own third-party dependency)

- **Version:** commit `eef885b0bb80dab8e9d79b9dc0c050a1c8e50c0b` (crashpad's own `DEPS` file,
  `chromium_git/chromium/mini_chromium`), vendored in the same sentry-native 0.17.1 zip.
- **License:** BSD-3-Clause
- **Source:** https://chromium.googlesource.com/chromium/mini_chromium
- **Linked:** statically, into `crashpad_handler.exe` and `crashpad_wer.dll`. Ships no file of its
  own.

### zlib (crashpad's own third-party dependency)

- **Version:** 1.2.12 (its own `LICENSE` file's header), commit `fef58692c1d7bec94c4ed3d030a45a1832a9615d`
  (crashpad's own `DEPS` file, `chromium_git/chromium/src/third_party/zlib`), vendored in the same
  sentry-native 0.17.1 zip.
- **License:** Zlib
- **Source:** https://chromium.googlesource.com/chromium/src/third_party/zlib
- **Linked:** statically, into `crashpad_handler.exe`. Ships no file of its own.

### AntiHooking

- **Version:** not versioned; built from source in this fork.
- **License:** GPL-3.0-or-later (it is part of moonlight-qt)
- **Source:** https://github.com/Vikt0r70/moonlight-qt (the `AntiHooking/` subproject)
- **Linked:** dynamically (`AntiHooking.dll`). It lives beside the client rather than inside it,
  which is also why `scripts/build-seathub.ps1` copies it into the deploy folder explicitly.

### Microsoft Visual C++ runtime

- **Files:** `concrt140.dll`, `msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`,
  `msvcp140_atomic_wait.dll`, `msvcp140_codecvt_ids.dll`, `vccorlib140.dll`, `vcruntime140.dll`,
  `vcruntime140_1.dll`, `vcruntime140_threads.dll`
- **Version:** redistributable directory `14.44.35112`, built by toolset `14.44.35207` (Visual
  Studio 2022). The two numbers differ because Microsoft versions the redistributable separately
  from the toolset.
- **License:** Microsoft's redistributable license (Visual Studio 2022)
- **Source:** https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist
- **Taken from:** the MSVC redistributable directory, not from the build tree — the build tree's
  copies are unsigned and repackaging them would break the signature that makes them trustworthy.

### Open Sans

- **Version:** v3.003
- **License:** SIL Open Font License 1.1 (no Reserved Font Name)
- **Source:** https://github.com/googlefonts/opensans at commit
  `bd7e37632246368c60fdcbd374dbf9bad11969b6`
- **File:** `OpenSans-SemiBold.ttf`, unmodified, compiled into the client as a Qt resource
  (`app/seathub/fonts/OFL.txt`/`OpenSans-SemiBold.ttf`, registered via `seathub/fonts.qrc`,
  06.6-06/06.6-14) and read by the pluggable text rasteriser `OsdRenderer` implements (D-15/D-17,
  the one font for every SeatHub-drawn overlay).
- **Ships as:** compiled into `SeatHub.exe` (the Qt resource), with its licence text shipped
  separately at `licenses\OpenSans-OFL.txt` in the deploy folder (D-15; `scripts/build-seathub.ps1`
  copies it from `app/seathub/fonts/OFL.txt` and the deploy-folder check requires it).

Pre-existing gap, not widened by this entry: upstream's own `ModeSeven.ttf` (the SDL_ttf fallback
`OverlayManager` still falls back to when no rasteriser is set, `overlaymanager.cpp`'s
`Path::readDataFile("ModeSeven.ttf")`) has never had its own SBOM row since this file was written
in 03-06; it is not this plan's asset and this plan does not add, remove or modify it.

## Statically linked into SeatHub.exe

These have no DLL of their own; they are compiled into the client. Verified by their absence from
the deploy folder and the presence of a `.lib` in the build tree:

| Dependency | Version | License | Source |
|------------|---------|---------|--------|
| moonlight-common-c | commit `8599b60` (no release numbering) | GPL-3.0 | https://github.com/moonlight-stream/moonlight-common-c |
| qmdnsengine | 0.1.0-23-gb7a5a9f | MIT | https://github.com/cgutman/qmdnsengine |
| libsoundio | 2.0.0-4-g34bbab8 | MIT | https://github.com/andrewrk/libsoundio |
| h264bitstream | 0.2.0-26-g34f3c58 | LGPL-2.1 | https://github.com/aizvorski/h264bitstream |

License confirmation for the statically linked set: `soundio/libsoundio/LICENSE` (MIT, Expat) and
`h264bitstream/h264bitstream/LICENSE` (GNU Lesser General Public License 2.1) were read in the
submodule working trees, and the versions come from `git submodule status`. The submodules carry the
projects' own license files; `moonlight-common-c` and `qmdnsengine` are the upstream projects at the
commits above.

## Not redistributed

- **`D3Dcompiler_47.dll`** — not in the payload. The deploy step passes
  `--no-system-d3d-compiler` and Qt relies on the copy Windows supplies in `System32` (Windows 8.1
  and later). Nothing is redistributed, so there is no obligation here.
- **Qt Concurrent** — Qt 6.11.2 ships it, but `app/app.pro` does not link it (`QT += core quick
  network quickcontrols2 svg websockets`), so no `Qt6Concurrent.dll` is deployed.
- **Code-signing certificate** — there is none, by decision (D-43).

## What the licenses require of this product

- **GPL-3.0 applies to the whole distributed work**, because a GPL component
  (`moonlight-common-c`) is linked into the client statically and `AntiHooking.dll` is GPL. So the
  package ships `GPL-3.0.txt` and the corresponding-source offer, and the installer shows both on
  its license page — the offer is assembled into `installer/packages/com.seathub.client/meta/license.txt`
  by `scripts/build-seathub.ps1` from `GPL-3.0.txt` and `installer/WRITTEN-OFFER.txt`. The license
  file alone is **not** compliance: the offer is what makes the source reachable (Pitfall 9).
- **The written offer points at the fork repository**, which is why making
  `Vikt0r70/moonlight-qt` public is a release-prep blocker: an offer that resolves for nobody
  satisfies no one. Recorded in `docs/runbooks/client-release.md`.
- **LGPL-3.0 / LGPL-2.1-or-later / MIT / BSD / Apache-2.0 / Zlib components** are all dynamically
  linked as DLLs (or, for MIT/BSD-2 statically linked code with permissive terms), so the
  relinking and notice conditions are met by shipping the unmodified DLLs beside the client and this
  notice file.

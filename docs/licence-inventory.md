# Licence and source inventory

What the RentaPC client installer contains, under which licence each part is offered, where the
licence text ships, and where the corresponding source can be obtained. This document reports what
the sources say. It is not a legal opinion, and every reading that is still unclear is an `OPEN:`
line for the owner rather than a resolution. [SBOM.md](../SBOM.md) carries the per-component detail
and agrees with the tables below.

## How this was built

- The package contents come from `scripts/build-seathub.ps1` (the deploy folder) and
  `scripts/pack-seathub.ps1` (the Velopack release), read at member base 75e97c60, plus
  `app/app.pro` for the Qt modules the client links.
- Versions were cross-checked against the Windows file-version resources of the last full deploy
  folder available on the build machine (client 0.1.27). That folder predates the Velopack library
  and the `licenses\` additions, so those two items are read from the scripts instead.
- No new build was made for this document. The next release build should be compared with these
  tables (see the OPEN lines).

## What the installer is

| Part | What it is | Licence | Source |
|------|------------|---------|--------|
| `RentaPC-Setup.exe` (outer) | The RentaPC setup window: a small plain-Win32 program built from `installer/bootstrapper` in this repository. It shows the privacy notice and choice, then runs the embedded Velopack setup | GPL-3.0 (RentaPC's own code) | This repository |
| Velopack Setup (embedded) | Velopack's unchanged one-click setup program, produced by the pinned `vpk` 1.2.161 and embedded in the outer installer | MIT (Velopack) | https://github.com/velopack/velopack |
| `RentaPC-<version>-full.nupkg` | The full update package: the deploy folder below plus Velopack's helpers | see the tables below | see the tables below |
| `releases.win.json` | Velopack's manifest for the update feed | n/a (data) | n/a |

Velopack's `Update.exe` and its setup stub are unchanged vendor helpers produced by `vpk`. They are
not RentaPC files, they are not modified, and they stay unsigned.

## What the deploy folder contains

### SeatHub additions and Moonlight

| Files | Component | Version | Licence | Licence text ships as | Corresponding source |
|-------|-----------|---------|---------|-----------------------|----------------------|
| `SeatHub.exe` | RentaPC client: the SeatHub additions in `app/seathub/` and `app/gui/` plus the Moonlight engine | 0.1.x (release version) | GPL-3.0 | `licenses\GPL-3.0.txt` | https://github.com/Vikt0r70/moonlight-qt, branch `seathub/main`; `licenses\WRITTEN-OFFER.txt` |
| `AntiHooking.dll` | Moonlight's AntiHooking helper, built from this repository | not versioned | GPL-3.0-or-later (per SBOM) | `licenses\GPL-3.0.txt` | the `AntiHooking/` subproject of the same repository |
| `gamecontrollerdb.txt` | SDL_GameControllerDB data file | commit e5a5fa2 | Zlib | not shipped as a separate file | https://github.com/gabomdq/SDL_GameControllerDB |
| compiled into `SeatHub.exe` | moonlight-common-c | commit 8599b60 | GPL-3.0 | `licenses\GPL-3.0.txt` | https://github.com/moonlight-stream/moonlight-common-c |
| compiled into `SeatHub.exe` | qmdnsengine | 0.1.0-23-gb7a5a9f | MIT | not shipped as a separate file | https://github.com/cgutman/qmdnsengine |
| compiled into `SeatHub.exe` | libsoundio | 2.0.0-4-g34bbab8 | MIT | not shipped as a separate file | https://github.com/andrewrk/libsoundio |
| compiled into `SeatHub.exe` | h264bitstream | 0.2.0-26-g34f3c58 | LGPL-2.1 | not shipped as a separate file | https://github.com/aizvorski/h264bitstream |
| compiled into `SeatHub.exe` | Open Sans SemiBold | v3.003 | OFL-1.1 | `licenses\OpenSans-OFL.txt` | https://github.com/googlefonts/opensans |
| compiled into `SeatHub.exe` | `ModeSeven.ttf` (upstream fallback font, `app/resources.qrc`) | not recorded | not recorded | not shipped | `app/ModeSeven.ttf` in the repository |

### Libraries from the Moonlight prebuilts repository

These DLLs come from `libs\windows\lib\x64\` (https://github.com/cgutman/moonlight-qt-prebuilts,
pinned at `a27d6a7`). The build script copies every DLL in that folder.

| Files | Component | Version | Licence | Licence text ships as | Corresponding source |
|-------|-----------|---------|---------|-----------------------|----------------------|
| `avcodec-61.dll`, `avutil-59.dll`, `swscale-8.dll` | FFmpeg | 7.0.2 (61.3.100, 59.8.100, 8.1.100) | LGPL-2.1-or-later (string compiled into the DLLs) | not shipped as a separate file | https://ffmpeg.org/ (tag n7.0.2); prebuilts repository above |
| `dav1d.dll` | dav1d | 1.4.3 | BSD-2-Clause | not shipped as a separate file | https://code.videolan.org/videolan/dav1d |
| `libplacebo-349.dll` | libplacebo | v7.349.0 | LGPL-2.1-or-later | not shipped as a separate file | https://code.videolan.org/videolan/libplacebo |
| `opus.dll` | Opus | 1.5.2 | BSD-3-Clause | not shipped as a separate file | https://opus-codec.org/ |
| `libcrypto-3-x64.dll`, `libssl-3-x64.dll` | OpenSSL | 3.3.2 | Apache-2.0 (per SBOM) | not shipped as a separate file | https://www.openssl.org/source/ |
| `SDL2.dll` | SDL2 | 2.31.0 | Zlib | not shipped as a separate file | https://github.com/libsdl-org/SDL |
| `SDL2_ttf.dll` | SDL2_ttf | 2.22.0 | Zlib | not shipped as a separate file | https://github.com/libsdl-org/SDL_ttf |
| `discord-rpc.dll` | discord-rpc | not recorded by the DLL | MIT (per SBOM) | not shipped as a separate file | https://github.com/discord/discord-rpc |

### Sentry and Velopack libraries

| Files | Component | Version | Licence | Licence text ships as | Corresponding source |
|-------|-----------|---------|---------|-----------------------|----------------------|
| `sentry.dll` | sentry-native (built for this package: crashpad backend, winhttp transport, Qt integration off, static runtime) | 0.17.1 | MIT | not shipped as a separate file | https://github.com/getsentry/sentry-native (release zip pinned by SHA-256 in `scripts/build-seathub.ps1`) |
| `crashpad_handler.exe`, `crashpad_wer.dll` | crashpad (getsentry fork, from the sentry-native zip) | with sentry-native 0.17.1 | Apache-2.0 | not shipped as a separate file | https://github.com/getsentry/crashpad |
| inside the two crashpad files | mini_chromium (eef885b0) and zlib 1.2.12 | as listed | BSD-3-Clause; Zlib | not shipped as a separate file | https://chromium.googlesource.com/chromium/mini_chromium; https://chromium.googlesource.com/chromium/src/third_party/zlib |
| `velopack_libc_win_x64_msvc.dll` | Velopack C library (the client's update calls) | 1.2.161 | MIT | not shipped as a separate file | https://github.com/velopack/velopack (zip pinned by SHA-256 in the build script) |
| `Update.exe` and the setup stub (added by `vpk`) | Velopack helpers, unchanged vendor files | 1.2.161 | MIT (Velopack) | not shipped as a separate file | https://github.com/velopack/velopack |

### Qt 6.11.2

Qt is linked dynamically and unmodified. The client links `QT += core quick network
quickcontrols2 svg websockets` (`app/app.pro`); `windeployqt` adds what the QML imports pull in.

| Files | Component | Version | Licence | Licence text ships as | Corresponding source |
|-------|-----------|---------|---------|-----------------------|----------------------|
| `Qt6Core`, `Qt6Gui`, `Qt6Network`, `Qt6OpenGL`, `Qt6Qml`, `Qt6QmlMeta`, `Qt6QmlModels`, `Qt6QmlWorkerScript`, `Qt6Quick`, `Qt6QuickEffects`, `Qt6QuickLayouts`, `Qt6QuickShapes`, `Qt6QuickTemplates2`, `Qt6Svg`, `Qt6WebSockets` | Qt modules | 6.11.2 | LGPL-3.0 (Qt offers LGPL-3.0, GPL or a commercial licence; RentaPC uses the LGPL-3.0 offer) | not shipped as a separate file | https://download.qt.io/official_releases/qt/6.11/6.11.2/single/ |
| `Qt6QuickControls2`, `...Basic`, `...BasicStyleImpl`, `...FluentWinUI3StyleImpl`, `...Impl`, `...Material`, `...MaterialStyleImpl` | Qt Quick Controls 2 | 6.11.2 | LGPL-3.0 | not shipped as a separate file | same |
| `Qt6LabsFolderListModel`, `Qt6QuickDialogs2`, `Qt6QuickDialogs2QuickImpl`, `Qt6QuickDialogs2Utils` | Qt modules pulled in by QML imports (found in the last full deploy folder; not in the earlier SBOM list) | 6.11.2 | LGPL-3.0 | not shipped as a separate file | same |
| `platforms\qwindows.dll`, `imageformats\` (gif, ico, jpeg, svg), `iconengines\qsvgicon.dll`, `networkinformation\qnetworklistmanager.dll`, `tls\` (certonly, schannel backends), `qml\`, `translations\*.qm` | Qt plugins, QML modules, translations | 6.11.2 | LGPL-3.0 | not shipped as a separate file | same |
| `opengl32sw.dll` | Qt's bundled software OpenGL (Mesa llvmpipe based) | with Qt 6.11.2 | not read from the file | not shipped as a separate file | same |

### Microsoft runtime and fonts

| Files | Component | Version | Licence | Licence text ships as | Corresponding source |
|-------|-----------|---------|---------|-----------------------|----------------------|
| `concrt140`, `msvcp140*`, `vccorlib140`, `vcruntime140*` DLLs | Microsoft Visual C++ redistributable | see OPEN below | Microsoft redistributable terms (Visual Studio 2022) | not shipped as a separate file | n/a (Microsoft binaries, not source-distributed) |

## Where licence notices ship in the package

The package's `licenses\` folder contains exactly three files: `GPL-3.0.txt`, `WRITTEN-OFFER.txt`
and `OpenSans-OFL.txt`. The licence texts of the other components (the Qt LGPL-3.0, the LGPL-2.1
of FFmpeg and libplacebo, the MIT, BSD, Apache and Zlib notices) are named in [SBOM.md](../SBOM.md)
in the public repository but are not copied into the installed package.

OPEN: do the LGPL, MIT, BSD, Apache and Zlib components require their licence texts or notices to
sit inside the installed package itself, or does naming them in SBOM.md with their source links
suffice? This is the owner's call, with SignPath's guidance if it asks.

## GPL coverage and corresponding source

- The client is distributed under GPL-3.0. There is no dual-licence offer and no proprietary
  licence of the client is offered to anyone (SignPath research, section 5).
- The SeatHub additions in `app/seathub/` and `app/gui/` are part of the same GPL-3.0 work as the
  Moonlight engine. Their corresponding source is the public fork repository at
  https://github.com/Vikt0r70/moonlight-qt (branch `seathub/main`), and
  [installer/WRITTEN-OFFER.txt](../installer/WRITTEN-OFFER.txt) (shipped as
  `licenses\WRITTEN-OFFER.txt`) is the written offer that points there. `FORK-CHANGES.md` lists
  every file that differs from upstream.
- The private RentaPC control plane and host-side software are separate programs that talk to the
  client over a network interface. They are not part of this repository and are not shipped in the
  installer, so the client's licence terms are about what the installer contains. This document
  makes no wider statement about them.
- The upstream baseline is moonlight-stream/moonlight-qt 6.1.0, commit f786e94c, kept on the
  `upstream-main` branch.

## LGPL relinking route (Qt)

Qt ships as unmodified DLLs beside `SeatHub.exe` and in `plugins` and `qml` folders, so a customer
who wants to use a different build of Qt 6.11.2 can replace those files in the per-user install
folder and start the client again; the Qt source for the shipped version is at the link above.
An update through the in-app updater restores the shipped DLLs. The same arrangement of separate
DLLs applies to FFmpeg, libplacebo, dav1d, Opus, OpenSSL, SDL2 and SDL2_ttf. `h264bitstream`
(LGPL-2.1) is compiled into `SeatHub.exe` rather than shipped as its own DLL; the whole client
source, including that library's pinned commit, is public at the link above, so a modified copy can
be rebuilt from it.

## Build licences of FFmpeg and OpenSSL

- FFmpeg: both `avcodec-61.dll` and `avutil-59.dll` carry the compiled-in string
  `LGPL version 2.1 or later`. A build with GPL options enabled would say GPL instead.
- OpenSSL 3.3.2: its project licence is Apache-2.0. The DLLs are prebuilt by the Moonlight
  prebuilts project.

OPEN: the exact FFmpeg configure options of the prebuilt DLLs are not recorded in this repository,
so whether any optional third-party code inside them carries terms beyond the LGPL-2.1-or-later
string is unconfirmed. Confirm with the prebuilts repository's build recipe.

OPEN: the OpenSSL build configuration (and any bundled third-party code in the prebuilt) is not
recorded here, so only the project's Apache-2.0 licence is stated.

## Other open readings

OPEN: SignPath's wording on commercial or dual-licensed projects. RentaPC is GPL-3.0 only with no
dual-licence offer, but the owner runs a paid rental service that uses the client. Ask SignPath
whether that commercial setting matters for its open-source programme.

OPEN: AntiHooking terms. SBOM.md records GPL-3.0-or-later because it is part of moonlight-qt, but
the DLL carries no version resource and no licence string of its own.

OPEN: discord-rpc terms and version. SBOM.md records MIT, but the prebuilt DLL carries no version
resource and no licence string; the version is recorded as the prebuilts commit `a27d6a7`.

OPEN: `ModeSeven.ttf`, the upstream fallback font compiled into the client through `app/resources.qrc`,
has no licence row in SBOM.md or in this document. Its origin and terms are not recorded in the
repository.

OPEN: Visual C++ runtime version. The last full deploy folder carries 14.44.35211.0 while SBOM.md
says redistributable directory 14.44.35112. Confirm against the next release build.

OPEN: `opengl32sw.dll` is shipped (the build script drops `--no-opengl-sw`), but its licence was
not read from the file.

OPEN: the comment beside `--no-system-d3d-compiler` in `scripts/build-seathub.ps1` says "ship Qt's
D3Dcompiler_47.dll", but the flag does the opposite, and the last deploy folder contains no
`D3Dcompiler_47.dll`. SBOM.md therefore lists it as not redistributed. Confirm on the next build.

OPEN: the Qt licence of each module was taken from Qt's published offer for Qt 6 (LGPL-3.0,
GPL, commercial). A module-by-module read of the shipped module licence headers was not done.

OPEN: Velopack's `Update.exe` and setup stub are Rust programs with their own third-party crates.
Their dependency licences were not inventoried; only Velopack's own MIT licence is stated.

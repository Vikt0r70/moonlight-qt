# Linux Qt Core-only suites (T059)

Recorded 2026-10-17 on the Ubuntu 24.04.5 LTS cloud container (g++ 13, GNU make, qmake 3.1),
member base `f448ce517ea14159f068081ddcd62455be28019c`. Every suite in `tests/` was built with
Ubuntu's `qmake6` out of tree and run with `QT_QPA_PLATFORM=offscreen`; the results below are
what happened, not what was expected.

LINUX-QT-CORE-SUITES: buildable
LINUX-QT-VERSION: 6.4.2
LINUX-SUITES: tst_attempt_vocab tst_update_retry
LINUX-DATE: 2026-10-17
LINUX-OS: Ubuntu 24.04.5 LTS (qt6-base-dev 6.4.2+dfsg-21.1build5)

Run them with `bash tests/linux/run-core-suites.sh --provision tst_attempt_vocab tst_update_retry`
(prints `LINUX_CORE_SUITES_PASSED` as its last line on success; `--provision` installs only
`qt6-base-dev` and `qt6-base-dev-tools`; exit 70 when no Qt 6 toolchain is present and
`--provision` is not given). The runner refuses (exit 2) any suite whose `.pro` declares a Qt
module beyond `core testlib`.

## Read this before relying on a Linux pass

The Linux Qt is Ubuntu's distribution build (6.4.2), not the Windows 6.11.2 msvc2022_64 build
the owner's PC uses. A Linux pass is evidence for pure logic only (vocabulary mapping, retry
state). The Windows named-suite run (`tests\run-all-suites.cmd`) stays the authority for wiring,
platform behaviour and everything the Linux build cannot compile.

## Passing on Linux (built and run here)

- `tst_attempt_vocab`: 11 passed, 0 failed (`QT += core testlib`).
- `tst_update_retry`: 9 passed, 0 failed (`QT += core testlib`).

## Core-only suites that built but did not pass on Linux

Both declare `QT += core testlib` and compile, so they qualify for the runner, but they fail at
run time on Linux and are therefore not listed in `LINUX-SUITES`.

- `tst_install_journal`: 18 passed, 13 failed. First failure `tests/tst_install_journal.cpp(105)`.
  Cause isolated with a 10-line Qt program: the test helper `put()` calls
  `QFile::setFileTime(..., FileModificationTime)` while the file is still open, and on Linux
  closing the file afterwards resets the modification time to the real clock (observed: set to
  2026-09-30T12:00:00Z, read back as the real time after close). The suite's age-based
  assertions then see the wrong ages. A test-side portability defect, not a defect in
  `install_journal.cpp`; fixing it is a change to a source file outside this task.
- `tst_d28_boundary`: 4 passed, 4 failed. The suite grades the git checkout against the pinned
  upstream ref `origin/v6.1.0`, which does not exist in this clone
  (`git rev-parse --verify origin/v6.1.0` fails; the suite sees git exit 128). It needs a clone
  that carries the upstream tag, so it is environment-bound, not a Linux Qt problem.

## Built and passed here but does not qualify for the runner

- `tst_liveness` declares `QT += core testlib network`, builds on Ubuntu Qt 6.4.2 and passed
  25 of 25 in one run on 2026-10-17. The runner's rule (a suite qualifies only with `core testlib`
  and nothing else) excludes it, so it is not in `LINUX-SUITES`; it is recorded as an
  observation only.

## Not proven on Linux

The `WINDOWS-ONLY:` lines below list every other suite with the concrete reason it is not
proven on Linux (first compiler or qmake error observed on 2026-10-17, or the Qt module that is
not part of the provisioned toolchain). Only `qt6-base-dev` and `qt6-base-dev-tools` were
installed, so the QML, Quick, QuickControls2 and WebSockets modules are not present; whether
they could be provisioned was not tried because the runner is limited to `core testlib`.

WINDOWS-ONLY: tst_token_store: app/seathub/token_store.cpp:22: fatal error: windows.h: No such file or directory (Windows DPAPI via crypt32; .pro links -lcrypt32)
WINDOWS-ONLY: tst_teardown: app/seathub/token_store.cpp:22: fatal error: windows.h: No such file or directory (DPAPI token store; .pro links -lcrypt32)
WINDOWS-ONLY: tst_threading: app/seathub/token_store.cpp:22: fatal error: windows.h: No such file or directory (DPAPI token store; .pro links -lcrypt32)
WINDOWS-ONLY: tst_log_tee: app/seathub/log_tee.h:35: fatal error: SDL.h: No such file or directory (SDL headers come from libs/windows/include, added only under win32)
WINDOWS-ONLY: tst_log_shipper: app/seathub/log_tee.h:35: fatal error: SDL.h: No such file or directory (compiles log_tee.cpp; SDL headers only under win32)
WINDOWS-ONLY: tst_osd_render: app/seathub/log_tee.h:35: fatal error: SDL.h: No such file or directory (SDL headers only under win32)
WINDOWS-ONLY: tst_pairing: app/seathub/log_tee.h:35: fatal error: SDL.h: No such file or directory (SDL headers only under win32)
WINDOWS-ONLY: tst_telemetry: app/seathub/log_tee.h:35: fatal error: SDL.h: No such file or directory (also links sentry-native from SEATHUB_SENTRY_DIR and -lcrypt32)
WINDOWS-ONLY: tst_engine_seam: app/seathub/engine_status.cpp:3: fatal error: SDL.h: No such file or directory (SDL headers only under win32)
WINDOWS-ONLY: tst_stream_stats: app/seathub/engine_termination.cpp:3: fatal error: SDL.h: No such file or directory (SDL headers only under win32)
WINDOWS-ONLY: tst_hud_bitmap: app/seathub/hud_overlay.h:9: fatal error: SDL.h: No such file or directory (also links d3d11 and dxgi)
WINDOWS-ONLY: tst_overlay_injection: app/streaming/video/overlaymanager.h:7: fatal error: SDL.h: No such file or directory (engine overlay code; also SDL2_ttf)
WINDOWS-ONLY: tst_control_plane: tests/tst_control_plane.cpp:1938:39: error: 'class QNetworkRequest' has no member named 'transferTimeoutAsDuration' (needs a Qt newer than Ubuntu's 6.4.2; also declares network)
WINDOWS-ONLY: tst_facade_wiring: qmake: Project ERROR: Unknown module(s) in QT: qml websockets (also links SDL2, crypt32, advapi32 and sentry-native)
WINDOWS-ONLY: tst_session_websocket: qmake: Project ERROR: Unknown module(s) in QT: websockets
WINDOWS-ONLY: tst_sse_client: qmake: Project ERROR: Unknown module(s) in QT: websockets
WINDOWS-ONLY: tst_error_map: qmake: Project ERROR: Unknown module(s) in QT: qml quick quickcontrols2
WINDOWS-ONLY: tst_settings_bridge: qmake: Project ERROR: Unknown module(s) in QT: qml quick quickcontrols2 (also uses libs/windows/include)
WINDOWS-ONLY: tst_ui_screens: qmake: Project ERROR: Unknown module(s) in QT: qml quick quickcontrols2 (also uses libs/windows/include)
WINDOWS-ONLY: tst_update_feed: qmake: Project ERROR: Unknown module(s) in QT: qml quick quickcontrols2 (also links advapi32 under win32)
WINDOWS-ONLY: tst_install_journal: builds on Linux but 13 of 31 checks fail at run time (tests/tst_install_journal.cpp:105 first; setFileTime on an open file is undone by close on Linux, see above)
WINDOWS-ONLY: tst_d28_boundary: builds on Linux but 4 of 8 checks fail at run time (needs git ref origin/v6.1.0, absent in this clone, see above)
WINDOWS-ONLY: tst_liveness: not a Core-only suite (QT += core testlib network); builds and passed 25 of 25 on Linux once, kept off the runner by the core-testlib-only rule

## Caveats

- 2 of 25 suites are proven on Linux. `tst_attempt_vocab` is the only suite whose `.pro` and
  sources are free of every Windows and sentry dependency and that the planned Core-only units
  can copy as a template.
- Out-of-tree builds leave nothing in the checkout: the runner builds in `mktemp -d` and removes
  it on exit.
- `tst_update_retry` passed with `XDG_*_HOME` pointed at a scratch directory; the runner sets
  them so no suite touches the host's real user directories.

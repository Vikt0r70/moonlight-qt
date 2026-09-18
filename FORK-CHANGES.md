# Fork changes

This file enumerates every file modified from the pinned upstream tag, `v6.1.0`
(commit `f786e94c7b2f943e24e65d7d74deb539b827fc84`), with a reason for each change.

Per SeatHub's fork boundary (`ADR-0044`, D-24/D-26/D-32): all SeatHub-specific code lives in
**new files** under `app/seathub/` (the C++ bridge module) and `app/gui/` (QML views only).
Upstream files are edited only where this file says so — every entry below corresponds to a
named exception in `.github/workflows/seathub-build-win.yml`'s CI diff gate, and the gate fails
any build where a modified file under `app/streaming/` is not listed here.

No decoder, renderer, audio, or input file is ever modified. The only two categories of allowed
exception are:

1. **Overlay compositor** (`app/streaming/video/overlaymanager.*`) — gains a bitmap input and
   per-value visibility control for the SeatHub HUD (D-28's original exception).
2. **`session.cpp`'s window title string literal** (non-Darwin `#else` branch, ~line 1829) — the
   only other permitted engine-file edit (`ADR-0046`).

## Modified files

Added by each plan (Plan 03-02 onward) in the same commit that makes the change.

| File | Category | Reason | Plan |
|------|----------|--------|------|
| `app/streaming/session.cpp` | engine (ADR-0046 exception) | SDL stream window title literal changed from `" - Moonlight"` to `" - SeatHub"` in the non-Darwin `#else` branch. The title is passed into `SDL_CreateWindow()` itself, so a runtime `SDL_SetWindowTitle()` could only rename the window after it already existed as "&lt;PC&gt; - Moonlight" — it cannot prevent the initial flash of the brand, and Phase 3 requires Moonlight never be visible. Marker comment `// SeatHub: D-28 exception, see ADR-0046` sits on the changed line for future upstream merges. | 03-02 |
| `app/main.cpp` | bridge | Register SeatHub types + SeatHub QML entrypoint + brand replacement per brand audit. `SeatHubClient` / `SessionLifecycle` registration, the `Tokens`/`Metrics` QML singletons, and every user-visible brand literal (`SDL_APP_NAME`, `SDL_AUDIO_DEVICE_APP_NAME`, application/organization name, crash-dump and log filenames, desktop file name, Wayland/X11 WMCLASS) changed to SeatHub. | 03-02 |
| `app/app.pro` | packaging | Brand replacement: `TARGET` (both branches), `QMAKE_TARGET_COMPANY` / `_DESCRIPTION` / `_PRODUCT`, `RC_ICONS` → `seathub.ico`, the embedded manifest path → `SeatHub.exe.manifest`, `APP_BUNDLE_RESOURCES.files` → `seathub.icns`, and the `unix:!macx` desktop/appstream asset paths → `com.seathub.SeatHub.*`. All `moonlight-common-c` submodule link and include lines are deliberately unchanged — that is the upstream library's name, not our brand. | 03-02 |
| `app/gui/main.qml` | UI | Replaced the stock moonlight-qt launcher with SeatHub's entry point: `SeatHubClient` facade wiring, a view per app state, the D-01 visibility host window, and the `Ctrl+Alt+Shift+Q` interrupt shortcut (D-02). Removes the PC/app browser route, the `ErrorMessageDialog.qml` route, and consumption of the CLI `initialView` context property, so no Moonlight-worded launcher or dialog window can appear during connect, stream or teardown. | 03-02 |
| `app/qml.qrc` | packaging | Registers the new `app/gui/` QML files (`Tokens.qml`, `Metrics.qml`, `SessionSegue.qml`, `SignInScreen.qml`, `HomeScreen.qml`, `ErrorScreen.qml`, `SeatHubOTPField.qml`) so they are available at `qrc:/gui/`. | 03-02 |
| `app/res/moonlight.svg` | brand asset (content only) | Content replaced with the SeatHub client mark at the *same* resource path. `app/streaming/session.cpp` references `:/res/moonlight.svg` for the SDL stream window icon, so replacing the content gives SeatHub the stream window's icon **without** editing an engine file and **without** widening ADR-0046's exception set. | 03-02 |
| `app/Moonlight.exe.manifest` → `app/SeatHub.exe.manifest` | packaging | Renamed, and its `<description>` changed from "Moonlight Game Streaming" to "SeatHub Streaming Client". The file is embedded into the executable by `QMAKE_LFLAGS`, so the old name was a Moonlight string in the shipped binary's manifest. | 03-02 |

### Upstream files deliberately left in place but no longer referenced

These are **not modified** and therefore need no diff-gate entry. They are listed so a future
reader does not mistake them for live code:

| File | Why it is still on disk |
|------|-------------------------|
| `app/gui/StreamSegue.qml` | Superseded by `app/gui/SessionSegue.qml`. Nothing loads it after `main.qml` was replaced, but deleting it would grow the merge diff against upstream for no functional gain. |
| `app/gui/ErrorMessageDialog.qml`, `NavigableMessageDialog.qml`, `NavigableDialog.qml` | Still referenced by the untouched `PcView.qml` / `AppView.qml` / `SettingsView.qml`, which are also unreachable now that `main.qml` no longer routes to them. No code path opens them. |
| `app/gui/PcView.qml`, `AppView.qml`, `SettingsView.qml`, `Cli*Segue.qml`, `QuitSegue.qml` | Unreachable from SeatHub's `main.qml`. Plan 03-04 (settings) and 03-05 (HUD) decide what is replaced rather than merely unreferenced. |
| `app/moonlight.ico`, `app/moonlight.icns`, `app/deploy/linux/com.moonlight_stream.Moonlight.desktop`, `app/deploy/linux/com.moonlight_stream.Moonlight.appdata.xml` | Superseded by `seathub.ico` / `seathub.icns` / `com.seathub.SeatHub.desktop` / `com.seathub.SeatHub.appdata.xml`, which `app.pro` now names. Left on disk to keep the diff free of deletions. |

## New files (not modifications, informational only)

SeatHub's own C++/QML files under `app/seathub/` and `app/gui/` are additions, not modifications
to upstream files — they do not require an entry in the table above and are not subject to the
CI diff gate's exception list (the gate only checks files that exist in the upstream tag's tree).

| File | Category | Plan |
|------|----------|------|
| `app/seathub/seathub_client.h`, `app/seathub/seathub_client.cpp` | bridge (D-35 facade) | 03-02 |
| `app/seathub/session_lifecycle.h`, `app/seathub/session_lifecycle.cpp` | bridge (engine seam) | 03-02 |
| `app/seathub/error_map.h`, `app/seathub/error_map.cpp` | bridge (D-51 error model) | 03-02 |
| `app/gui/SessionSegue.qml` | UI (D-01/D-03 visibility + lifecycle) | 03-02 |
| `app/gui/SignInScreen.qml` | UI (D-55 sign-in shell) | 03-02 |
| `app/gui/HomeScreen.qml`, `app/gui/ErrorScreen.qml` | UI | 03-02 |
| `app/gui/SeatHubOTPField.qml` | UI primitive | 03-02 |
| `app/gui/Tokens.qml` | design tokens — byte-for-byte copy of `seathub-web`'s generated `npm run tokens:build` output (Phase 2.1, D-20/D-54). Never hand-edit; regenerate in `seathub-web` and copy. | 03-02 |
| `app/gui/Metrics.qml` | design tokens — the fork-side number companion `Tokens.qml` needs (it carries spacing/type values as CSS-shaped strings, and QML's `spacing`/`radius`/`font.pixelSize` are numbers). Hand-written. | 03-02 |
| `app/seathub.ico`, `app/seathub.icns` | brand assets | 03-02 |
| `app/deploy/linux/com.seathub.SeatHub.desktop`, `app/deploy/linux/com.seathub.SeatHub.appdata.xml` | packaging | 03-02 |

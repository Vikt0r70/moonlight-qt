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
| `app/main.cpp` | bridge | `SettingsBridge` and `UpdateFeedClient` registered as uncreatable QML types in the `SeatHub` module, so the QML layer can name the types it reaches through `SeatHubClient.settings` / `.updates` (D-35) and cannot construct them itself. | 03-04 |
| `app/app.pro` | packaging | Brand replacement: `TARGET` (both branches), `QMAKE_TARGET_COMPANY` / `_DESCRIPTION` / `_PRODUCT`, `RC_ICONS` → `seathub.ico`, the embedded manifest path → `SeatHub.exe.manifest`, `APP_BUNDLE_RESOURCES.files` → `seathub.icns`, and the `unix:!macx` desktop/appstream asset paths → `com.seathub.SeatHub.*`. All `moonlight-common-c` submodule link and include lines are deliberately unchanged — that is the upstream library's name, not our brand. | 03-02 |
| `app/app.pro` | packaging | Four SeatHub bridge translation units added to `SOURCES`/`HEADERS` (`seathub/settings_bridge.*`, `seathub/update_feed_client.*`, plus the `seathub/seathub_version.h` header). Additions only — no upstream source list entry is removed or reordered. | 03-04 |
| `app/app.pro` | packaging | The HUD producer added to `SOURCES`/`HEADERS` (`seathub/hud_overlay.*`). Additions only. | 03-05 |
| `app/gui/main.qml` | UI | Replaced the stock moonlight-qt launcher with SeatHub's entry point: `SeatHubClient` facade wiring, a view per app state, the D-01 visibility host window, and the `Ctrl+Alt+Shift+Q` interrupt shortcut (D-02). Removes the PC/app browser route, the `ErrorMessageDialog.qml` route, and consumption of the CLI `initialView` context property, so no Moonlight-worded launcher or dialog window can appear during connect, stream or teardown. | 03-02 |
| `app/gui/main.qml` | UI | Settings added as a view inside the home state (`SeatHubClient.inSettings`, so a session ending while the page is open leaves the page in place), and the forced-update modal instantiated outside the view `Loader` so no view change can take it away mid-update (D-41). The feed check runs once on launch. | 03-04 |
| `app/qml.qrc` | packaging | Registers the new `app/gui/` QML files (`Tokens.qml`, `Metrics.qml`, `SessionSegue.qml`, `SignInScreen.qml`, `HomeScreen.qml`, `ErrorScreen.qml`, `SeatHubOTPField.qml`) so they are available at `qrc:/gui/`. | 03-02 |
| `app/qml.qrc` | packaging | Registers the settings page and its control primitives (`SettingsPage.qml`, `SeatHubToggle.qml`, `SeatHubSelect.qml`, `SeatHubNumberField.qml`, `SeatHubReadOnlyRow.qml`) and `ForcedUpdateModal.qml`. Additions only. | 03-04 |
| `app/res/moonlight.svg` | brand asset (content only) | Content replaced with the SeatHub client mark at the *same* resource path. `app/streaming/session.cpp` references `:/res/moonlight.svg` for the SDL stream window icon, so replacing the content gives SeatHub the stream window's icon **without** editing an engine file and **without** widening ADR-0046's exception set. | 03-02 |
| `app/streaming/video/overlaymanager.h` | engine (D-28 overlay-compositor exception) | Declares one new public method, `bool updateOverlaySurface(OverlayType type, SDL_Surface* surface)`, with its contract in a comment block. The class could only accept **text** (every surface it published was rasterised by SDL_ttf from a `char[512]`), so the SeatHub HUD had no way in. `ADR-0045` records why the HUD cannot be a QML bitmap instead (the Qt window is hidden for the whole session, so `QQuickItem::grabToImage()` is structurally impossible) and why this path is the one that preserves the single-window requirement (`STREAM-01`) for free: `D3D11VARenderer::renderFrame()` already composites the overlays into the stream's own swapchain before `Present()`. No existing declaration, enum, or behaviour is changed. | 03-05 |
| `app/streaming/video/overlaymanager.cpp` | engine (D-28 overlay-compositor exception) | Implements `updateOverlaySurface()`: refuses and frees a surface that is not `SDL_PIXELFORMAT_ARGB8888` or that needs locking (every renderer's upload path asserts exactly that, on a render thread), refuses and frees one published while the overlay is disabled, otherwise publishes it through the same `SDL_AtomicSetPtr` swap the text path uses, frees the previous surface, and notifies the renderer **exactly once** by calling `m_Renderer->notifyOverlayUpdated(type)` directly — deliberately not through `setOverlayTextUpdated()`, which would rasterise the text field and overwrite the bitmap just published. Ownership transfers in every case (including refusal) so a caller cannot leak by ignoring the return value. Additive: no existing function's body changes. | 03-05 |
| `app/Moonlight.exe.manifest` → `app/SeatHub.exe.manifest` | packaging | Renamed, and its `<description>` changed from "Moonlight Game Streaming" to "SeatHub Streaming Client". The file is embedded into the executable by `QMAKE_LFLAGS`, so the old name was a Moonlight string in the shipped binary's manifest. | 03-02 |

Nothing under `app/streaming/audio/`, `app/streaming/input/` or `app/streaming/video/` (other than
the overlay compositor exception, which Plan 03-05 is the first and so far only plan to exercise —
`app/streaming/video/overlaymanager.h` and `.cpp`) is modified by any plan, and Plan 03-04 adds
none. The settings page reads and writes upstream's `StreamingPreferences` through its public
members only — `app/settings/streamingpreferences.*` is **not** modified, which is why the
settings audit can claim write-through without a second store (D-12, STREAM-02). The HUD itself
adds no further engine edits: it lives in `app/seathub/hud_overlay.*` and reaches the swapchain
only through the public `OverlayManager` API.

### Upstream files deliberately left in place but no longer referenced

These are **not modified** and therefore need no diff-gate entry. They are listed so a future
reader does not mistake them for live code:

| File | Why it is still on disk |
|------|-------------------------|
| `app/gui/StreamSegue.qml` | Superseded by `app/gui/SessionSegue.qml`. Nothing loads it after `main.qml` was replaced, but deleting it would grow the merge diff against upstream for no functional gain. |
| `app/gui/ErrorMessageDialog.qml`, `NavigableMessageDialog.qml`, `NavigableDialog.qml` | Still referenced by the untouched `PcView.qml` / `AppView.qml` / `SettingsView.qml`, which are also unreachable now that `main.qml` no longer routes to them. No code path opens them. |
| `app/gui/PcView.qml`, `AppView.qml`, `SettingsView.qml`, `Cli*Segue.qml`, `QuitSegue.qml` | Unreachable from SeatHub's `main.qml`. Plan 03-04 answered the settings half of this: `SettingsView.qml` is **replaced by `app/gui/SettingsPage.qml`** and is deliberately left on disk, unmodified, so the diff against upstream stays free of deletions — the upstream file still references `NavigableDialog.qml` and friends, so removing it would grow the diff for no functional gain. Plan 03-05 answers the HUD half. |
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
| `app/seathub/settings_bridge.h`, `app/seathub/settings_bridge.cpp` | bridge (setting write-through: `StreamingPreferences` reads/writes, the streaming guard, the launch-only quality-profile overrides, the D-14 fallback reporting) | 03-04 |
| `app/seathub/update_feed_client.h`, `app/seathub/update_feed_client.cpp` | bridge (`GET /api/releases/{app}` client, version comparison, checksum-verified download) | 03-04 |
| `app/seathub/seathub_version.h` | bridge — SeatHub's own version constant (D-46); the feed's rows are compared against this, not against upstream's `app/version.txt`, which stays upstream's | 03-04 |
| `app/seathub/hud_overlay.h`, `app/seathub/hud_overlay.cpp` | bridge — the in-session HUD producer (D-56 subset: duration timer + the End session affordance, with D-04's auto-hide). Renders with `QPainter` into a `QImage::Format_ARGB32` and hands it to an injected publisher, so the file needs no engine header and is linkable into a test with no `Session` in it. `SeatHubClient` injects the publisher that publishes through `OverlayManager::updateOverlaySurface()`, which is the only reason the engine's overlay exception is reachable at all. It is **not** an overlay manager and it does **not** create a window: ADR-0045 records why the compositor is the engine's own. | 03-05 |
| `app/gui/SessionSegue.qml` | UI (D-01/D-03 visibility + lifecycle) | 03-02 |
| `app/gui/SignInScreen.qml` | UI (D-55 sign-in shell) | 03-02 |
| `app/gui/HomeScreen.qml`, `app/gui/ErrorScreen.qml` | UI | 03-02 |
| `app/gui/SeatHubOTPField.qml` | UI primitive | 03-02 |
| `app/gui/SettingsPage.qml` | UI — the flat settings page: five groups (D-48), every audited `[streamsettings]` key a control, `capture-system-keys` as checkbox + dropdown (ADR-0042). Replaces upstream's unreachable `SettingsView.qml`. | 03-04 |
| `app/gui/SeatHubToggle.qml`, `app/gui/SeatHubSelect.qml`, `app/gui/SeatHubNumberField.qml`, `app/gui/SeatHubReadOnlyRow.qml` | UI primitives — labelled rows for the settings page. Options always come from the bridge's catalogue, never a second copy in QML. | 03-04 |
| `app/gui/ForcedUpdateModal.qml` | UI — the blocking forced-update modal (D-41); it has no dismissal affordance by design | 03-04 |
| `app/gui/Tokens.qml` | design tokens — byte-for-byte copy of `seathub-web`'s generated `npm run tokens:build` output (Phase 2.1, D-20/D-54). Never hand-edit; regenerate in `seathub-web` and copy. | 03-02 |
| `app/gui/Metrics.qml` | design tokens — the fork-side number companion `Tokens.qml` needs (it carries spacing/type values as CSS-shaped strings, and QML's `spacing`/`radius`/`font.pixelSize` are numbers). Hand-written. | 03-02 |
| `app/seathub.ico`, `app/seathub.icns` | brand assets | 03-02 |
| `app/deploy/linux/com.seathub.SeatHub.desktop`, `app/deploy/linux/com.seathub.SeatHub.appdata.xml` | packaging | 03-02 |
| `tests/tst_error_map.cpp`, `tests/tst_error_map.pro` | test (new top-level `tests/` tree — upstream ships none) | 03-02 |
| `tests/tst_settings_bridge.cpp`, `tests/tst_settings_bridge.pro` | test — write-through against upstream's real `StreamingPreferences`, the streaming write guard, the in-memory session overrides, and the settings page loading | 03-04 |
| `tests/tst_update_feed.cpp`, `tests/tst_update_feed.pro` | test — feed parsing, semantic version comparison, SHA-256 verification against published vectors, the stream-time block, and the modal rendering | 03-04 |
| `tests/tst_overlay_injection.cpp`, `tests/tst_overlay_injection.pro` | test — the semantics of `OverlayManager::updateOverlaySurface()` against the real `OverlayManager` and a mock `IOverlayRenderer` (`ADR-0045` proof b) | 03-05 |
| `tests/tst_hud_bitmap.cpp`, `tests/tst_hud_bitmap.pro` | test — the pixel-format contract end to end: `QImage::Format_ARGB32` → `SDL_PIXELFORMAT_ARGB8888` → the exact `CreateTexture2D` code from `d3d11va.cpp` → `CopyResource` + `Map` byte comparison (`ADR-0045` proof a, and proof c for the HUD producer) | 03-05 |

## Upstream files that must never be modified

For the avoidance of doubt, the CI diff gate fails the build if any file under
`app/streaming/`, `app/streaming/video/`, `app/streaming/audio/` or `app/streaming/input/`
differs from the pinned tag except the two exception categories above. That deliberately
covers the decoder, renderer, audio and input code, and the one file in that tree this fork
touches is `session.cpp` — for its window-title literal only.

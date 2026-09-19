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
| `app/app.pro` | packaging | Two new dependencies, both additive: `websockets` added to the `QT +=` line, and `crypt32.lib` added to the Win32 system-library line. `Qt6WebSockets` is what `app/seathub/session_websocket.cpp` opens the control plane's `/ws/session/{session_id}` channel with (D-29); `crypt32` is DPAPI (`CryptProtectData` / `CryptUnprotectData`), which is the D-30 credential store and has no substitute on Windows. | 03-03 |
| `app/app.pro` | packaging | Seven SeatHub bridge translation units added to `SOURCES`/`HEADERS` (`seathub/control_plane_client.*`, `seathub/token_store.*`, `seathub/session_websocket.*`, `seathub/pairing_controller.*`, `seathub/teardown_controller.*`, `seathub/liveness_timer.*`, `seathub/authorized_through_timer.*`). Additions only - no upstream source list entry is removed or reordered. | 03-03 |
| `app/app.pro` | packaging | Two more SeatHub bridge translation units added to `SOURCES`/`HEADERS` (`seathub/pairing_seam.*` and `seathub/pairing_handshake.*`) - the production pairing seam 03-03 left as an interface and the upstream handshake it drives. Additions only. | 03-03 (gap closure) |
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
| `app/seathub/control_plane_client.h`, `app/seathub/control_plane_client.cpp` | bridge - the control-plane HTTP client (D-29). Every route is one of the eight `docs/spec/openapi.yaml` 1.6.0 documents for a customer bearer token: OTP request/verify, refresh, session create/fetch, `GET /api/sessions/{id}/pairing`, `POST /api/sessions/{id}/liveness`, `POST /api/sessions/{id}/end`. No Sunshine admin route appears anywhere on this client's wire - not `/api/pin`, not `/api/pin-token`, not `/api/clients/*`; the planner named those, the contract has none of them, and they are the Node Agent's (`COVERAGE.md`). `classify()` is Pitfall 4 as code: `ok` is set from the response body's `status` field and never from the HTTP status line, so an HTTP 200 carrying `status:false` is a failure. `moveToOwnThread()` gives it a thread with a running event loop, which a stream needs because upstream suspends Qt processing for the whole session (`app/streaming/session.cpp:1965-1966`) - a main-thread `QTimer` or `QNetworkAccessManager` would be dead for exactly the interval liveness and the session channel exist to serve. | 03-03 |
| `app/seathub/token_store.h`, `app/seathub/token_store.cpp` | bridge - the DPAPI credential store (D-30). User scope, deliberately not machine scope: the client runs as the customer's own interactive user, so a machine-scope blob would be readable by every account on the machine (`docs/spec/agents.md` justifies machine scope for *agent* tokens running as LocalSystem; that reasoning does not transfer). `CRYPTPROTECT_UI_FORBIDDEN`, one blob per credential under `%APPDATA%/SeatHub`, written through `QSaveFile` so a crash cannot leave half a credential, decrypted buffers wiped with `SecureZeroMemory`. There is deliberately no plaintext fallback. `clearAll()` is what makes STREAM-10's "keeps no stored rig, address or pairing of its own" true, and teardown refuses to report success while it returns false. | 03-03 |
| `app/seathub/session_websocket.h`, `app/seathub/session_websocket.cpp` | bridge - the control plane's `/ws/session/{session_id}` channel (D-29) over `QWebSocket`. Routes `session.state`, `session.billing`, `session.warning` and `error` to typed signals; an unknown `type` is a logged no-op, so a future server-side addition cannot break an older client; a malformed frame is reported and does not take the channel down. `sslErrors` is deliberately **not** ignored - the control plane presents a publicly-trusted certificate, and Sunshine's self-signed one belongs to a different socket this client never opens. Reconnect backoff is exponential and capped at 30 s, because D-33's promise is "keep streaming and keep retrying" and an uncapped backoff decays into "stop retrying". | 03-03 |
| `app/seathub/pairing_controller.h`, `app/seathub/pairing_controller.cpp` | bridge - silent pairing (D-21/D-22, STREAM-03). Takes the session's PIN from `GET /api/sessions/{id}/pairing`, polls at the D-08-locked 250 ms, fails closed at the 90 s deadline with a reference the error screen can render, and hands the PIN to an injected `PairingSeam` and nowhere else - no signal carries it and no accessor returns it, so this client has no PIN surface for a view to render, which is STREAM-03 in its strongest form. The seam is an interface rather than a call into `app/backend/computermanager.h`, so upstream's pairing cryptography stays upstream (03-RESEARCH §Pairing sequence step 3) and no engine file is edited. A handshake that reports success without a client UUID is treated as a refusal: the UUID is the only thing teardown can verify against (Pitfall 3, D-07). | 03-03 |
| `app/seathub/teardown_controller.h`, `app/seathub/teardown_controller.cpp` | bridge - teardown (D-10, STREAM-10). Encodes disable, then remove, then verify as a `TeardownStage` enum that only ever advances strictly forward, so Pitfall 5 is a property the code enforces rather than a comment a later edit can silently reorder. The three rig-side steps are the Node Agent's - this client holds no Sunshine admin credential and calls no Sunshine admin route (03-RESEARCH §Teardown order) - so the controller waits on the control plane's session record, applies `TEARDOWN_GRACE_SECONDS` and reports `TEARDOWN_TIMEOUT` rather than a normal end, and refuses to report success while the DPAPI store still holds anything. | 03-03 |
| `app/seathub/liveness_timer.h`, `app/seathub/liveness_timer.cpp` | bridge - the D-31 liveness report: every 10 s, carrying `state` and `error_code` per ADR-0041/D-34. The important behaviour is a negative one: a failed report never ends the session. The media path runs straight from the rig to this PC and never through the control plane, so an undeliverable heartbeat says nothing about whether the stream works; after the 30 s grace the timer raises a non-fatal warning and keeps retrying, rather than turning a control-plane outage into a customer-visible one (D-33). | 03-03 |
| `app/seathub/authorized_through_timer.h`, `app/seathub/authorized_through_timer.cpp` | bridge - the D-33/WR-04 billing-safety hard stop, and the only place a session is ended locally on a timer. Tracks the session token's `authorized_through` horizon, re-arms when the control plane extends it, and fires when it elapses - so an unreachable control plane costs a warning and a retry, not free streaming, and a session never runs past the horizon the server approved. | 03-03 |
| `tests/tst_control_plane.cpp`, `tests/tst_control_plane.pro` | test - Pitfall 4 on every route, the documented request bodies, the `SH-` reference shape, and the guarantee that no Sunshine admin path appears on the wire | 03-03 |
| `tests/tst_token_store.cpp`, `tests/tst_token_store.pro` | test - runs against the real Windows DPAPI, and proves D-30 negatively: it stores a token, reads the bytes that actually landed on disk, and fails with a hex dump if the token's own bytes appear in them | 03-03 |
| `tests/tst_session_websocket.cpp`, `tests/tst_session_websocket.pro` | test - the endpoint mapping (`https` to `wss`, never a Sunshine port), the backoff schedule up to its 30 s cap, and the frame-routing rules including the unknown-type no-op | 03-03 |
| `tests/tst_pairing.cpp`, `tests/tst_pairing.pro` | test - the PIN reaches the seam and no signal, the 250 ms poll and 90 s deadline, 409-before-ready is a wait rather than a failure, `status:false` on an HTTP 200 is a failure, and a success without a client UUID is a refusal | 03-03 |
| `tests/tst_teardown.cpp`, `tests/tst_teardown.pro` | test - the stage order is strictly forward, a rig-side stall is `TEARDOWN_TIMEOUT` and not a normal end, and teardown leaves no file behind (checked against a real stored DPAPI blob) | 03-03 |
| `tests/tst_liveness.cpp`, `tests/tst_liveness.pro` | test - the 10 s interval and 30 s grace, the non-fatal warning that leaves the timer running, and the `authorized_through` horizon arithmetic including the extension path | 03-03 |
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

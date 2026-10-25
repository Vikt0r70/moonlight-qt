# AGENTS.md - RentaPC (moonlight-qt fork)

This is the customer Windows client, a fork of `moonlight-qt`. Its SeatHub code is confined to
`app/seathub/`; the streaming engine stays as upstream wrote it. The workspace repo (Seven Hills
Alpha) `AGENTS.md` does NOT apply inside this repo. This file is the only agent rulebook here. The
workspace `.project/` contracts point at this file; they do not live in this repo.

## Authority

1. The user, in chat.
2. This file.
3. The workspace spec's client page, `docs/spec/client.md` (workspace repo).
4. The workspace intent, `.project/intent/INTENT.md` (workspace repo).

If two sources disagree, stop and report it. Never average.

## Boundary and the engine

- SeatHub code is new files under `app/seathub/` (C++) and `app/gui/` (QML views only). Wiring lives
  in `app/main.cpp`, `app/app.pro` and `app/qml.qrc`. `FORK-CHANGES.md` lists every file that differs
  from upstream.
- No streaming-engine surface is allowed beyond the four exceptions in `FORK-CHANGES.md`:
  `app/streaming/video/overlaymanager.*`, the `session.cpp` window-title literal, the `session.cpp`
  port-test server and relay, and `app/backend/nvpairingmanager.cpp`. Every line added to
  `session.cpp` carries `SeatHub:` or `ADR-0046`. Every line added to `nvpairingmanager.cpp` carries
  `ADR-0109`. A new exception needs an ADR in the workspace repo first.
- The CI `diff-gate` job in `.github/workflows/seathub-build-win.yml` compares against upstream tag
  `v6.1.0` and fails any other change under `app/streaming/` or `app/backend/`. `tst_d28_boundary`
  pins the exact line sets. Never change an engine file to get a reading (ADR-0038, ADR-0044).
- Only these `app/seathub` files include engine headers: `moonlight_engine_session.{h,cpp}`,
  `osd_compositor.h`, `pairing_handshake.cpp`, `settings_bridge.cpp`. Do not add to that list.
- No new dependency without the owner's approval. No secrets or DSNs in the repo; telemetry compiles
  in no DSN. Accepted risks in ADR-0050 are not defects.
- Money and minutes are integers (piasters, minutes); instants are UTC.

## Starting work and releases

- Start from the newest published release tag (`seathub-v*`; newest as of writing is
  `seathub-v0.1.35`), never from the old local 0.1.27 branch. Find it with
  `git ls-remote --tags origin "seathub-v0.1.*"`. `docs/release-versions.md` records the history.
- Version sources are `app/version.txt` and `SEATHUB_VERSION` in `app/seathub/seathub_version.h`.
- The release and publication procedure is owned by the workspace repo, Seven Hills Alpha:
  `docs/runbooks/client-release.md`. This repo only builds (`scripts/build-seathub.ps1`) and packs
  (`scripts/pack-seathub.ps1`); the push-to-main CI job publishes the `seathub-v$VERSION` release.
  Do not publish from here.

## Structure rules

- S1. Each business area has one owner and one front door. QML talks to `SeatHubClient`
  (`app/seathub/seathub_client.h:66`). Control-plane HTTP goes only through `ControlPlaneClient`
  (`app/seathub/control_plane_client.h:350`). The session sequence lives in `SessionLifecycle`
  (`app/seathub/session_lifecycle.h:33`). Tokens live in `TokenStore` (`app/seathub/token_store.h:41`).
- S2. Outside services sit behind a small interface. The engine is reached only through the
  `EngineSession` seam (`app/seathub/engine_session.h:51`; production `MoonlightEngineSession`,
  `moonlight_engine_session.h:93`; fake `stub_engine_session.cpp`). Pairing goes through
  `PairingSeam` (`app/seathub/pairing_controller.h:60`). Sentry is reached only through
  `app/seathub/telemetry.cpp`, the one file that includes `sentry.h`. A new provider is a new
  adapter; nothing else changes.
- S3. Pure logic (formatting, error mapping, retry state, time maths) stays apart from network,
  engine and screen code, so a suite can run it alone (for example `error_map`, `duration_text`,
  `update_retry_state`).
- S4. Tests go through the front door, and every behaviour change starts with a failing test.
- S5. Nothing reaches into another area's private parts. Never include or call another area's
  internals; use its header's public functions.
- Tests use public functions, not internals. A test double is for an outside-service adapter only.
- No refactoring for its own sake: new code follows these rules, a file a task must change moves
  toward them when the move is small, and untouched old code is not a finding.

## Tests and evidence

- Suites are qmake projects in `tests/` (`tst_<name>.cpp` plus `tst_<name>.pro`). Every change needs
  a suite, registered in `tests/run-all-suites.cmd`.
- Run named suites: `tests\run-all-suites.cmd tst_a tst_b` (prints `NAMED_SUITES_PASSED`). With no
  arguments it runs the full list and prints `ALL_SUITES_PASSED`. Needs Qt 6.11.2 at
  `C:\Qt\6.11.2\msvc2022_64` and a VS 2022 x64 C++ toolset.
- Test-first: for a behaviour change or bug fix, write the test, run it, and see it fail for the
  expected reason; log `RED: <suite> failed: <one line>`. A bug fix starts with a test that
  reproduces the bug. Exceptions (live-rig checks, config, docs, generated code) log
  `RED: n/a (<reason>)`.
- Research before code: read current Qt/library docs (Context7) before writing against an API, and
  search for real examples before a second guess at the same failure.
- Run long builds once, in the background; a timeout is an unknown outcome, not a failure.

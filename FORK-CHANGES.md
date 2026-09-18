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

_None yet. This is the fork's foundation commit — no SeatHub changes exist yet. Entries below are
added by each later plan (Plan 03-02 onward) as it modifies a file, in the same commit that makes
the change._

| File | Category | Reason | Plan |
|------|----------|--------|------|
| _(none)_ | | | |

## New files (not modifications, informational only)

SeatHub's own C++/QML files under `app/seathub/` and `app/gui/` are additions, not modifications
to upstream files — they do not require an entry in the table above and are not subject to the
CI diff gate's exception list (the gate only checks files that exist in the upstream tag's tree).
This section is kept for completeness once those files start landing (Plan 03-02 onward).

_None yet._

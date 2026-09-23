#pragma once

// SeatHub's own version, which is what the release feed's rows are compared against.
//
// D-46: the version is SeatHub's, plus the monotonic build number the feed row carries; it never
// implies parity with the pinned upstream moonlight-qt tag (that pin lives in
// `seathub-ops/pins.yaml`). The value here is the retired Tauri client's own version, taken from
// its `src-tauri/tauri.conf.json` and `src-tauri/Cargo.toml` so the version does not restart
// when the client is replaced (D-52). The packaged binary's file version still comes from
// upstream's `app/version.txt`; aligning that is Plan 03-06's packaging work.

#define SEATHUB_VERSION "0.1.16"

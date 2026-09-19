#pragma once

// Per-session teardown state, owned by the facade that owns the session.
//
// Why this is a separate object and not a member. It is written on the control-plane thread
// (`beginSession()` -> reset, `handleReadyForDeletion()` -> markStarted, `handleTeardownCompleted()`
// -> reset) and read on the Qt main thread (`handleReadyForDeletion()`), which is the same
// cross-thread access pattern the security re-audit flagged on `TeardownController::stage()`. Two
// facts make the split safe and make `std::atomic` sufficient rather than a lock:
//
//   * Every reader is asking one question - "has this session's teardown already been asked for?"
//     - and every writer answers it idempotently. A stale read can, at worst, ask for a teardown
//     that is already running, which `TeardownController::teardown()` (`kStageIdle`/`kStageDone`
//     branches) drops rather than double-running.
//   * `markStarted()` is test-and-set. Two racing callers cannot both get `true`, so the
//     "exactly once per session" property does not depend on which thread got there first.
//
// What it is NOT: a replacement for `TeardownController`'s own stage. That stage is the teardown
// state machine's internal progress - which of disable/remove/verify is done, and whether the store
// has been cleared - and is read on the network thread where it is also written. This flag answers
// a different question, and the two are deliberately not merged.
//
// Plan 03-06 fix, defect F-9. Before it, the facade guarded on `TeardownController::stage() !=
// TeardownStage::Done`. `Done` is sticky: it is set when a teardown completes and cleared only by
// `cancel()`, which only `signOut()` calls. So the first session tore down correctly and every
// session after it skipped teardown entirely - no `POST /api/sessions/{id}/end`, no rig-side
// disable/unpair, no local token clear, and no `teardownCompleted()` - which is how STREAM-10
// ("pairing disable -> unpair -> verify, no local state") went unmet from the second session
// onward. It was also an unsynchronised read of a non-atomic enum across threads.

#include <atomic>

class SessionTeardownGuard
{
public:
    /// A new session is beginning: whatever the previous one did is history, and this session's
    /// teardown has not been asked for yet.
    void reset() { m_started.store(false, std::memory_order_relaxed); }

    /// Claim this session's teardown. True to exactly one caller per `reset()`; false to every
    /// caller after it - including a second one racing on another thread.
    bool markStarted()
    {
        bool expected = false;
        return m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

private:
    std::atomic<bool> m_started{false};
};

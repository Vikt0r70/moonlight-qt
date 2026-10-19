#pragma once

#include "reporting_consent_store.h"

#include <atomic>

// The customer's reporting choice (ADR-0107), as the app sees it. `coveredUploadsAllowed()` is the
// ONE switch every covered sender asks - Sentry crash reports and the Crashpad handler, the log
// shipper, the quality outbox and the evidence-journal capsule delivery - on every send. Functional
// calls (sign-in, Play, End reports, liveness, SSE, session calls, update checks) never ask it.
//
// The state is read from the per-user store once, as the first act of `main()` (`loadFromStore()`),
// before Sentry or any network object exists. After that only the customer's own explicit choice -
// the setup window's box, the in-app ask, or the Settings row (`recordChoice()`) - changes it. No
// server value, sign-in, account change, update or reinstall reaches this class.
//
// Header-only and free of Qt types so the suites that compile one of the gated senders
// (tst_telemetry, tst_log_shipper, tst_facade_wiring) need no new source file. Until
// `loadFromStore()` has run the answer is "allowed": those suites exercise the senders without a
// `main()`, and every real entry point of the app loads first (app/main.cpp), so a process that
// reaches the network without having loaded does not exist.
class ReportingConsent
{
public:
    // After this call: an unrecorded choice, or one recorded as "do not send", answers false.
    static void loadFromStore()
    {
        publish(reporting_consent_store::read());
    }

    static bool coveredUploadsAllowed()
    {
        const int state = s_state.load(std::memory_order_acquire);
        if ((state & kLoaded) == 0) {
            return true;
        }
        return (state & kRecorded) != 0 && (state & kSend) != 0;
    }

    static bool choiceRecorded()
    {
        return (s_state.load(std::memory_order_acquire) & kRecorded) != 0;
    }

    // The notice version stored with the recorded choice (0 when none is recorded).
    static unsigned noticeVersion()
    {
        return s_noticeVersion.load(std::memory_order_acquire);
    }

    // True when the app must put the notice in front of the customer before any covered upload:
    // no choice exists yet, or the stored choice was made against an older notice. The previous
    // choice (if any) stays in force while this is true.
    static bool needsAsk(unsigned packageNoticeVersion = reporting_consent_store::kCurrentNoticeVersion)
    {
        const int state = s_state.load(std::memory_order_acquire);
        if ((state & kLoaded) == 0) {
            return false;
        }
        if ((state & kRecorded) == 0) {
            return true;
        }
        return noticeVersion() < packageNoticeVersion;
    }

    // The customer's own explicit choice. Persists first and takes effect only when the write
    // landed, so a failed write can never leave the screen and the disk disagreeing.
    static bool recordChoice(bool send, unsigned noticeVersionShown
                                        = reporting_consent_store::kCurrentNoticeVersion)
    {
        if (!reporting_consent_store::write(send, noticeVersionShown)) {
            return false;
        }
        publish(reporting_consent_store::Record{ true, send, noticeVersionShown });
        return true;
    }

    // Tests only.
    static void setStateForTests(bool loaded, const reporting_consent_store::Record& record)
    {
        if (!loaded) {
            s_noticeVersion.store(0);
            s_state.store(0, std::memory_order_release);
            return;
        }
        publish(record);
    }

    // QML singleton + Win32 ask, defined in reporting_consent.cpp (app and tst_reporting_consent only).
    static void registerQml();
    // Shows the notice window. `changes` is the "what changed" list for a re-ask (empty for a
    // first ask). Returns true when the customer answered (and the answer was recorded); false when
    // they closed it without answering, in which case nothing is recorded.
    static bool askAtStartup(const wchar_t* const* changes, int changeCount);

private:
    static constexpr int kLoaded = 1;
    static constexpr int kRecorded = 2;
    static constexpr int kSend = 4;

    static void publish(const reporting_consent_store::Record& record)
    {
        s_noticeVersion.store(record.recorded ? record.noticeVersion : 0, std::memory_order_release);
        int state = kLoaded;
        if (record.recorded) {
            state |= kRecorded;
            if (record.send) {
                state |= kSend;
            }
        }
        s_state.store(state, std::memory_order_release);
    }

    static inline std::atomic<int> s_state{ 0 };
    static inline std::atomic<unsigned> s_noticeVersion{ 0 };
};

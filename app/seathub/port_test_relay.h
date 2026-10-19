#pragma once

// port_test_relay (ADR-0104, T062): the one route the real `LiTestClientConnectivity` result takes
// from the engine's connection thread to the facade.
//
// `Session::clStageFailed` and `Session::clConnectionTerminated` run the port test on moonlight's
// connection thread and keep only an int in `m_PortTestResults`. The two calls this header exists
// for (`seathub::PortTestRelay::publish(portFlags, m_PortTestResults)` in app/streaming/session.cpp,
// marked `// SeatHub: ADR-0104`) hand that int and the requested flags here. The facade takes the
// result on its own thread (`take`) when it builds the End body and the liveness report, and
// `reset`s at the start of every Play, so a result an earlier Play published and never took is not
// attached to a later one.
//
// Header-only and Qt Core only. The library's inconclusive sentinel is `ML_TEST_RESULT_INCONCLUSIVE`
// from `Limelight.h` (0xFFFFFFFF at the pinned moonlight-common-c commit 8599b604, Limelight.h:900).
// The submodule is empty in a fresh task worktree and `tests/tst_facade_wiring.pro` has no
// Limelight include path, so the header includes it only when the compiler can find it; otherwise
// the fixed value is used. When both are visible they must agree.

#include <QMutex>
#include <QMutexLocker>

#include "port_test_result.h"

#if defined(__has_include)
#if __has_include(<Limelight.h>)
#include <Limelight.h>
#endif
#endif

namespace seathub {

/// `ML_TEST_RESULT_INCONCLUSIVE` at the pinned commit.
constexpr unsigned int kPortTestInconclusive = 0xFFFFFFFFu;

#ifdef ML_TEST_RESULT_INCONCLUSIVE
static_assert(static_cast<unsigned int>(ML_TEST_RESULT_INCONCLUSIVE) == kPortTestInconclusive,
              "moonlight-common-c's inconclusive sentinel moved; update port_test_relay.h");
#endif

class PortTestRelay
{
public:
    /// Called on the engine's connection thread right after `LiTestClientConnectivity` returns.
    /// `result` is `m_PortTestResults`, an `int`, so it arrives as the same bits it was stored as.
    static void publish(unsigned int requestedFlags, unsigned int result)
    {
        State& s = state();
        QMutexLocker locker(&s.mutex);
        s.result = classifyPortTest(requestedFlags, result, kPortTestInconclusive);
    }

    /// The last published result, or `NotRun` when none was published since the last `take` or
    /// `reset`. Taking clears it: one result belongs to one stage failure or termination.
    static PortTestResult take()
    {
        State& s = state();
        QMutexLocker locker(&s.mutex);
        const PortTestResult out = s.result;
        s.result = PortTestResult();
        return out;
    }

    /// The last published result without clearing it. A termination report reads it on the engine
    /// thread while the End body for the same Play still needs it on the facade thread.
    static PortTestResult peek()
    {
        State& s = state();
        QMutexLocker locker(&s.mutex);
        return s.result;
    }

    /// Drops anything published and not taken. The facade calls it when a Play begins.
    static void reset()
    {
        State& s = state();
        QMutexLocker locker(&s.mutex);
        s.result = PortTestResult();
    }

private:
    struct State
    {
        QMutex mutex;
        PortTestResult result;
    };

    static State& state()
    {
        static State s;
        return s;
    }
};

} // namespace seathub

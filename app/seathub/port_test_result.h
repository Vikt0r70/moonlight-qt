#pragma once

// port_test_result (ADR-0104, owner ruling (a) and (g) of 2026-10-17): the real
// `LiTestClientConnectivity` result as a closed class, the wire `port_test` object, and the one
// condition under which the existing "Check your firewall and port forwarding rules" sentence
// may show. No new customer text exists (ruling (g)).
//
// Header-only and Qt-Core-only. The library's inconclusive sentinel
// (`ML_TEST_RESULT_INCONCLUSIVE`, 0xFFFFFFFF at the pinned commit) is a PARAMETER so this header
// never includes `Limelight.h`; T062 passes the macro and re-reads its value at the pinned commit.
// No address is part of the result: an outcome and a bitmask of well-known port numbers only.

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

struct PortTestResult {
    enum class Outcome { NotRun, Clean, Failed, Inconclusive };

    Outcome outcome = Outcome::NotRun;
    /// The `LiTestClientConnectivity` return value (the `ML_PORT_FLAG_*` bits that did not pass);
    /// meaningful only for `Failed`, 0 otherwise.
    quint32 failedMask = 0;
};

/// `requestedFlags` is what the failed stage or termination asked to test: 0 means the library
/// returned 0 without testing anything, which is not a result (`NotRun`, and the client omits
/// `port_test`). `result == inconclusiveSentinel` is `Inconclusive`; 0 is `Clean`; anything else
/// is `Failed` with `failedMask = result`.
inline PortTestResult classifyPortTest(unsigned int requestedFlags, unsigned int result,
                                       unsigned int inconclusiveSentinel)
{
    PortTestResult out;
    if (requestedFlags == 0) {
        out.outcome = PortTestResult::Outcome::NotRun;
    } else if (result == inconclusiveSentinel) {
        out.outcome = PortTestResult::Outcome::Inconclusive;
    } else if (result == 0) {
        out.outcome = PortTestResult::Outcome::Clean;
    } else {
        out.outcome = PortTestResult::Outcome::Failed;
        out.failedMask = result;
    }
    return out;
}

/// The ADR-0104 `port_test` object: `{"outcome": "failed"|"clean"|"inconclusive", "failed_mask":
/// <positive integer>}` with `failed_mask` present exactly when `failed`. Empty for `NotRun`,
/// which callers omit.
inline QJsonObject portTestJson(const PortTestResult& result)
{
    QJsonObject object;
    switch (result.outcome) {
    case PortTestResult::Outcome::NotRun:
        break;
    case PortTestResult::Outcome::Clean:
        object.insert(QStringLiteral("outcome"), QStringLiteral("clean"));
        break;
    case PortTestResult::Outcome::Inconclusive:
        object.insert(QStringLiteral("outcome"), QStringLiteral("inconclusive"));
        break;
    case PortTestResult::Outcome::Failed:
        object.insert(QStringLiteral("outcome"), QStringLiteral("failed"));
        object.insert(QStringLiteral("failed_mask"), static_cast<qint64>(result.failedMask));
        break;
    }
    return object;
}

/// The existing firewall sentence may show only for a conclusive result with a failed port.
/// Not run, clean and inconclusive never show it.
inline bool showFirewallSentence(const PortTestResult& result)
{
    return result.outcome == PortTestResult::Outcome::Failed;
}

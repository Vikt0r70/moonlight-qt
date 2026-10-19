#pragma once

// pairing_retry (INTENT SC18/N18 correction 2026-10-17, ADR-0072 item 2 amendment, ADR-0098
// amendment): the shared budget of the ONE automatic pairing retry a Play may take.
//
// A Play gets at most one retry in total. The existing 409 / ALREADY_IN_PROGRESS clear and the
// new "any other non-PAIRED handshake result" retry draw from this one budget, so a conflict
// clear followed by a failure is NOT a second retry. `decide()` is a pure state machine: the
// caller (T061, `HandshakeTask::run`) fills `Inputs` from facts it already holds and runs the
// handshake again only when a reason comes back. Nothing here sleeps, posts, logs or touches the
// PIN: `pinShapeOk` reads a PIN only to answer a bool and keeps nothing.
//
// Header-only and Qt-Core-only (QString, <optional>): it compiles into any `.pro` that already
// compiles its caller and needs no new translation unit, and the Linux Core suite runner
// (`tests/linux/run-core-suites.sh`) builds its suite.

#include <QString>
#include <QtGlobal>

#include <optional>

class PairingRetryBudget
{
public:
    /// Why the retry was granted. `Conflict` is the rig's 409 or `ALREADY_IN_PROGRESS`;
    /// `Failure` is any other non-PAIRED result.
    enum class Reason { Conflict, Failure };

    struct Inputs {
        /// The handshake just finished PAIRED.
        bool ok = false;
        /// The failure was the rig's 409 on getservercert or `ALREADY_IN_PROGRESS`.
        bool pairingConflict = false;
        /// This run's generation still equals the current one (no newer `pair()`, no cancel).
        bool generationCurrent = true;
        /// The `reconnecting` ACTIVE flow (a held session re-pairing inside its grace).
        bool reconnecting = false;
        /// A stream already exists for this Play.
        bool streamExists = false;
        /// Milliseconds spent since the single 90 s seam deadline started (the retry does not
        /// restart it).
        qint64 elapsedMs = 0;
        /// The single seam deadline in milliseconds.
        qint64 deadlineMs = 0;
    };

    /// A reason at most once per instance. Nothing is granted, and nothing is spent, when the
    /// handshake succeeded, the generation changed, the flow is `reconnecting`, a stream exists
    /// or `elapsedMs >= deadlineMs`.
    std::optional<Reason> decide(const Inputs& in)
    {
        if (m_spent || in.ok || !in.generationCurrent || in.reconnecting || in.streamExists
            || in.elapsedMs >= in.deadlineMs) {
            return std::nullopt;
        }
        m_spent = true;
        return in.pairingConflict ? Reason::Conflict : Reason::Failure;
    }

    /// True once a reason has been granted.
    bool spent() const { return m_spent; }

    /// The closed `retry_reason` token for the wire and the result record.
    static QString token(Reason reason)
    {
        return reason == Reason::Conflict ? QStringLiteral("conflict") : QStringLiteral("failure");
    }

    /// Exactly four ASCII digits. Answers a bool only; the PIN is neither kept nor logged.
    static bool pinShapeOk(const QString& pin)
    {
        if (pin.size() != 4) {
            return false;
        }
        for (const QChar c : pin) {
            if (c.unicode() < '0' || c.unicode() > '9') {
                return false;
            }
        }
        return true;
    }

private:
    bool m_spent = false;
};

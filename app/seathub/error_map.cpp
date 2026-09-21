#include "error_map.h"

namespace {

// Customer-facing strings come from `docs/spec/copy.md`.
//   §Support & errors  - "Generic error: `Something went wrong on our side. Reference SH-9K2XQ1.`"
// The code in that line is that section's *sample*, not a code this client may print, and
// ADR-0008 mints references per response on the control plane, never on the client: "a reference
// the system cannot resolve is worse than none". So `error` carries the deck's sentence and
// `reference` stays empty unless the control plane supplied one - `ErrorScreen.qml` renders the
// reference row only when there is something to render (audit F9; the three client-invented codes
// this file used to carry were removed rather than re-shaped).
const char* kGenericSentence = "Something went wrong on our side.";
// copy.md §Support & errors, "Offline", in full. Home shows the same sentence; the two must not drift.
const char* kOfflineSentence = "Can't reach SevenHills right now. Showing the last known balance.";

} // namespace

SeatHubFailure SeatHubFailure::network(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Network;
    f.error = message;
    // No reference, deliberately: the request never reached the control plane, so no reference
    // exists for it, and ADR-0008 §3 forbids the client minting one.
    return f;
}

SeatHubFailure SeatHubFailure::auth(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Auth;
    f.error = message;
    return f;
}

SeatHubFailure SeatHubFailure::local(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Local;
    f.error = message;
    return f;
}

SeatHubFailure SeatHubFailure::engine(const QString& message, const QString& reference)
{
    SeatHubFailure f;
    f.kind = FailureKind::Engine;
    f.error = message;
    f.reference = reference;
    return f;
}

SeatHubFailure SeatHubFailure::api(int statusCode, const QString& error, const QString& reference,
                                   const QString& failure)
{
    SeatHubFailure f;
    f.kind = FailureKind::Api;
    f.error = error;
    // Whatever the control plane sent, verbatim - including nothing, if the response was
    // malformed: an invented fallback would be a code support cannot resolve, which ADR-0008
    // calls out as worse than showing none.
    f.reference = reference;
    f.statusCode = statusCode;
    f.failure = failure;
    return f;
}

SeatHubFailure SeatHubFailure::generic()
{
    SeatHubFailure f;
    f.kind = FailureKind::Engine;
    // copy.md §Support & errors: the sentence alone. Its "SH-9K2XQ1" is the deck's sample code,
    // not this incident's, and no locally generated failure has a resolvable code (ADR-0008).
    f.error = QString::fromLatin1(kGenericSentence);
    return f;
}

QString SeatHubFailure::offlineSentence()
{
    return QString::fromLatin1(kOfflineSentence);
}

QVariantMap SeatHubFailure::toVariantMap() const
{
    QVariantMap map;
    switch (kind) {
    case FailureKind::Api:     map.insert(QStringLiteral("kind"), QStringLiteral("api")); break;
    case FailureKind::Network: map.insert(QStringLiteral("kind"), QStringLiteral("network")); break;
    case FailureKind::Auth:    map.insert(QStringLiteral("kind"), QStringLiteral("auth")); break;
    case FailureKind::Local:   map.insert(QStringLiteral("kind"), QStringLiteral("local")); break;
    case FailureKind::Engine:  map.insert(QStringLiteral("kind"), QStringLiteral("engine")); break;
    }
    map.insert(QStringLiteral("error"), error);
    map.insert(QStringLiteral("reference"), reference);
    map.insert(QStringLiteral("statusCode"), statusCode);
    map.insert(QStringLiteral("failure"), failure);
    return map;
}

// ---------------------------------------------------------------------------
// The D-51 mapping, minus the copy this file used to invent.
//
// `Session::stageFailed(stage, errorCode, failingPorts)` carries the stage the engine was in when
// the connection failed. The stage string is moonlight-common-c's own human-readable name
// (`QString::fromLocal8Bit(LiGetStageName(stage))` in `session.cpp`), which is internal
// vocabulary: `STAGE_AUDIO_STREAM_INIT`, `STAGE_RTSP_HANDSHAKE`, and so on. A customer must never
// see any of it.
//
// The table that used to live here chose between two client-written sentences ("Can't reach the
// rig right now.", "This rig can't decode the stream. Try a different quality setting.") and
// three client-minted reference codes. Neither sentence is in `docs/spec/copy.md` and the spec
// forbids editing it to make a build pass, so every mapped failure now takes the deck's own
// generic sentence and the stage detail survives only in `diagnostic` - which is logged and never
// rendered (D-51, Pitfall 7).
// ---------------------------------------------------------------------------

SeatHubFailure mapStageFailure(const QString& stage, int errorCode, const QString& failingPorts)
{
    // copy.md §Play flow has no sentence for a stream that never started - "Failure, mode boot"
    // and "Expired before streaming" are both about a session that ended, not about a connection
    // that failed - so the §Support & errors sentence is the only deck-sanctioned one here.
    // A per-stage sentence needs a copy.md entry first; recorded as a spec gap.
    SeatHubFailure f = SeatHubFailure::generic();
    f.diagnostic = QStringLiteral("stage=%1 errorCode=%2 failingPorts=%3")
                       .arg(stage)
                       .arg(errorCode)
                       .arg(failingPorts);
    return f;
}

SeatHubFailure mapLaunchError(const QString& text)
{
    // T-03-05 / D-51: the engine's own text is intercepted and never rendered. It is
    // kept in `diagnostic` only, so the streaming diagnostic survives for support
    // (Pitfall 7) without showing a customer anything of Moonlight's.
    SeatHubFailure f = SeatHubFailure::generic();
    f.diagnostic = text;
    return f;
}

SeatHubFailure mapPortTestFailure(int portTestResult)
{
    Q_UNUSED(portTestResult);
    return SeatHubFailure::generic();
}

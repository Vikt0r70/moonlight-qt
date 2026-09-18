#include "error_map.h"

namespace {

// Customer-facing strings come from `docs/spec/copy.md`.
//   §Support & errors  - "Generic error: `Something went wrong on our side. Reference SH-9K2XQ1.`"
const char* kGenericSentence = "Something went wrong on our side.";
const char* kGenericReference = "SH-9K2XQ1";

// Local (client-side) failures carry no control-plane reference of their own, so the
// client names one from the same ADR-0008 vocabulary. `SH-GENERR` is the fallback for
// any engine stage the table does not name (D-51).
const char* kGenericEngineReference = "SH-GENERR";

} // namespace

SeatHubFailure SeatHubFailure::network(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Network;
    f.error = message;
    f.reference = QString::fromLatin1(kGenericReference);
    return f;
}

SeatHubFailure SeatHubFailure::auth(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Auth;
    f.error = message;
    f.reference = QString::fromLatin1(kGenericReference);
    return f;
}

SeatHubFailure SeatHubFailure::local(const QString& message)
{
    SeatHubFailure f;
    f.kind = FailureKind::Local;
    f.error = message;
    f.reference = QString::fromLatin1(kGenericReference);
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

SeatHubFailure SeatHubFailure::generic()
{
    SeatHubFailure f;
    f.kind = FailureKind::Engine;
    // copy.md §Support & errors gives the whole sentence with the reference inline.
    f.error = QStringLiteral("%1 Reference %2.").arg(QString::fromLatin1(kGenericSentence),
                                                     QString::fromLatin1(kGenericReference));
    f.reference = QString::fromLatin1(kGenericReference);
    return f;
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
// Tracer stub (Plan 03-02 Task 1). Task 2 replaces this with the full D-51 table
// keyed off every stage name `Session::stageFailed()` can emit.
// ---------------------------------------------------------------------------
SeatHubFailure mapStageFailure(const QString& stage, int errorCode, const QString& failingPorts)
{
    Q_UNUSED(errorCode);

    // Only the two stages the tracer's stub path exercises are tabled here.
    if (stage == QLatin1String("connection_refused")) {
        return SeatHubFailure::engine(QStringLiteral("Can't reach the rig right now."),
                                      QStringLiteral("SH-CONREF"));
    }
    if (stage == QLatin1String("decoder_unavailable")) {
        return SeatHubFailure::engine(
            QStringLiteral("This rig can't decode the stream. Try a different quality setting."),
            QStringLiteral("SH-DECUNAV"));
    }

    // Anything else falls back to the generic SeatHub message (D-51). `failingPorts`
    // is engine detail and is deliberately not surfaced.
    Q_UNUSED(failingPorts);
    return SeatHubFailure::generic();
}

SeatHubFailure mapLaunchError(const QString& text)
{
    // T-03-05 / D-51: the engine's own text is intercepted and never rendered. It is
    // kept in `diagnostic` only, so the streaming diagnostic survives for support
    // (Pitfall 7) without showing a customer anything of Moonlight's.
    SeatHubFailure f = SeatHubFailure::engine(QString::fromLatin1(kGenericSentence),
                                              QString::fromLatin1(kGenericEngineReference));
    f.diagnostic = text;
    return f;
}

SeatHubFailure mapPortTestFailure(int portTestResult)
{
    Q_UNUSED(portTestResult);
    return SeatHubFailure::generic();
}

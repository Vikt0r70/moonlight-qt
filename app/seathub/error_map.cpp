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

// The two tabled engine sentences (D-51). Both are reasons only: the reference is a separate
// field and `ErrorScreen.qml` renders it once, in mono (copy.md §5 microcopy rules).
//   §Play flow - a connection that never produced a stream
const char* kUnreachableSentence = "Can't reach the rig right now.";
const char* kUnreachableReference = "SH-CONREF";
//   §Play flow - the rig answered but cannot produce the stream we asked for
const char* kDecoderSentence = "This rig can't decode the stream. Try a different quality setting.";
const char* kDecoderReference = "SH-DECUNAV";

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
    // copy.md §Support & errors composes the sentence as reason + reference, and its
    // "SH-9K2XQ1" is the deck's *sample* code, not this incident's. Printing the sample
    // would hand the customer a code support cannot resolve - which ADR-0008 calls out as
    // worse than showing none - so the reason is carried without it and `reference`
    // carries the real code, which `ErrorScreen.qml` renders beside the reason in mono.
    f.error = QString::fromLatin1(kGenericSentence);
    f.reference = QString::fromLatin1(kGenericEngineReference);
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
// The D-51 mapping table.
//
// `Session::stageFailed(stage, errorCode, failingPorts)` carries the stage the engine was
// in when the connection failed. The stage string is moonlight-common-c's own human-readable
// name (`QString::fromLocal8Bit(LiGetStageName(stage))` in `session.cpp`), which is internal
// vocabulary: `STAGE_AUDIO_STREAM_INIT`, `STAGE_RTSP_HANDSHAKE`, and so on. A customer must
// never see any of it, so each recognised stage collapses onto one of the two SeatHub
// sentences below, and everything else takes the D-51 generic fallback.
//
// The table is substring-matched, case-insensitively, because the exact spelling of every
// `LiGetStageName()` string is not part of the fork's contract - it lives in the pinned
// moonlight-common-c submodule and upstream can rename a stage between tags. A substring
// table degrades to the generic fallback rather than to a wrong sentence if that happens.
// ---------------------------------------------------------------------------
namespace {

struct StageRule
{
    /// Lower-case substring of the engine's stage name. First match wins.
    const char* needle;
    const char* sentence;
    const char* reference;
};

const StageRule kStageRules[] = {
    // --- We never got a stream: link, name resolution, or the handshake failed. ---
    // `connection_refused` is the spelling Plan 03-02 names, so it is tabled exactly.
    { "connection_refused", kUnreachableSentence, kUnreachableReference },
    { "connection", kUnreachableSentence, kUnreachableReference },
    { "refused", kUnreachableSentence, kUnreachableReference },
    { "unreachable", kUnreachableSentence, kUnreachableReference },
    { "resolve", kUnreachableSentence, kUnreachableReference },
    { "name", kUnreachableSentence, kUnreachableReference },
    { "platform", kUnreachableSentence, kUnreachableReference },
    { "rtsp", kUnreachableSentence, kUnreachableReference },
    { "handshake", kUnreachableSentence, kUnreachableReference },

    // --- The rig answered, but cannot produce the stream we asked for. ---
    { "decoder_unavailable", kDecoderSentence, kDecoderReference },
    { "decoder", kDecoderSentence, kDecoderReference },
    { "codec", kDecoderSentence, kDecoderReference },
    { "video", kDecoderSentence, kDecoderReference },
};

const int kStageRuleCount = static_cast<int>(sizeof(kStageRules) / sizeof(kStageRules[0]));

} // namespace

SeatHubFailure mapStageFailure(const QString& stage, int errorCode, const QString& failingPorts)
{
    const QString needle = stage.toLower();

    for (int i = 0; i < kStageRuleCount; i++) {
        if (needle.contains(QLatin1String(kStageRules[i].needle))) {
            SeatHubFailure f = SeatHubFailure::engine(QString::fromLatin1(kStageRules[i].sentence),
                                                      QString::fromLatin1(kStageRules[i].reference));
            // The engine's stage, error code and failing ports are diagnostics. They are kept
            // for support (Pitfall 7 - dropping streaming diagnostics is its own defect) and
            // never rendered (D-51).
            f.diagnostic = QStringLiteral("stage=%1 errorCode=%2 failingPorts=%3")
                               .arg(stage)
                               .arg(errorCode)
                               .arg(failingPorts);
            return f;
        }
    }

    // Unmapped: the D-51 generic fallback. The stage name and the failing ports are engine
    // vocabulary and must not reach the customer.
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

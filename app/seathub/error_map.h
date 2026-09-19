#pragma once

// SeatHub failure model + engine-signal -> support-reference-code mapping (D-51).
//
// Ported from the retired Tauri client's `src-tauri/src/error.rs`
// (`FailureKind` / `Failure`). The one shape every failure carries, whether it came
// from the control plane or from the streaming engine:
//
//   kind        which layer failed - the UI picks its copy from this
//   error       customer-facing text. NEVER raw upstream/engine text.
//   reference   `SH-XXXXXX` (ADR-0008). Always present; it is the only thing
//               support can search by, so an error without one is a defect.
//   statusCode  the control plane's HTTP status, or 0 when the failure was local
//   failure     `AllocationRefused.failure`, e.g. NO_HOST_AVAILABLE
//   diagnostic  diagnostics-only detail (raw engine text, or the engine's stage /
//               error code / failing ports). Never rendered.
//
// The mapping table lives in `error_map.cpp`: each recognised engine stage collapses onto
// one of two SeatHub sentences, and anything unnamed takes the D-51 generic fallback.

#include <QMetaType>
#include <QString>
#include <QVariantMap>

enum class FailureKind
{
    /// The control plane answered with `{status_code, status:false, error, reference}`.
    Api,
    /// We never reached the control plane. There is no control-plane reference.
    Network,
    /// No usable credentials. The UI must return to sign-in.
    Auth,
    /// Something on this machine: decoder refused, credential store refused.
    Local,
    /// The streaming engine failed a stage (D-51 - new, not in the Rust original).
    Engine,
};

struct SeatHubFailure
{
    FailureKind kind = FailureKind::Engine;
    /// Customer-facing. Sourced from `docs/spec/copy.md`, never from the engine.
    QString error;
    /// `SH-XXXXXX` (ADR-0008). Always non-empty.
    QString reference;
    /// Control-plane HTTP status, or 0 when the failure was local to the client.
    int statusCode = 0;
    /// `AllocationRefused.failure`, when the control plane named one.
    QString failure;
    /// Diagnostics only - raw engine text kept for the log/support, never shown.
    QString diagnostic;

    static SeatHubFailure network(const QString& message);
    static SeatHubFailure auth(const QString& message);
    static SeatHubFailure local(const QString& message);
    static SeatHubFailure engine(const QString& message, const QString& reference);

    /// The control plane answered and refused: an HTTP 200 carrying `status:false`, or any
    /// non-2xx `Error` / `AllocationRefused` response. Carries the control plane's own
    /// ADR-0008 reference when it named one, and its `failure` code when it named one.
    /// `error` is the control plane's customer-facing sentence, never engine text.
    static SeatHubFailure api(int statusCode, const QString& error, const QString& reference,
                              const QString& failure = QString());

    /// The D-51 fallback: `Something went wrong on our side. Reference SH-9K2XQ1.`
    static SeatHubFailure generic();

    /// Keys the customer-facing fields for QML. The `diagnostic` field is deliberately
    /// omitted - it must never cross into the view layer (D-51).
    QVariantMap toVariantMap() const;
};

// Declared so a failure can cross a thread boundary. The control-plane callbacks run on the
// network thread `ControlPlaneClient` owns, so `pairingFailed` and `teardownFailed` are delivered
// to the facade as queued connections - which needs a registered metatype, not just a copyable
// struct.
Q_DECLARE_METATYPE(SeatHubFailure)

/// Maps `Session::stageFailed(stage, errorCode, failingPorts)` to a SeatHub failure.
SeatHubFailure mapStageFailure(const QString& stage, int errorCode, const QString& failingPorts);

/// Maps `Session::displayLaunchError(text)` to a SeatHub failure. The raw `text` is
/// retained in `diagnostic` only - it must never reach the customer (T-03-05).
SeatHubFailure mapLaunchError(const QString& text);

/// Maps a `Session::sessionFinished(portTestResult)` non-zero port-test result.
SeatHubFailure mapPortTestFailure(int portTestResult);

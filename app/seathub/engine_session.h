#pragma once

// The engine seam: the exact surface `SessionLifecycle` needs from the streaming engine, with
// nothing from `app/streaming/` in this header.
//
// Why it exists. Before Plan 03-06's gap closure nothing ever called `attachSession()`, so
// `m_session` was always null and `start()` always ran the Plan 03-02 tracer: the client signed
// in and paired for real and then streamed nothing. The seam is what makes the real path
// *assertable*: a test injects a fake `EngineSession`, watches the lifecycle drive it, and proves
// the seven engine signals are connected to SeatHub's handlers rather than to a stub - none of
// which is possible if the only implementation is upstream's `Session`.
//
// The signal set is upstream's own, with upstream's names and argument shapes
// (`app/streaming/session.h`). `SessionLifecycle` re-emits them under its own name, so
// `SessionSegue.qml` has one wiring and the engine's file is never edited to achieve any of this
// beyond ADR-0046's window-title literal.
//
// Two implementations exist:
//
//   production  `moonlight_engine_session.cpp`  upstream's `Session`, constructed from the host
//                                               the pairing handshake resolved
//   fake        `stub_engine_session.cpp`       the Plan 03-02 tracer, now reachable only when
//                                               something constructs it explicitly. It is the
//                                               only D-01/D-03 window-sequence proof that needs
//                                               no Sunshine host, and it is never a default.

#include <QObject>
#include <QString>

#include <memory>

class QWindow;

struct SDL_Surface;

/// A host a pairing handshake resolved, in the only form the engine side needs: upstream's
/// `NvComputer`, carrying the certificate the handshake pinned.
///
/// Opaque here on purpose. This header is on the pairing seam's contract, which compiles into
/// `tst_pairing` - a binary with no engine in it - so the concrete record (and its
/// `app/backend/` include) lives in `moonlight_engine_session.h` instead.
class PairedHost
{
public:
    virtual ~PairedHost() = default;
};

using PairedHostPtr = std::shared_ptr<PairedHost>;

/// One streaming session, as `SessionLifecycle` drives it.
class EngineSession : public QObject
{
    Q_OBJECT

public:
    explicit EngineSession(QObject* parent = nullptr);
    ~EngineSession() override;

    /// Drives the session to completion.
    ///
    /// The production implementation blocks its caller for the whole stream and hijacks the
    /// calling thread as SDL's main thread - upstream's `Session::exec(QWindow*)` semantics, and
    /// the reason every control-plane object in this client owns a thread of its own. A fake may
    /// return immediately; callers must not assume either shape.
    virtual void run(QWindow* window) = 0;

    /// D-02: end the active stream now.
    ///
    /// The production implementation synthesises the engine's own quit keystroke (v6.1.0 has no
    /// `Session::interrupt()`), which the engine's event loop then handles as an ordinary session
    /// end. A fake ends its own sequence.
    virtual void interrupt() = 0;

    /// ADR-0045: composite the HUD bitmap into the running stream's own swapchain.
    ///
    /// A null surface hides the overlay. A non-null surface is `SDL_PIXELFORMAT_ARGB8888`, owns
    /// its pixels, and transfers ownership in every case - true means the engine accepted it,
    /// false means it refused it and the caller must not use it again.
    ///
    /// This is reached through the session object rather than through the engine's
    /// `Session::get()` global, which is what lets the publisher be written (and the publish path
    /// be tested) with no engine in the binary at all.
    virtual bool publishOverlaySurface(SDL_Surface* surface) = 0;

signals:
    void stageStarting(QString stage);
    void stageFailed(QString stage, int errorCode, QString failingPorts);
    void connectionStarted();
    void displayLaunchError(QString text);
    /// The engine's other public reporting seam: a setting it could not honour as saved
    /// (`Session::emitLaunchWarning`). The text is engine wording and never reaches a screen.
    void displayLaunchWarning(QString text);
    void quitStarting();
    void sessionFinished(int portTestResult);
    void readyForDeletion();
};

Q_DECLARE_METATYPE(PairedHostPtr)

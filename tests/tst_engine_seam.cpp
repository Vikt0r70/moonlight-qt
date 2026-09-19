/*****************************************************************************
 * SeatHub fork - engine-seam tests (Plan 03-06 gap closure).
 *
 * The verifier's first blocker: no real engine `Session` was ever attached, so every behaviour
 * downstream of that attach - the seven lifecycle signals, the HUD publish path, the D-01/D-03
 * window sequence - was unreachable in practice while the code looked correct in isolation.
 *
 * These tests assert the wiring itself, with a fake in place of upstream's `Session`. That is the
 * honest test without a Sunshine host: they cannot prove a stream works, and they do not claim to.
 * What they prove is that a session started through this lifecycle is driven, observed and
 * published to through an `EngineSession` rather than through a stand-in, and that with none
 * attached the client fails closed instead of quietly substituting one.
 *****************************************************************************/

#include <QtTest>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QWindow>

#include <memory>

#include "seathub/engine_session.h"
#include "seathub/pairing_seam.h"
#include "seathub/session_lifecycle.h"

namespace {

/// An `EngineSession` that does nothing but record what it was asked to do. Engine-free: this is
/// the whole reason the seam exists.
class FakeEngineSession : public EngineSession
{
    Q_OBJECT

public:
    using EngineSession::EngineSession;

    void run(QWindow* window) override
    {
        runs.append(window);
    }

    void interrupt() override
    {
        ++interrupts;
    }

    bool publishOverlaySurface(SDL_Surface* surface) override
    {
        published.append(surface);
        // The real engine keeps an accepted surface and frees a refused one. This fake accepts
        // everything, which is what makes "the lifecycle kept its hands off the surface" visible:
        // nothing here frees, and nothing in the lifecycle may.
        return true;
    }

    QList<QWindow*> runs;
    QList<SDL_Surface*> published;
    int interrupts = 0;
};

/// A host record with no engine in it. `PairedHost` is opaque precisely so this compiles without
/// `app/backend/`.
class FakePairedHost : public PairedHost
{
};

PairingTarget targetForFakeHandshake()
{
    PairingTarget target;
    target.sessionId = QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111");
    target.hostAddress = QStringLiteral("203.0.113.7");
    target.httpsPort = 47984;
    target.pairingPin = QStringLiteral("4217");
    return target;
}

} // namespace

class TstEngineSeam : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<PairedHostPtr>("PairedHostPtr");
    }

    // --- nothing attached: fail closed, never a stand-in -----------------------------------

    void withNoSessionAttachedStartFailsClosedInsteadOfFallingBack()
    {
        SessionLifecycle lifecycle;
        QVERIFY(lifecycle.upstreamSession() == nullptr);
        QVERIFY(!lifecycle.active());

        // The regression this test exists for: this used to start the Plan 03-02 tracer and return
        // true, so the facade reported a stream that was a timed fake.
        QVERIFY2(!lifecycle.start(nullptr),
                 "with no engine session attached, start() must refuse rather than substitute one");
        QVERIFY2(!lifecycle.active(), "a refused start must not leave the lifecycle active");

        // And nothing has become attachable behind the caller's back.
        QVERIFY(lifecycle.upstreamSession() == nullptr);
    }

    void theHudPublisherWithNoSessionRefusesAndKeepsItsHandsOff()
    {
        SessionLifecycle lifecycle;
        SDL_Surface* surface = reinterpret_cast<SDL_Surface*>(quintptr(0x1234));

        // False means "the engine did not take it", and the caller then owns the surface. The
        // lifecycle must not free it: it has no idea where it came from.
        QVERIFY(!lifecycle.publishOverlaySurface(surface));
    }

    // --- the eight engine signals reach SeatHub's own signals ------------------------------

    void everyEngineSignalReachesTheLifecyclesOwnSignals()
    {
        SessionLifecycle lifecycle;
        auto* engine = new FakeEngineSession;
        lifecycle.attachSession(engine);
        QCOMPARE(lifecycle.upstreamSession(), static_cast<QObject*>(engine));

        QSignalSpy starting(&lifecycle, &SessionLifecycle::stageStarting);
        QSignalSpy failed(&lifecycle, &SessionLifecycle::stageFailed);
        QSignalSpy started(&lifecycle, &SessionLifecycle::connectionStarted);
        QSignalSpy launchError(&lifecycle, &SessionLifecycle::displayLaunchError);
        QSignalSpy launchWarning(&lifecycle, &SessionLifecycle::displayLaunchWarning);
        QSignalSpy quitting(&lifecycle, &SessionLifecycle::quitStarting);
        QSignalSpy finished(&lifecycle, &SessionLifecycle::sessionFinished);
        QSignalSpy ready(&lifecycle, &SessionLifecycle::readyForDeletion);

        emit engine->stageStarting(QStringLiteral("RTSP handshake"));
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 2, QStringLiteral("47984"));
        emit engine->connectionStarted();
        emit engine->displayLaunchError(QStringLiteral("display mode refused"));
        emit engine->displayLaunchWarning(QStringLiteral("HDR ignored"));
        emit engine->quitStarting();
        emit engine->sessionFinished(0);

        QCOMPARE(starting.count(), 1);
        QCOMPARE(starting.at(0).at(0).toString(), QStringLiteral("RTSP handshake"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("RTSP handshake"));
        QCOMPARE(failed.at(0).at(1).toInt(), 2);
        QCOMPARE(failed.at(0).at(2).toString(), QStringLiteral("47984"));
        QCOMPARE(started.count(), 1);
        QCOMPARE(launchError.count(), 1);
        QCOMPARE(launchWarning.count(), 1);
        QCOMPARE(quitting.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toInt(), 0);

        // `readyForDeletion` is the release signal, and it is asserted separately below; it arrives
        // from the engine's cleanup task, not from the stream loop.
        QCOMPARE(ready.count(), 0);
    }

    // --- both directions: the lifecycle drives, and is driven ---------------------------------

    void startDrivesTheAttachedSessionAndInterruptReachesIt()
    {
        SessionLifecycle lifecycle;
        auto* engine = new FakeEngineSession;
        lifecycle.attachSession(engine);

        QSignalSpy activeChanged(&lifecycle, &SessionLifecycle::activeChanged);

        QVERIFY(lifecycle.start(nullptr));
        QCOMPARE(engine->runs.size(), 1);
        QCOMPARE(engine->runs.at(0), static_cast<QWindow*>(nullptr));
        QVERIFY(lifecycle.active());
        QVERIFY(activeChanged.count() >= 1);

        // A second start while one is running is refused, not queued.
        QVERIFY(!lifecycle.start(nullptr));
        QCOMPARE(engine->runs.size(), 1);

        lifecycle.interrupt();
        QCOMPARE(engine->interrupts, 1);
    }

    void theHudPublisherReachesTheAttachedSession()
    {
        SessionLifecycle lifecycle;
        auto* engine = new FakeEngineSession;
        lifecycle.attachSession(engine);

        SDL_Surface* surface = reinterpret_cast<SDL_Surface*>(quintptr(0x5678));
        QVERIFY(lifecycle.publishOverlaySurface(surface));
        QCOMPARE(engine->published.size(), 1);
        QCOMPARE(engine->published.at(0), surface);

        // Hiding the overlay is a publish too - a null surface is the contract's "hide".
        QVERIFY(lifecycle.publishOverlaySurface(nullptr));
        QCOMPARE(engine->published.size(), 2);
        QCOMPARE(engine->published.at(1), static_cast<SDL_Surface*>(nullptr));
    }

    // --- release is not destruction -----------------------------------------------------------

    void readyForDeletionDetachesWithoutDestroyingTheSession()
    {
        SessionLifecycle lifecycle;
        auto* engine = new FakeEngineSession;
        QPointer<EngineSession> guard(engine);
        lifecycle.attachSession(engine);
        QVERIFY(lifecycle.start(nullptr));

        QSignalSpy ready(&lifecycle, &SessionLifecycle::readyForDeletion);
        QSignalSpy upstreamChanged(&lifecycle, &SessionLifecycle::upstreamSessionChanged);
        QSignalSpy activeChanged(&lifecycle, &SessionLifecycle::activeChanged);

        emit engine->readyForDeletion();

        QCOMPARE(ready.count(), 1);
        QVERIFY(!lifecycle.active());
        QVERIFY2(lifecycle.upstreamSession() == nullptr, "release must detach the session");
        QVERIFY(upstreamChanged.count() >= 1);
        QVERIFY(activeChanged.count() >= 1);

        // Crucial, and the reason the lifecycle does not own the session: this signal is emitted
        // from the engine's own cleanup task, so the object is still in use. Deleting it here - as
        // this class used to - would be a use-after-free. The facade destroys it later.
        QVERIFY2(!guard.isNull(), "the lifecycle must not destroy the session it was handed");

        // Detached means the next start refuses again: there is no session to run.
        QVERIFY(!lifecycle.start(nullptr));
        QCOMPARE(engine->runs.size(), 1);

        // The signals are disconnected too, so a late engine emission is not re-emitted for a
        // released session.
        emit engine->stageStarting(QStringLiteral("late"));
        QSignalSpy starting(&lifecycle, &SessionLifecycle::stageStarting);
        emit engine->stageStarting(QStringLiteral("late"));
        QCOMPARE(starting.count(), 0);

        delete engine;
    }

    void attachingIsRefusedWhileASessionIsRunning()
    {
        SessionLifecycle lifecycle;
        auto* first = new FakeEngineSession;
        lifecycle.attachSession(first);
        QVERIFY(lifecycle.start(nullptr));

        auto* second = new FakeEngineSession;
        lifecycle.attachSession(second);

        QCOMPARE(lifecycle.upstreamSession(), static_cast<QObject*>(first));

        // Detaching an idle lifecycle is allowed, and takes effect: the null start proves it.
        emit first->readyForDeletion();
        lifecycle.attachSession(nullptr);
        QVERIFY(lifecycle.upstreamSession() == nullptr);
        QVERIFY(!lifecycle.start(nullptr));

        delete first;
        delete second;
    }

    // --- the ordering the facade depends on ---------------------------------------------------

    void theSeamAnnouncesTheHostBeforeItReportsSuccess()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult result;
            result.ok = true;
            result.clientIdentity = QStringLiteral("fingerprint");
            result.host = std::make_shared<FakePairedHost>();
            return result;
        });

        int order = 0;
        int hostOrder = 0;
        int doneOrder = 0;

        // No context object: a direct connection, so the order below is the order of execution
        // inside `ProductionPairingSeam::finish()`.
        connect(&seam, &ProductionPairingSeam::hostResolved, &seam, [&]() { hostOrder = ++order; });

        QSignalSpy hosts(&seam, &ProductionPairingSeam::hostResolved);
        seam.pair(targetForFakeHandshake(), [&](bool ok, const QString&, const QString&) {
            QVERIFY(ok);
            doneOrder = ++order;
        });

        QTRY_VERIFY(doneOrder != 0);

        QCOMPARE(hostOrder, 1);
        QCOMPARE(doneOrder, 2);
        QVERIFY2(hostOrder < doneOrder,
                 "the host must arrive before the completion: handlePairingCompleted() starts the "
                 "stream, and it has to find the engine session already attached");
        QCOMPARE(hosts.count(), 1);
    }

    void theSeamAnnouncesNoHostOnAFailure()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult result;
            result.ok = false;
            result.engineError = QStringLiteral("PIN_WRONG");
            return result;
        });

        QSignalSpy hosts(&seam, &ProductionPairingSeam::hostResolved);

        int doneCalls = 0;
        bool sawOk = true;
        seam.pair(targetForFakeHandshake(), [&](bool ok, const QString&, const QString&) {
            ++doneCalls;
            sawOk = ok;
        });

        QTRY_COMPARE(doneCalls, 1);
        QVERIFY2(!sawOk, "a failed handshake must report failure, never a paired host");
        QCOMPARE(hosts.count(), 0);
    }
};

QTEST_MAIN(TstEngineSeam)

#include "tst_engine_seam.moc"

/*****************************************************************************
 * SeatHub fork - unit tests for silent pairing (Plan 03-03 Task 2, STREAM-03).
 *
 * STREAM-03 is "Customer connects to a rig without typing a PIN". The strongest form of that
 * is not "we did not show a PIN field" - it is that this client has no PIN surface at all, so
 * there is nothing to show. These tests hold that line directly:
 *
 *   * the control-plane-issued PIN reaches the engine seam and nothing else;
 *   * no signal carries it, so no view can obtain it;
 *   * every failure is fail-closed, produces a SeatHub error with a reference, and never a
 *     dialog;
 *   * the 90-second deadline is real.
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <type_traits>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/attempt_vocab.h"
#include "seathub/log_shipper.h"
#include "seathub/log_tee.h"
#include "seathub/pairing_controller.h"
#include "seathub/pairing_key.h"
#include "seathub/pairing_recovery.h"
#include "seathub/pairing_seam.h"

namespace {

class StubHandOff
{
public:
    explicit StubHandOff(bool answer = true)
        : m_answer(answer)
    {
    }

    bool operator()(const ShippedLine& line)
    {
        QMutexLocker locker(&m_mutex);
        m_received.append(line);
        return m_answer;
    }

    QVector<ShippedLine> received() const
    {
        QMutexLocker locker(&m_mutex);
        return m_received;
    }

    int countMatching(const QString& marker) const
    {
        QMutexLocker locker(&m_mutex);
        int count = 0;
        for (const ShippedLine& line : m_received) {
            if (line.body.contains(marker)) {
                ++count;
            }
        }
        return count;
    }

    QVector<ShippedLine> matching(const QString& marker) const
    {
        QMutexLocker locker(&m_mutex);
        QVector<ShippedLine> result;
        for (const ShippedLine& line : m_received) {
            if (line.body.contains(marker)) {
                result.append(line);
            }
        }
        return result;
    }

    void setAnswer(bool answer)
    {
        QMutexLocker locker(&m_mutex);
        m_answer = answer;
    }

private:
    mutable QMutex m_mutex;
    QVector<ShippedLine> m_received;
    bool m_answer;
};

struct StopShipperOnScopeExit
{
    ~StopShipperOnScopeExit()
    {
        LogShipper::instance().stop();
        LogShipper::instance().publishIds(QString(), QString(), QString());
        LogShipper::instance().setCanShip(false);
    }
};

QString ownSpoolFilePath(const QString& directory)
{
    return QDir(directory).filePath(
        QStringLiteral("spool-%1.jsonl").arg(QCoreApplication::applicationPid()));
}

class RawTeeCapture
{
public:
    RawTeeCapture()
    {
        m_handle = LogTee::addSink([this](LogLevel, int, int, const QString& text) {
            if (text.contains(QLatin1String("pairing_instrumentation"))) {
                QMutexLocker locker(&m_mutex);
                m_lines.append(text);
            }
        });
    }

    ~RawTeeCapture()
    {
        if (m_handle != 0) {
            LogTee::removeSink(m_handle);
            m_handle = 0;
        }
    }

    QStringList lines() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines;
    }

    int count() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines.size();
    }

private:
    LogTee::SinkHandle m_handle{0};
    mutable QMutex m_mutex;
    QStringList m_lines;
};

const char* kSessionId = "aaaabbbb-cccc-dddd-eeee-ffff00001111";
const char* kPin = "4821";
const char* kClientUuid = "9f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e";

class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent)
        : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant(httpStatus));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override
    {
        return m_buffer.read(data, maxSize);
    }
    qint64 bytesAvailable() const override
    {
        return m_buffer.size() + QNetworkReply::bytesAvailable();
    }

private:
    QBuffer m_buffer;
};

class FakeNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    QList<int> statuses;
    QList<QByteArray> bodies;
    int calls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override
    {
        const int index = qMin(calls, statuses.size() - 1);
        const int status = statuses.isEmpty() ? 200 : statuses.at(qMax(0, index));
        const QByteArray body = bodies.isEmpty() ? QByteArray() : bodies.at(qMax(0, index));
        ++calls;
        return new FakeReply(status, body, this);
    }
};

// Answers by path, so the session read that rides every poll tick can be told apart from the
// authorization poll it rides with. `/pairing` is always the documented 409 (no target yet).
class PathNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    int sessionStatus = 200;
    QString sessionState = QStringLiteral("PREPARING");
    QString sessionId = QLatin1String(kSessionId);
    int sessionReads = 0;
    int pairingPolls = 0;
    /// Answers for the first `/pairing` requests, one each and in order. Once they are used up the
    /// documented 409 below answers, as it always did.
    QList<QPair<int, QByteArray>> authorizations;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        const QString path = request.url().path();
        if (path.endsWith(QLatin1String("/pairing"))) {
            ++pairingPolls;
            if (!authorizations.isEmpty()) {
                const QPair<int, QByteArray> answer = authorizations.takeFirst();
                return new FakeReply(answer.first, answer.second, this);
            }
            QJsonObject object;
            object.insert(QStringLiteral("status_code"), 409);
            object.insert(QStringLiteral("status"), false);
            object.insert(QStringLiteral("error"), QStringLiteral("The session isn't ready yet."));
            object.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
            return new FakeReply(409, QJsonDocument(object).toJson(QJsonDocument::Compact), this);
        }
        ++sessionReads;
        QJsonObject session;
        session.insert(QStringLiteral("id"), sessionId);
        session.insert(QStringLiteral("state"), sessionState);
        session.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
        session.insert(QStringLiteral("minutes_billed"), 0);
        session.insert(QStringLiteral("reconnect_count"), 0);
        return new FakeReply(sessionStatus, QJsonDocument(session).toJson(QJsonDocument::Compact), this);
    }
};

// The 409 the contract documents for "this session has no pairing target yet".
QByteArray conflictBody()
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 409);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), QStringLiteral("The session isn't ready yet."));
    object.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// `state` defaults to empty, meaning "absent" - the shape an older server still sends, with no
// `state` field on the wire at all. A non-empty value adds it, per contract 3.5.0.
QByteArray authorizationBody(const QString& pin, const QString& state = QString())
{
    QJsonObject ports;
    ports.insert(QStringLiteral("https"), 47984);
    ports.insert(QStringLiteral("control"), 47989);
    ports.insert(QStringLiteral("rtsp"), 48010);

    QJsonObject lease;
    lease.insert(QStringLiteral("lease_id"), QStringLiteral("11111111-2222-3333-4444-555555555555"));
    lease.insert(QStringLiteral("lease_seq"), 1);
    lease.insert(QStringLiteral("kind"), QStringLiteral("connect"));
    lease.insert(QStringLiteral("connect_deadline_at"), QStringLiteral("2026-09-19T00:10:00Z"));

    QJsonObject body;
    body.insert(QStringLiteral("session_id"), QLatin1String(kSessionId));
    body.insert(QStringLiteral("pairing_pin"), pin.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                            : QJsonValue(pin));
    body.insert(QStringLiteral("host_address"), QStringLiteral("203.0.113.7"));
    body.insert(QStringLiteral("ports"), ports);
    body.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    body.insert(QStringLiteral("lease"), lease);
    if (!state.isEmpty()) {
        body.insert(QStringLiteral("state"), state);
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// Records exactly what it was asked to pair against. This is the only thing in the process that
// is allowed to see the PIN.
class RecordingSeam : public PairingSeam
{
public:
    int calls = 0;
    PairingTarget lastTarget;
    bool ok = true;
    QString uuid = QString::fromLatin1(kClientUuid);
    QString engineError;
    // G-06.2-2: what cancel() asked the seam to clear, and how long a pair() answer stays in
    // flight (so a poll can be caught mid-handshake).
    int clears = 0;
    PairingTarget lastClearedTarget;
    int responseDelayMs = 0;

    void pair(const PairingTarget& target,
              std::function<void(bool, const QString&, const QString&)> done) override
    {
        ++calls;
        lastTarget = target;
        // Delivered asynchronously, like a real handshake.
        QTimer::singleShot(responseDelayMs, [done, this]() { done(ok, uuid, engineError); });
    }

    void clearPendingPairing(const PairingTarget& target) override
    {
        ++clears;
        lastClearedTarget = target;
    }
};

// Instantiates the client the controller drives, points it at the fake transport, and hands it
// over. `ControlPlaneClient::setNetworkAccessManager` is the production injection seam, so the
// test exercises the real request path with no server anywhere.
ControlPlaneClient* wire(PairingController& controller, QNetworkAccessManager* fake)
{
    auto* client = new ControlPlaneClient(&controller);
    client->setNetworkAccessManager(fake);
    controller.setControlPlane(client);
    return client;
}

// ---------------------------------------------------------------------------------------------
// The production seam. Its handshake is the one part of silent pairing that cannot run here - it
// needs a Sunshine host - so these tests fake the handshake and hold the seam's own contract: the
// deadline, the exactly-once rule, the fail-closed branches, and the PIN's confinement.
// ---------------------------------------------------------------------------------------------

PairingTarget pairingTarget()
{
    PairingTarget target;
    target.sessionId = QString::fromLatin1(kSessionId);
    target.hostAddress = QStringLiteral("203.0.113.7");
    target.httpsPort = 19995;
    target.controlPort = 20000;
    target.pairingPin = QString::fromLatin1(kPin);
    return target;
}

PairingHandshakeResult handshakeResult(bool ok, const QString& identity,
                                       const QString& diagnostic = QString())
{
    PairingHandshakeResult result;
    result.ok = ok;
    result.clientIdentity = identity;
    result.engineError = diagnostic;
    return result;
}

/// The one nondeterministic value in a `stage=result` record is `handshake_ms`; every other byte is
/// compared exactly. A non-numeric or missing value stays as it is and fails the comparison.
QString withMsNormalized(QString body)
{
    return body.replace(QRegularExpression(QStringLiteral("handshake_ms=\\d+$")),
                        QStringLiteral("handshake_ms=N"));
}

/// Everything the seam said, in the order it said it.
struct SeamReport
{
    QVector<bool> outcomes;
    QVector<QString> identities;
    QVector<QString> diagnostics;

    int count() const { return outcomes.size(); }
};

std::function<void(bool, const QString&, const QString&)> recordInto(SeamReport* report)
{
    return [report](bool ok, const QString& identity, const QString& diagnostic) {
        report->outcomes.append(ok);
        report->identities.append(identity);
        report->diagnostics.append(diagnostic);
    };
}

/// Cuts the global pool to one thread and holds that thread, for as long as the object lives or
/// until `open()`. Everything queued behind it then runs strictly in the order it was queued once
/// it opens, which is what lets a test say "this clear was queued BEFORE the newer handshake
/// started, and runs AFTER it" without a sleep. `drain()` waits the pool out so nothing is left
/// running against the test's locals.
class SerialPool
{
public:
    SerialPool()
        : m_pool(QThreadPool::globalInstance()),
          m_previous(m_pool->maxThreadCount())
    {
        m_pool->setMaxThreadCount(1);
        m_pool->start([this]() { m_gate.acquire(); });
    }

    ~SerialPool()
    {
        drain();
        m_pool->setMaxThreadCount(m_previous);
    }

    void open()
    {
        if (!m_open) {
            m_open = true;
            m_gate.release();
        }
    }

    void drain()
    {
        open();
        m_pool->waitForDone(10000);
    }

private:
    QThreadPool* m_pool;
    int m_previous;
    QSemaphore m_gate;
    bool m_open = false;
};

// A reply that never completes on its own - it stands for a request stalled on a dead connection,
// where the ordinary `finished()`-driven poll never returns.
class StallingReply : public QNetworkReply
{
    Q_OBJECT

public:
    explicit StallingReply(QObject* parent) : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
    }

    void abort() override {}
    qint64 readData(char*, qint64) override { return 0; }
};

class StallingNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    int calls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override
    {
        ++calls;
        return new StallingReply(this);
    }
};

} // namespace

class TstPairing : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
        LogTee::install();
    }

    void theDefaultsAre30sAnd1s()
    {
        // 06.1's ADR item R1: 30 s from READY. J-16: 1 s. Neither is the client's to choose.
        QCOMPARE(PairingController::kPollIntervalMs, 1000);
        QCOMPARE(PairingController::kDeadlineMs, 30000);

        // And a fresh controller actually holds them, before `setDeadlineMs`/`setPollIntervalMs`
        // (the test-only overrides used everywhere else in this file) ever run.
        PairingController controller;
        QCOMPARE(controller.pollIntervalMs(), 1000);
        QCOMPARE(controller.deadlineMs(), 30000);
    }

    void theClockCountsFromReady()
    {
        // R1: the clock is scoped to a pairing resolving once the rig is READY, not to the rig
        // getting ready. A run of waiting answers restarts it on every "not yet" - the same rule
        // `conflict_doesNotSpendThePairingDeadline` already holds for the 409 shape - and it
        // must still pair once READY with a PIN finally arrives, however long the wait ran.
        {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(20);
            controller.setDeadlineMs(100);

            // Each poll spends two replies; forty waiting answers is about 400 ms of "not ready",
            // several times the 100 ms deadline under test - and it must still pair.
            for (int i = 0; i < 40; ++i) {
                fake->statuses.append(200);
                fake->bodies.append(authorizationBody(QString(), QStringLiteral("PREPARING")));
            }
            fake->statuses.append(200);
            fake->bodies.append(authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")));

            QSignalSpy failed(&controller, &PairingController::pairingFailed);
            QSignalSpy completed(&controller, &PairingController::pairingCompleted);

            controller.start(QString::fromLatin1(kSessionId));
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(seam->calls, 1);
        }

        // A READY answer with no PIN pending is not the rig getting ready - it is a pairing that
        // has not resolved - so it does spend this clock, and fails closed with the timed-out
        // sentence once the deadline passes.
        {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(1);
            controller.setDeadlineMs(1);

            fake->statuses = { 200 };
            fake->bodies = { authorizationBody(QString(), QStringLiteral("READY")) };

            QSignalSpy failed(&controller, &PairingController::pairingFailed);
            controller.start(QString::fromLatin1(kSessionId));

            QTRY_COMPARE(failed.count(), 1);
            const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
            QCOMPARE(failure.error, QStringLiteral("The rig didn't finish connecting. Try again."));
            QCOMPARE(seam->calls, 0);
        }
    }

    void happyPath_authorizesThenPairsAndNeverInvolvesTheCustomer()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(granted.count(), 1);
        QCOMPARE(failed.count(), 0);

        // The seam got the address and the PIN...
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.hostAddress, QStringLiteral("203.0.113.7"));
        QCOMPARE(seam->lastTarget.httpsPort, 47984);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
        QCOMPARE(seam->lastTarget.sessionId, QString::fromLatin1(kSessionId));

        // ...and the result carries the exact Sunshine client UUID, which is the only thing that
        // ever identifies this client (Pitfall 3, D-07).
        QCOMPARE(completed.at(0).at(0).toString(), QString::fromLatin1(kClientUuid));
        QCOMPARE(controller.state(), QStringLiteral("ready"));
    }

    void thePinIsNotOnAnySignal()
    {
        // The PIN's only route out of the control plane is the engine seam. If it ever appeared
        // in a signal argument a view could show it, which is the thing STREAM-03 forbids.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);

        // Every argument of every emission, checked for the PIN. `QSignalSpy` is a QObject and
        // cannot be copied into a container, so the check is applied to each spy in turn.
        const auto assertNoPin = [](const QSignalSpy& spy) {
            for (const QList<QVariant>& emission : spy) {
                for (const QVariant& argument : emission) {
                    QVERIFY2(!argument.toString().contains(QLatin1String(kPin)),
                             "the PIN must never be a signal argument");
                }
            }
        };
        assertNoPin(granted);
        assertNoPin(completed);

        // The controller exposes no accessor that returns it either.
        QVERIFY(!controller.sessionId().contains(QLatin1String(kPin)));
    }

    void authorizationGrantedCarriesNoQualityProfile()
    {
        // A-68 / D-06 reversal: the authorization's `quality_profile` is parsed into
        // `SessionAuthorization` (proven elsewhere - `tst_control_plane.cpp`) but nothing reads
        // it any more, so this signal - the only thing that could have carried it onward - now
        // carries nothing at all. This test replaces `authorizationGrantedCarriesTheQualityProfile`
        // (06.6-19), which asserted the exact D-37/WR-05 override contract this plan removes.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);

        QCOMPARE(granted.count(), 1);
        // No field rides this signal any more - the lease, the ports, the (unused) quality
        // profile and the PIN all stay inside the controller (STREAM-03).
        QCOMPARE(granted.at(0).size(), 0);
    }

    void conflict_isAWaitNotAFailure()
    {
        // `GET /api/sessions/{id}/pairing` 409s until the host has a pairing target. The early
        // polls are expected and must not surface as an error.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409, 409, 200 };
        fake->bodies = { conflictBody(), conflictBody(),
                         authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    // -----------------------------------------------------------------------------------------
    // J-16 / 06.1's ADR item 3: the tracer for the waiting branch (Plan 09, Task 1).
    // -----------------------------------------------------------------------------------------

    void aWaitingAnswerIsNotAFailure()
    {
        // A 200 answer whose state is REQUESTED, ALLOCATED or PREPARING fails nothing, does not
        // emit `authorizationGranted`, restarts the clock and schedules the next poll - exactly
        // like the 409 branch it replaces on a modern server.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString(), QStringLiteral("PREPARING")),
                         authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        // Granted exactly once - for the READY answer, never for the waiting one.
        QCOMPARE(granted.count(), 1);
        QCOMPARE(seam->calls, 1);
    }

    void aReadyAnswerWithAPinPairs()
    {
        // A 200 answer with `state: "READY"` and a PIN starts pairing as today.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
    }

    void anOlderServers409IsStillAWait()
    {
        // The existing 409 behaviour, for an older server that has not moved to the 200-with-
        // state shape, is unchanged.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409, 200 };
        fake->bodies = { conflictBody(), authorizationBody(QString::fromLatin1(kPin),
                                                            QStringLiteral("READY")) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void anOlderServers200WithoutStateAndNoPinPollsAgain()
    {
        // An answer with no `state` field at all (an older server) and a null PIN keeps today's
        // behaviour: granted, then poll again rather than seating an empty PIN into the engine.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString()), authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
    }

    void nullPin_pollsAgainRatherThanPairingWithNothing()
    {
        // `pairing_pin` is nullable: null means the host has not been told to expect this client
        // yet. Seating an empty PIN into the engine would be a wasted handshake.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString()), authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
    }

    void conflict_doesNotSpendThePairingDeadline()
    {
        // A 409 means the rig is still being prepared. D-08's 90 s is the time a pairing has to
        // resolve (`docs/spec/README.md`), not the time the rig takes to get ready - that has its
        // own server deadline, and a session it fails reaches this client as a terminal session
        // read. Live on 2026-09-23 "Preparing the rig" took 64-81 s on a single-OS rig and the
        // clock that started at Play ran out before the rig could pair, so the customer had to
        // press Try again. Here the 409s outlast the deadline several times over, then the PIN
        // arrives, and the pairing must still go ahead.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(20);
        controller.setDeadlineMs(100);

        // Each poll spends two replies (the authorization and the session read), so forty 409s
        // are about twenty polls, about 400 ms of "not yet" against a 100 ms deadline.
        for (int i = 0; i < 40; ++i) {
            fake->statuses.append(409);
            fake->bodies.append(conflictBody());
        }
        fake->statuses.append(200);
        fake->bodies.append(authorizationBody(QString::fromLatin1(kPin)));

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
    }

    void deadline_expiresAndFailsClosedWithASeatHubError()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);
        controller.setDeadlineMs(1);

        // Never resolves: the rig has a pairing target but no PIN ever arrives for it. (A run of
        // 409s no longer spends this deadline - see conflict_doesNotSpendThePairingDeadline.)
        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString()) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);

        // A SeatHub failure with a readable reason - never a dialog, never a Moonlight error
        // surface (D-51). No reference: this deadline is the client's own, and ADR-0008 says a
        // reference the client generates resolves to nothing, which is worse than showing none.
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        QVERIFY(!failure.error.isEmpty());
        QVERIFY2(failure.reference.isEmpty(),
                 "a client-side deadline must not invent an ADR-0008 reference code");
        QCOMPARE(failure.kind, FailureKind::Local);
        QCOMPARE(seam->calls, 0);
        QCOMPARE(controller.state(), QStringLiteral("failed"));
    }

    void controlPlaneRefusalOnHttp200_isAFailure()
    {
        // Pitfall 4: the control plane's `Error` body is valid JSON on an HTTP 200.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        QJsonObject body;
        body.insert(QStringLiteral("status_code"), 200);
        body.insert(QStringLiteral("status"), false);
        body.insert(QStringLiteral("error"), QStringLiteral("That session has already ended."));
        body.insert(QStringLiteral("reference"), QStringLiteral("SH-4F7KQ2"));

        fake->statuses = { 200 };
        fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        // The control plane's own sentence and reference, not the client's substitute.
        QCOMPARE(failure.error, QStringLiteral("That session has already ended."));
        QCOMPARE(failure.reference, QStringLiteral("SH-4F7KQ2"));
        QCOMPARE(failure.kind, FailureKind::Api);
        QCOMPARE(seam->calls, 0);
    }

    void engineFailure_keepsItsTextOutOfWhatTheCustomerReads()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->ok = false;
        seam->engineError = QStringLiteral("NvPairingManager: salt mismatch at 0x7ffd");
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();

        // D-51: engine text is diagnostic only.
        QVERIFY2(!failure.error.contains(QStringLiteral("NvPairingManager")),
                 qPrintable(failure.error));
        QVERIFY(!failure.error.contains(QStringLiteral("0x7ffd")));
        QCOMPARE(failure.diagnostic, QStringLiteral("NvPairingManager: salt mismatch at 0x7ffd"));
        // And `toVariantMap()` - the map QML actually sees - drops it entirely.
        QVERIFY(!failure.toVariantMap().contains(QStringLiteral("diagnostic")));
    }

    void successWithoutAClientUuid_isARefusal()
    {
        // The UUID is the only identifier teardown can verify against (Pitfall 3). A handshake
        // that reports success without one cannot be torn down, so it is not a success.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->uuid.clear();
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
    }

    void noSeam_failsClosedRatherThanClaimingSuccess()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        wire(controller, fake);
        // No seam set.

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
    }

    void theSessionIsReadOnEveryPollTickAndReportedAsTheServerSaidIt()
    {
        // CUST-12 / ADR-0055: the connecting stages come from the session's own state, read on the
        // tick this controller already runs - the same interval, no timer of its own.
        PairingController controller;
        auto* fake = new PathNetworkAccessManager;
        wire(controller, fake);
        controller.setPollIntervalMs(1);

        QSignalSpy reads(&controller, &PairingController::sessionRead);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_VERIFY(reads.count() >= 2);
        QCOMPARE(reads.at(0).at(0).value<SessionInfo>().state, QStringLiteral("PREPARING"));
        QCOMPARE(reads.at(0).at(0).value<SessionInfo>().id, QString::fromLatin1(kSessionId));

        // The answer moves on as the session does.
        fake->sessionState = QStringLiteral("READY");
        QTRY_COMPARE(reads.last().at(0).value<SessionInfo>().state, QStringLiteral("READY"));

        // One read per authorization poll at most: it rides the tick, it does not add ticks.
        QVERIFY(fake->sessionReads <= fake->pairingPolls);
        QCOMPARE(failed.count(), 0);

        // And it stops with the poll.
        controller.cancel();
        const int readsAtCancel = fake->sessionReads;
        QTest::qWait(30);
        QCOMPARE(fake->sessionReads, readsAtCancel);
    }

    void aSessionReadThatFailedIsDroppedAndDoesNotFailPairing()
    {
        PairingController controller;
        auto* fake = new PathNetworkAccessManager;
        wire(controller, fake);
        controller.setPollIntervalMs(1);
        QSignalSpy reads(&controller, &PairingController::sessionRead);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        // A read that failed says nothing about the session, and does not fail pairing.
        fake->sessionStatus = 500;
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_VERIFY(fake->sessionReads >= 3);
        QCOMPARE(reads.count(), 0);
        QCOMPARE(failed.count(), 0);

        controller.cancel();
    }

    void cancel_stopsEverything()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409 };
        fake->bodies = { conflictBody() };

        controller.start(QString::fromLatin1(kSessionId));
        controller.cancel();

        QCOMPARE(controller.state(), QStringLiteral("idle"));
        QVERIFY(controller.sessionId().isEmpty());

        const int callsAtCancel = fake->calls;
        QTest::qWait(20);
        // No further polls: cancel really stopped the loop rather than merely changing a label.
        QCOMPARE(fake->calls, callsAtCancel);
    }

    void emptySessionId_failsImmediately()
    {
        PairingController controller;
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString());
        QCOMPARE(failed.count(), 1);
    }

    // -----------------------------------------------------------------------------------------
    // The production seam (`app/seathub/pairing_seam.{h,cpp}`, Plan 03-03 gap closure).
    // -----------------------------------------------------------------------------------------

    void seam_reportsThePairedIdentityToItsCaller()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            return handshakeResult(true, QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(report.outcomes.first(), true);
        QCOMPARE(report.identities.first(),
                 QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
        QVERIFY(report.diagnostics.first().isEmpty());
    }

    void seam_thePinReachesTheHandshakeAndNothingElseReportsIt()
    {
        ProductionPairingSeam seam;
        QString pinSeenByTheHandshake;
        seam.setHandshake([&pinSeenByTheHandshake](const PairingTarget& target) {
            pinSeenByTheHandshake = target.pairingPin;
            return handshakeResult(true, QStringLiteral("aa"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // The handshake is the only holder, and the PIN is not on the way back out - not in the
        // identity, not in the diagnostic, which is the value the controller keeps for support.
        QCOMPARE(pinSeenByTheHandshake, QString::fromLatin1(kPin));
        QVERIFY(!report.identities.first().contains(QLatin1String(kPin)));
        QVERIFY(!report.diagnostics.first().contains(QLatin1String(kPin)));
    }

    void seam_deadline_failsClosedAndSpeaksOnce()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(50);

        // A handshake that does not come back inside the deadline. This is not a contrivance: the
        // first upstream pairing request is issued with no client-side timeout at all
        // (`app/backend/nvpairingmanager.cpp:237` passes 0), so it blocks until the host side
        // answers - up to Sunshine's five-minute pending-session lifetime.
        QSemaphore release;
        seam.setHandshake([&release](const PairingTarget&) {
            release.acquire();
            return handshakeResult(true, QStringLiteral("late"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 5000);

        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QVERIFY(!report.diagnostics.first().isEmpty());

        // The handshake finally returns. It does not get a second word: `done` is called exactly
        // once per `pair()`.
        release.release();
        QTest::qWait(150);
        QCOMPARE(report.count(), 1);
    }

    void seam_withoutAHandshake_failsClosed()
    {
        ProductionPairingSeam seam;
        // No handshake installed - the state 03-03 shipped.

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));

        QCOMPARE(report.count(), 1);
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
    }

    void seam_successWithoutAnIdentity_isNotSuccess()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            // `ok` with nothing that identifies the client: there is no way to verify a teardown
            // against it, so it cannot be reported as a pairing (fail closed, Pitfall 3).
            return handshakeResult(true, QString());
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
    }

    void seam_incompleteTarget_neverStartsAHandshake()
    {
        ProductionPairingSeam seam;
        int handshakes = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            return handshakeResult(true, QStringLiteral("aa"));
        });

        PairingTarget noAddress = pairingTarget();
        noAddress.hostAddress.clear();
        SeamReport addressReport;
        seam.pair(noAddress, recordInto(&addressReport));
        QCOMPARE(addressReport.count(), 1);
        QCOMPARE(addressReport.outcomes.first(), false);

        PairingTarget noPin = pairingTarget();
        noPin.pairingPin.clear();
        SeamReport pinReport;
        seam.pair(noPin, recordInto(&pinReport));
        QCOMPARE(pinReport.count(), 1);
        QCOMPARE(pinReport.outcomes.first(), false);

        PairingTarget noControlPort = pairingTarget();
        noControlPort.controlPort = 0;
        SeamReport portReport;
        seam.pair(noControlPort, recordInto(&portReport));
        QCOMPARE(portReport.count(), 1);
        QCOMPARE(portReport.outcomes.first(), false);

        QCOMPARE(handshakes, 0);
    }

    // -----------------------------------------------------------------------------------------
    // 06.6-18/T-06.6-53: a new pair() while a stale handshake is still running abandons it rather
    // than refusing - refusing would leave a customer's Try again stuck behind a handshake that
    // will never resolve into their new session anyway. Upstream's own blocking call cannot be
    // aborted, so the old one simply keeps its pool thread until it returns and finds nothing
    // waiting for it: the seam's generation guard is what makes that harmless.
    // -----------------------------------------------------------------------------------------

    void aSupersededHandshakeResultIsDropped()
    {
        ProductionPairingSeam seam;
        QSemaphore releaseA;
        // Unconditional: an assertion below failing (RED, or a future regression) must not leave
        // A's pool thread blocked forever - a leaked `releaseA.acquire()` hangs the whole binary at
        // exit, not just this one test.
        auto cleanup = qScopeGuard([&releaseA]() { releaseA.release(); });

        int handshakes = 0;
        seam.setHandshake([&releaseA, &handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                // A's own handshake: blocks until the test releases it, well after B has already
                // been reported.
                releaseA.acquire();
                return handshakeResult(true, QStringLiteral("late-a"));
            }
            // B's own handshake: nothing supersedes it, so it returns at once.
            return handshakeResult(true, QStringLiteral("b-identity"));
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));

        // Wait until A's handshake is actually inside the pool thread before B supersedes it -
        // that is the state this behavior is about.
        QTRY_COMPARE(handshakes, 1);
        seam.pair(targetB, recordInto(&reportB));

        // B is not refused "already in flight": its own handshake starts at once.
        QTRY_COMPARE(handshakes, 2);
        QTRY_COMPARE(reportB.count(), 1);
        QCOMPARE(reportB.outcomes.first(), true);
        QCOMPARE(reportB.identities.first(), QStringLiteral("b-identity"));

        // A's handshake finally returns (the scope guard above releases it, exactly once - a
        // second, explicit release here would double it). It gets no word: its generation is
        // stale by the time it arrives, superseded by B's own `pair()` call.
        cleanup.dismiss();
        releaseA.release();
        QTest::qWait(150);
        QCOMPARE(reportA.count(), 0);
    }

    void theDeadlineBelongsToTheCurrentHandshake()
    {
        ProductionPairingSeam seam;
        // 500 ms rather than the 150 ms this test once used: the machine-load assumption baked
        // into the old numbers - that `QTRY_COMPARE` + `qWait(100)` land inside A's window -
        // flaked on a loaded machine (A's deadline could fire before B even superseded it,
        // consuming A itself). The property under test is unchanged: B's deadline counts from
        // B's own `pair()` call, and without the restart B's deadline never fires at all, which
        // the `QTRY_VERIFY` below still catches.
        seam.setDeadlineMs(500);

        QSemaphore release;
        // Unconditional, for the same reason as `aSupersededHandshakeResultIsDropped` above: two
        // pool threads can be blocked here, and an early-failing assertion must not leave either
        // of them stuck forever.
        auto cleanup = qScopeGuard([&release]() { release.release(2); });

        int handshakes = 0;
        seam.setHandshake([&release, &handshakes](const PairingTarget&) {
            ++handshakes;
            release.acquire();
            return handshakeResult(true, QStringLiteral("late"));
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));
        QTRY_COMPARE(handshakes, 1);

        // A is still deep inside its own 500 ms window when B supersedes it.
        QTest::qWait(100);
        seam.pair(targetB, recordInto(&reportB));
        QTRY_COMPARE(handshakes, 2);

        // If the deadline still belonged to A, it could not have carried B: B's own deadline is
        // a fresh 500 ms counted from B's own `pair()` call, not from whatever remained of A's.
        // Nothing may have fired for B yet.
        QCOMPARE(reportB.count(), 0);

        // B's own full deadline does still apply.
        QTRY_VERIFY_WITH_TIMEOUT(reportB.count() == 1, 5000);
        QCOMPARE(reportB.outcomes.first(), false);

        // Neither blocked handshake gets a word after this: both are stale by the time they
        // return. The scope guard above does the releasing, exactly once each.
        cleanup.dismiss();
        release.release(2);
        QTest::qWait(100);
        QCOMPARE(reportA.count(), 0);
        QCOMPARE(reportB.count(), 1);
    }

    // -----------------------------------------------------------------------------------------
    // G-06.2-2 / Plan 06.2-12 Task 1: pairing conflict recovery.
    //
    // The rig holds pending pairing sessions keyed by the shared upstream uniqueid; a 409 on
    // getservercert means a half-open session holds it. The seam worker's recovery loop clears
    // the rig state and retries exactly once; a non-conflict failure clears without retrying; a
    // superseded run clears nothing (the newer run owns the rig) and is dropped.
    // -----------------------------------------------------------------------------------------

    void seamConflict_clearsOnceRetriesOnce_reportsTheRetry()
    {
        ProductionPairingSeam seam;
        QStringList order;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            order.append(QStringLiteral("run%1").arg(handshakes));
            if (handshakes == 1) {
                PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                                  QStringLiteral("conflict"));
                conflict.pairingConflict = true;
                return conflict;
            }
            return handshakeResult(true, QStringLiteral("retry-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            order.append(QStringLiteral("cancel"));
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // Exactly two handshake runs, exactly one clear between them, and the clear completed
        // before the retry started.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QCOMPARE(order.join(QLatin1Char(',')), QStringLiteral("run1,cancel,run2"));

        // The retry's result is what the caller hears - reported as-is, once.
        QCOMPARE(report.outcomes.first(), true);
        QCOMPARE(report.identities.first(), QStringLiteral("retry-identity"));
        QVERIFY(report.diagnostics.first().isEmpty());
    }

    void seamConflictTwice_reportsTheSecondFailureOnce()
    {
        ProductionPairingSeam seam;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                              QStringLiteral("still-conflict-%1")
                                                                  .arg(handshakes));
            conflict.pairingConflict = true;
            return conflict;
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // A clean retry that still fails is a failure - the caller hears the SECOND run's own
        // text, once. No third run: exactly one retry, however the retry ends.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QCOMPARE(report.diagnostics.first(), QStringLiteral("still-conflict-2"));
    }

    void seamNonConflictFailure_oneClearOneRetry()
    {
        ProductionPairingSeam seam;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            // No conflict flag: a plain failure. The failed run may still have left a pending
            // session on the rig, so it gets one clear; SC18 then grants the Play its one retry
            // (a plain failure used to get none), and the retry never repeats the clear.
            return handshakeResult(false, QString(), QStringLiteral("plain-failure"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QCOMPARE(report.outcomes.first(), false);
        QCOMPARE(report.diagnostics.first(), QStringLiteral("plain-failure"));
    }

    void aWrongPinFailureCarriesStepAndClassButNeverTheDiagnostic()
    {
        // ADR-0072 items 1-2: a failed handshake reports WHICH step failed and WHY as closed
        // vocabulary tokens, while `engineError` keeps its local diagnostic for support only.
        // The fake handshake builds exactly what `pairing_handshake.cpp` builds for
        // `NvPairingManager::PIN_WRONG` (the mapper itself is asserted in
        // `tst_attempt_vocab::pairStateMapsToClosedClasses`, which this suite does not link);
        // what is under test here is that the classification survives the seam and reaches the
        // facade as separate members, and that it is never the diagnostic text.
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult rejected;
            rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
            rejected.attemptStep = QStringLiteral("pair_handshake");
            rejected.stepClass = QStringLiteral("pin_rejected");
            rejected.attempts = 1;
            return rejected;
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // The failure itself: not ok, no identity, and the local diagnostic unchanged - it is
        // still there for support, and it is still the only text in the result.
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QCOMPARE(report.diagnostics.first(), QStringLiteral("the rig rejected the pairing PIN"));

        // The classification handed to the facade, as separate members.
        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        // SC18: a wrong-PIN failure is a non-PAIRED result, so the Play retried once; the final
        // class is the same and the first failure rides the record beside it.
        QCOMPARE(delivered.attempts, 2);
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstAttemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("failure"));
        QVERIFY(delivered.pinShapeOk);
        QVERIFY(delivered.handshakeMs >= 0);

        // The step is one of the 11 frozen tokens, read from the header block the vocabulary
        // script checks - a step outside it could never reach `/end` or a Sentry query.
        bool stepIsFrozen = false;
        for (int i = 0; i < kAttemptStepCount; ++i) {
            if (delivered.attemptStep == QLatin1String(kAttemptStepTokens[i])) {
                stepIsFrozen = true;
            }
        }
        QVERIFY2(stepIsFrozen, "the step must be one of the 11 frozen attempt-step tokens");

        // Never the diagnostic: the class is a token in its own right, and the local text stays
        // out of it (the redaction rule, ADR-0072 item 2).
        QVERIFY(!delivered.engineError.isEmpty());
        QVERIFY2(delivered.stepClass != delivered.engineError,
                 qPrintable(QStringLiteral("the class must not be the diagnostic: %1")
                                .arg(delivered.stepClass)));
        QVERIFY2(!delivered.stepClass.contains(delivered.engineError),
                 "the diagnostic must not leak into the class");

        // The same classification on the accessor the facade reads once `pairingFailed` lands.
        QCOMPARE(seam.lastResult().attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(seam.lastResult().stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(seam.lastResult().attempts, 2);
        QCOMPARE(seam.lastResult().engineError, QStringLiteral("the rig rejected the pairing PIN"));
    }

    void aRecoveredConflictSuccessCarriesTheFrozenRecordOutOfTheSeam()
    {
        // ADR-0072 item 2 / `docs/spec/client.md` "Play diagnostics - Rules": a pairing that
        // succeeds only after the clear-and-retry reports ONE record - `step=pair_handshake`,
        // `failure_class=in_progress`, `attempt=2` - and it reaches the facade on the result the
        // seam reports. The first run below is exactly what production builds for the rig's HTTP
        // 409 on getservercert (step `pair_server_info`, class `http_4xx`): the recovered record
        // is FROZEN by the ADR, not inherited from that mapper.
        ProductionPairingSeam seam;
        int handshakes = 0;
        int cancels = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                PairingHandshakeResult conflict;
                conflict.attemptStep = QStringLiteral("pair_server_info");
                conflict.stepClass = QStringLiteral("http_4xx");
                conflict.pairingConflict = true;
                conflict.engineError = QStringLiteral(
                    "the rig did not answer the server-info request (HTTP 409)");
                return conflict;
            }
            PairingHandshakeResult paired;
            paired.ok = true;
            paired.clientIdentity = QStringLiteral("recovered-identity");
            return paired;
        });
        seam.setCancelRequest([&cancels](const QString&, int) {
            ++cancels;
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // One clear, one retry, and the retry's own identity reported - the G-06.2-2 contract.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QVERIFY(report.outcomes.first());
        QCOMPARE(report.identities.first(), QStringLiteral("recovered-identity"));
        QVERIFY(report.diagnostics.first().isEmpty());

        // ...and exactly ONE classification carrying the frozen recovered record, which is what
        // the facade's INFO emission is built from.
        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY2(delivered.ok, "the recovered result must be reported as a success");
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("in_progress"));
        QCOMPARE(delivered.attempts, 2);
        // SC18 fields: the first attempt was a failure (a conflict), the retry was the conflict
        // clear, and the PIN only ever appears as its shape.
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstAttemptStep, QStringLiteral("pair_server_info"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("http_4xx"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("conflict"));
        QVERIFY(delivered.pinShapeOk);
        QVERIFY(delivered.handshakeMs >= 0);

        // No diagnostic text and no address rides the record (ADR-0072 item 2, T-06.7-35).
        QVERIFY2(!delivered.stepClass.isEmpty() && delivered.stepClass != delivered.engineError,
                 "the class must be a token, never the diagnostic");
        QVERIFY2(!delivered.engineError.contains(QStringLiteral("HTTP 409")),
                 "the local diagnostic stays local - it is not part of the record");
    }

    void aRetryThatFailsAgainReportsTheLastFailureClass()
    {
        // ADR-0072 item 2: "A retry that fails again is `outcome=failed`, `failure_class` of the
        // LAST failure, `attempt` 2" - the second run's own classification, not the cleared one's.
        ProductionPairingSeam seam;
        int handshakes = 0;
        int cancels = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                PairingHandshakeResult conflict;
                conflict.attemptStep = QStringLiteral("pair_server_info");
                conflict.stepClass = QStringLiteral("http_4xx");
                conflict.pairingConflict = true;
                conflict.engineError =
                    QStringLiteral("the rig did not answer the server-info request (HTTP 409)");
                return conflict;
            }
            PairingHandshakeResult rejected;
            rejected.attemptStep = QStringLiteral("pair_handshake");
            rejected.stepClass = QStringLiteral("pin_rejected");
            rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
            return rejected;
        });
        seam.setCancelRequest([&cancels](const QString&, int) {
            ++cancels;
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QVERIFY(!report.outcomes.first());

        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY(!delivered.ok);
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.attempts, 2);
    }

    void theEndBodyCarriesTheAttemptStepAndNoOtherNewField()
    {
        // ADR-0072 item 1 / plan 09: the classified step is the ONE thing this plan sends to the
        // server - `attempt_step` on `POST /api/sessions/{id}/end` (contract 3.8.0) - and only
        // when the client classified one. `buildEndRequest` is the exact builder `endSession()`
        // posts, so what it produces here is what the wire carries.
        EndReport report;
        report.stage = QStringLiteral("pairing");
        report.attemptStep = QStringLiteral("pair_handshake");

        const QJsonObject object =
            QJsonDocument::fromJson(ControlPlaneClient::buildEndRequest(true, report)).object();
        QCOMPARE(object.value(QStringLiteral("attempt_step")).toString(),
                 QStringLiteral("pair_handshake"));
        QCOMPARE(object.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(object.value(QStringLiteral("stage")).toString(), QStringLiteral("pairing"));

        // No OTHER field joined the body: the key set is exactly the report's own known fields
        // plus `attempt_step`. The failure class never travels (D-12) - it stays in Sentry.
        QStringList keys = object.keys();
        keys.sort();
        QStringList expected{ QStringLiteral("attempt_step"), QStringLiteral("failed"),
                              QStringLiteral("stage") };
        expected.sort();
        QCOMPARE(keys, expected);

        // An unclassified failure - the controller's own deadline, or a control-plane refusal -
        // leaves the field out entirely: an absent field is not the same as an empty or a guessed
        // one on this leniently-parsed body.
        EndReport unclassified;
        unclassified.stage = QStringLiteral("pairing");
        const QJsonObject without = QJsonDocument::fromJson(
            ControlPlaneClient::buildEndRequest(true, unclassified)).object();
        QVERIFY2(!without.contains(QStringLiteral("attempt_step")),
                 "a step the client never classified must not be invented on the wire");

        // And a customer-initiated end still posts no body at all (contract 3.3.0).
        QVERIFY(ControlPlaneClient::buildEndRequest(false, report).isEmpty());
    }

    void seamSupersededRun_noClearNoRetry_droppedResult()
    {
        ProductionPairingSeam seam;
        QSemaphore releaseA;
        auto cleanup = qScopeGuard([&releaseA]() { releaseA.release(); });
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                // A's handshake: blocked until the test releases it, then returns a conflict
                // failure. By then B has superseded it, and B's run owns the rig state.
                releaseA.acquire();
                PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                                  QStringLiteral("stale"));
                conflict.pairingConflict = true;
                return conflict;
            }
            return handshakeResult(true, QStringLiteral("b-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));
        QTRY_COMPARE(handshakes, 1);
        seam.pair(targetB, recordInto(&reportB));
        QTRY_COMPARE(handshakes, 2);
        QTRY_COMPARE(reportB.count(), 1);
        QCOMPARE(reportB.outcomes.first(), true);

        // A's conflict failure finally lands - stale. It clears nothing (that would kill B's
        // own pending pairing), retries nothing, and is dropped entirely.
        cleanup.dismiss();
        releaseA.release();
        QTest::qWait(150);
        QCOMPARE(reportA.count(), 0);
        QCOMPARE(cancels, 0);
        QCOMPARE(handshakes, 2);
    }

    void singleFlight_aPollDuringAnInFlightHandshakeNeverStartsASecond()
    {
        // One Play attempt sends one getservercert sequence. The 2026-09-29 incident saw three
        // in one second - a poll tick or a push-driven pollNow() re-entering pairing while the
        // handshake was still running - two of them 409s against the rig's half-open session.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->responseDelayMs = 200; // the handshake stays in flight across the polls below
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        for (int i = 0; i < 4; ++i) {
            fake->statuses.append(200);
            fake->bodies.append(authorizationBody(QString::fromLatin1(kPin),
                                                  QStringLiteral("READY")));
        }

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1); // the handshake is in flight

        // Push frames wake the poll twice while it runs. Neither may start a second handshake
        // for the same session.
        controller.pollNow();
        controller.pollNow();
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1);

        // The one in-flight handshake still resolves normally.
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void teardown_cancelRequestsTheRigSideClearOnceForTheLastTarget()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->responseDelayMs = 200; // keep the handshake unresolved across the cancel
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin),
                                           QStringLiteral("READY")) };

        controller.start(QString::fromLatin1(kSessionId));
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1);

        controller.cancel();
        QCOMPARE(seam->clears, 1);
        QCOMPARE(seam->lastClearedTarget.hostAddress, QStringLiteral("203.0.113.7"));
        QCOMPARE(seam->lastClearedTarget.httpsPort, 47984);
        QCOMPARE(seam->lastClearedTarget.sessionId, QString::fromLatin1(kSessionId));

        // A second cancel - teardown paths call this more than once - must not repeat the clear.
        controller.cancel();
        QCOMPARE(seam->clears, 1);
    }

    void teardown_cancelBeforeAHandshakeStartedSendsNoClear()
    {
        // No handshake ran, so this client left no pending pairing session on the rig - and a
        // clear aimed at a session another client may be pairing would cancel THEIR handshake.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 409 };
        fake->bodies = { conflictBody() };

        controller.start(QString::fromLatin1(kSessionId));
        controller.cancel();
        QCOMPARE(seam->clears, 0);
    }

    // PF15-b, UD-04 R06: everything attempt A leaves in flight - the rig-side clear it asked for, the
    // handshake it started, the poll replies it was waiting on - is fenced to A. None of it can
    // cancel, clear, pair or fail attempt B, and none of it needs a sleep to be harmless.
    void staleClearCannotTouchNewAttempt()
    {
        const QString session = QString::fromLatin1(kSessionId);

        // --- the seam: A is cancelled, B starts before A's clear has run. The rig has one pending
        // pairing slot; the clear aimed at A would cancel B's pairing, so it must never go out.
        {
            SerialPool pool;
            ProductionPairingSeam seam;
            std::atomic<int> cancels{0};
            std::atomic<int> handshakes{0};
            seam.setCancelRequest([&cancels](const QString&, int) {
                ++cancels;
                return true;
            });
            seam.setHandshake([&handshakes](const PairingTarget& target) {
                ++handshakes;
                if (target.sessionId == QLatin1String("session-a")) {
                    // A's handshake ends in the rig's already-pairing conflict, which would send its
                    // own clear and then a retry if it were still the current attempt.
                    PairingHandshakeResult conflict =
                        handshakeResult(false, QString(), QStringLiteral("stale"));
                    conflict.pairingConflict = true;
                    return conflict;
                }
                return handshakeResult(true, QStringLiteral("b-identity"));
            });

            PairingTarget targetA = pairingTarget();
            targetA.sessionId = QStringLiteral("session-a");
            PairingTarget targetB = pairingTarget();
            targetB.sessionId = QStringLiteral("session-b");

            SeamReport reportA;
            SeamReport reportB;
            seam.pair(targetA, recordInto(&reportA));
            seam.clearPendingPairing(targetA);          // A is cancelled; its clear is queued
            seam.pair(targetB, recordInto(&reportB));   // B starts before that clear has run
            pool.open();

            QTRY_COMPARE(reportB.count(), 1);
            QCOMPARE(reportB.outcomes.first(), true);
            pool.drain();
            QCOMPARE(cancels.load(), 0);      // not A's clear, and not A's own recovery clear
            QCOMPARE(handshakes.load(), 2);   // A ran once and was not retried
            QCOMPARE(reportA.count(), 0);     // and A never speaks again
        }

        // --- the same cancel with no newer attempt: A's clear still goes out, once, and the
        // abandoned handshake adds neither a clear nor a retry of its own.
        {
            SerialPool pool;
            ProductionPairingSeam seam;
            std::atomic<int> cancels{0};
            std::atomic<int> handshakes{0};
            seam.setCancelRequest([&cancels](const QString&, int) {
                ++cancels;
                return true;
            });
            seam.setHandshake([&handshakes](const PairingTarget&) {
                ++handshakes;
                PairingHandshakeResult conflict =
                    handshakeResult(false, QString(), QStringLiteral("stale"));
                conflict.pairingConflict = true;
                return conflict;
            });

            PairingTarget targetA = pairingTarget();
            targetA.sessionId = QStringLiteral("session-a");
            SeamReport reportA;
            seam.pair(targetA, recordInto(&reportA));
            seam.clearPendingPairing(targetA);
            pool.open();
            pool.drain();

            QCOMPARE(cancels.load(), 1);
            QCOMPARE(handshakes.load(), 1);
            QTest::qWait(30);                 // let any queued result reach the seam
            QCOMPARE(reportA.count(), 0);
        }

        // --- the controller: A's handshake finishes after A was cancelled and B began.
        {
            PairingController controller;
            auto* fake = new PathNetworkAccessManager;
            fake->authorizations.append(qMakePair(
                200, authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY"))));
            auto* seam = new RecordingSeam;
            seam->responseDelayMs = 100;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(60000);

            QSignalSpy completed(&controller, &PairingController::pairingCompleted);
            QSignalSpy failed(&controller, &PairingController::pairingFailed);

            controller.start(session);
            QTRY_COMPARE(seam->calls, 1);   // A's handshake is in flight
            controller.cancel();
            QCOMPARE(seam->clears, 1);      // A's clear was asked of the seam, once
            controller.start(session);      // B: the same session again, as a reconnect does
            QTest::qWait(250);              // A's handshake reports in the meantime

            QCOMPARE(completed.count(), 0);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(controller.state(), QStringLiteral("authorizing"));
            QCOMPARE(seam->calls, 1);       // B has not been paired with A's PIN either
            controller.cancel();
        }

        // --- the controller: A's poll replies arrive after A was cancelled and B began.
        {
            PairingController controller;
            auto* fake = new PathNetworkAccessManager;
            fake->authorizations.append(qMakePair(
                200, authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY"))));
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(60000);

            QSignalSpy reads(&controller, &PairingController::sessionRead);
            QSignalSpy granted(&controller, &PairingController::authorizationGranted);

            controller.start(session);      // A: its authorization and session replies are in flight
            controller.cancel();
            controller.start(session);      // B: gets the documented 409, not A's READY and PIN
            QTRY_COMPARE(fake->pairingPolls, 2);
            QTRY_VERIFY(reads.count() >= 1);
            QTest::qWait(50);               // every reply of both attempts has been delivered

            QCOMPARE(seam->calls, 0);       // A's PIN never reached the seam on B's behalf
            QCOMPARE(granted.count(), 0);
            QCOMPARE(reads.count(), 1);     // only B's own session read speaks for B
            QCOMPARE(controller.state(), QStringLiteral("authorizing"));
            controller.cancel();
        }
    }

    void cancelRequest_theWireCarriesTheUpstreamUniqueidAndCancelPhrase()
    {
        // The rig-side clear only clears when it names the session the rig actually holds: the
        // request line must be exactly what Sunshine's pair() handler reads
        // (`src/nvhttp.cpp:705-713` - a /pair request with uniqueid and a cancel phrase).
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        QString requestLine;
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            QTcpSocket* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &requestLine]() {
                if (requestLine.isEmpty()) {
                    requestLine = QString::fromLatin1(socket->readLine());
                }
                socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
            });
        });

        // Any HTTP answer counts: the 400 here is Sunshine's no-op-clear answer.
        QVERIFY(sendPairingCancelRequest(QStringLiteral("127.0.0.1"), server.serverPort()));

        QTRY_VERIFY(!requestLine.isEmpty());
        QCOMPARE(requestLine.trimmed(),
                 QStringLiteral("GET /pair?uniqueid=0123456789ABCDEF&phrase=cancel HTTP/1.1"));
    }

    void parity_theSharedUniqueIdStillMatchesUpstream()
    {
        // The recovery constant and the handshake's own pairing requests must name the same
        // uniqueid, or the clear would miss the rig's session forever. Upstream's hard-coded
        // value lives at app/backend/nvhttp.cpp:482; this test opens that file and asserts the
        // literal is still there.
        const QString upstream = QStringLiteral("%1/../app/backend/nvhttp.cpp")
                                     .arg(QCoreApplication::applicationDirPath());
        QFile file(upstream);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(upstream));
        QVERIFY(file.readAll().contains(QByteArrayLiteral("0123456789ABCDEF")));
    }

    void fingerprint_ofACertificate_isItsSha256Hex()
    {
        // The fixture is a throwaway self-signed certificate generated for this test; only its
        // public half is stored next to the test source. Expected value: SHA-256 over its DER.
        const QString path = QFINDTESTDATA("fixtures/seathub-client-cert.pem");
        QVERIFY2(!path.isEmpty(), "the certificate fixture was not found next to the test source");

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(clientCertificateFingerprint(file.readAll()),
                 QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
    }

    void fingerprint_ofAnUnreadablePem_isEmpty()
    {
        // No identity is ever invented: an unreadable certificate produces nothing, and the seam
        // turns that into a failure rather than a success with no handle on it.
        QVERIFY(clientCertificateFingerprint(QByteArray()).isEmpty());
        QVERIFY(clientCertificateFingerprint(QByteArray("not a certificate")).isEmpty());
        QVERIFY(clientCertificateFingerprint(
                    QByteArray("-----BEGIN CERTIFICATE-----\nnope\n-----END CERTIFICATE-----\n"))
                    .isEmpty());
    }

    // -----------------------------------------------------------------------------------------
    // ADR-0074 W03, the warm return: a NEW session whose authorization says `warm_return: true`
    // may skip the PIN handshake - and only then, only with host trust this app run pinned itself,
    // and only when the rig's answer confirms it. Every other case runs the normal pairing.
    // -----------------------------------------------------------------------------------------

    void theAuthorizationsWarmReturnReachesTheSeamExactlyAsTheControlPlaneSentIt()
    {
        struct Case { const char* name; QJsonValue value; bool present; bool expected; };
        const QVector<Case> cases = {
            { "true", QJsonValue(true), true, true },
            { "false", QJsonValue(false), true, false },
            { "absent", QJsonValue(), false, false },
            { "null", QJsonValue(QJsonValue::Null), true, false },
            { "the string true", QJsonValue(QStringLiteral("true")), true, false },
        };
        for (const Case& c : cases) {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);

            QJsonObject body = QJsonDocument::fromJson(authorizationBody(QString::fromLatin1(kPin))).object();
            if (c.present) {
                body.insert(QStringLiteral("warm_return"), c.value);
            }
            fake->statuses = { 200 };
            fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

            QSignalSpy completed(&controller, &PairingController::pairingCompleted);
            controller.start(QString::fromLatin1(kSessionId));
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);

            QCOMPARE(seam->calls, 1);
            QVERIFY2(seam->lastTarget.warmReturn == c.expected, c.name);
            // A PIN is still issued and still reaches the seam: a normal pairing is always possible.
            QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
        }
    }

    void warmReturnWithNoPinIsNotAPairing()
    {
        // `warm_return` is advisory. An authorization that carries it but no PIN yet is still "not
        // ready": the controller keeps polling and the seam is never asked.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(20);

        QJsonObject body = QJsonDocument::fromJson(authorizationBody(QString())).object();
        body.insert(QStringLiteral("warm_return"), true);
        fake->statuses = { 200 };
        fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

        controller.start(QString::fromLatin1(kSessionId));
        QTest::qWait(150);
        QCOMPARE(seam->calls, 0);
        controller.cancel();
    }

    void warmReturnSkipsHandshakeOnlyWhenAuthorized()
    {
        const QByteArray pinnedEarlierThisRun = QByteArrayLiteral("-----BEGIN CERTIFICATE-----\nHELD\n-----END CERTIFICATE-----\n");

        std::atomic<int> handshakes{0};
        std::atomic<int> warmChecks{0};
        QByteArray heldSeenByTheCheck;
        bool warmCheckOk = true;
        bool warmCheckHasHost = true;

        const auto makeSeam = [&](ProductionPairingSeam* seam, bool installCheck) {
            seam->setHandshake([&](const PairingTarget&) {
                ++handshakes;
                PairingHandshakeResult result = handshakeResult(true, QStringLiteral("normal-identity"));
                result.host = std::make_shared<PairedHost>();
                return result;
            });
            if (installCheck) {
                seam->setWarmReturnCheck([&](const PairingTarget&, const QByteArray& held) {
                    ++warmChecks;
                    heldSeenByTheCheck = held;
                    PairingHandshakeResult result;
                    result.ok = warmCheckOk;
                    result.clientIdentity = warmCheckOk ? QStringLiteral("warm-identity") : QString();
                    if (warmCheckHasHost) {
                        result.host = std::make_shared<PairedHost>();
                    }
                    return result;
                });
            }
        };
        // The identity the seam reported for one `pair()`, or an empty string on a failed one.
        QString reported;
        const auto run = [&](ProductionPairingSeam* seam, bool warmReturn) {
            PairingTarget target = pairingTarget();
            target.warmReturn = warmReturn;
            SeamReport report;
            seam->pair(target, recordInto(&report));
            QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
            reported = report.identities.first();
        };
        const auto reset = [&]() {
            handshakes = 0;
            warmChecks = 0;
            heldSeenByTheCheck.clear();
            warmCheckOk = true;
            warmCheckHasHost = true;
        };
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });

        // 1. Authorized, trust held, rig confirms: the handshake does not run, and the check was
        //    handed exactly the certificate this run pinned.
        {
            HeldRigTrust::clear();
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("warm-identity"));
            QCOMPARE(handshakes.load(), 0);
            QCOMPARE(warmChecks.load(), 1);
            QCOMPARE(heldSeenByTheCheck, pinnedEarlierThisRun);
        }

        // 2. Trust held and the check would say yes, but the control plane did not say warm: the
        //    normal pairing runs and the check is never even asked.
        {
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, false);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            QCOMPARE(handshakes.load(), 1);
            QCOMPARE(warmChecks.load(), 0);
        }

        // 3. Authorized but nothing held - a restart, or a sign-out cleared it: normal pairing, and
        //    no check is asked of the rig with nothing to pin it by.
        {
            HeldRigTrust::clear();
            reset();
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            QCOMPARE(handshakes.load(), 1);
            QCOMPARE(warmChecks.load(), 0);
        }

        // 4. Authorized and held, but the check fails or the rig answers not paired: the normal
        //    pairing runs after it, once, and that is what is reported.
        {
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            warmCheckOk = false;
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            QCOMPARE(warmChecks.load(), 1);
            QCOMPARE(handshakes.load(), 1);
        }

        // 5. A check that says yes without handing back a host record cannot start a stream: it
        //    does not count, and the normal pairing runs.
        {
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            warmCheckHasHost = false;
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            QCOMPARE(handshakes.load(), 1);
        }

        // 6. No check installed at all: a warm indication changes nothing.
        {
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            ProductionPairingSeam seam;
            makeSeam(&seam, false);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            QCOMPARE(handshakes.load(), 1);
            QCOMPARE(warmChecks.load(), 0);
        }

        // 7. The next Play after a warm one is a fresh decision: authorized again it skips again,
        //    not authorized it pairs, with the same trust still held across both.
        {
            HeldRigTrust::remember(pinnedEarlierThisRun);
            reset();
            ProductionPairingSeam seam;
            makeSeam(&seam, true);
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("warm-identity"));
            run(&seam, false);
            QCOMPARE(reported, QStringLiteral("normal-identity"));
            run(&seam, true);
            QCOMPARE(reported, QStringLiteral("warm-identity"));
            QCOMPARE(handshakes.load(), 1);
            QCOMPARE(warmChecks.load(), 2);
        }
    }

    // -----------------------------------------------------------------------------------------
    // L07: a warm SAME-SESSION reconnect. An `ACTIVE` session reconnecting inside its disconnect
    // grace now carries `warm_return` too (contract 3.16.0). The attempt skips the PIN when the
    // rig confirms the pin this run already holds, and falls back to the PIN handshake when that
    // fails - one handshake at a time, whichever way it goes.
    // -----------------------------------------------------------------------------------------

    void aFailedWarmReconnectFallsBackToThePin()
    {
        const QByteArray firstPin = QByteArrayLiteral("-----BEGIN CERTIFICATE-----\nFIRST\n-----END CERTIFICATE-----\n");
        const QByteArray secondPin = QByteArrayLiteral("-----BEGIN CERTIFICATE-----\nSECOND\n-----END CERTIFICATE-----\n");
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        HeldRigTrust::clear();
        HeldRigTrust::remember(firstPin);

        std::atomic<int> handshakes{0};
        std::atomic<int> warmChecks{0};
        std::atomic<int> running{0};
        std::atomic<int> mostAtOnce{0};
        std::atomic<bool> warmCheckOk{false};
        QByteArray heldSeenByTheCheck;
        // The warm check and the PIN handshake both count themselves in and out, so "never two at
        // once" is something the test reads, not something it hopes for.
        const auto enter = [&]() {
            const int now = ++running;
            int seen = mostAtOnce.load();
            while (now > seen && !mostAtOnce.compare_exchange_weak(seen, now)) {
            }
            QThread::msleep(15);
        };

        // Declared before the controller, so the controller (which polls through the seam) is
        // destroyed first.
        ProductionPairingSeam seam;
        seam.setHandshake([&](const PairingTarget&) {
            enter();
            ++handshakes;
            // Production remembers the pin a full pairing produced (`pairing_handshake.cpp`).
            HeldRigTrust::remember(secondPin);
            PairingHandshakeResult result = handshakeResult(true, QStringLiteral("pin-identity"));
            result.host = std::make_shared<PairedHost>();
            --running;
            return result;
        });
        seam.setWarmReturnCheck([&](const PairingTarget& target, const QByteArray& held) {
            enter();
            ++warmChecks;
            heldSeenByTheCheck = held;
            PairingHandshakeResult result;
            // The check never sees the PIN's use: it is handed the target only to know the rig.
            result.ok = warmCheckOk.load() && !target.hostAddress.isEmpty();
            result.clientIdentity = result.ok ? QStringLiteral("warm-identity") : QString();
            if (result.ok) {
                result.host = std::make_shared<PairedHost>();
            }
            --running;
            return result;
        });

        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        wire(controller, fake);
        controller.setSeam(&seam);

        // The reconnect's authorization: the SAME session, ACTIVE, `warm_return` true, and a PIN
        // (a PIN is always issued, so a normal pairing is always possible).
        QJsonObject body = QJsonDocument::fromJson(
            authorizationBody(QString::fromLatin1(kPin), QStringLiteral("ACTIVE"))).object();
        body.insert(QStringLiteral("warm_return"), true);
        fake->statuses = { 200 };
        fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        // Attempt 1: the rig does not confirm the held pin. The warm attempt fails, the PIN
        // handshake runs once after it, and that is what pairs.
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("pin-identity"));
        QCOMPARE(warmChecks.load(), 1);
        QCOMPARE(handshakes.load(), 1);
        QCOMPARE(heldSeenByTheCheck, firstPin);
        QCOMPARE(failed.count(), 0);

        // Attempt 2 of the same session (the next reconnect try): the rig now confirms the pin the
        // PIN pairing left held. The PIN handshake does not run again.
        warmCheckOk = true;
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 5000);
        QCOMPARE(completed.at(1).at(0).toString(), QStringLiteral("warm-identity"));
        QCOMPARE(warmChecks.load(), 2);
        QCOMPARE(handshakes.load(), 1);
        QCOMPARE(heldSeenByTheCheck, secondPin);
        QCOMPARE(failed.count(), 0);

        // And never two at once, warm check and PIN handshake together.
        QCOMPARE(mostAtOnce.load(), 1);

        // The seam says which kind of pairing the last result was, so the caller can drop the held
        // trust when a warm attempt's stream then fails.
        QVERIFY(seam.lastResult().warm);
    }

    void aWarmResultIsMarkedWarmAndAPinResultIsNot()
    {
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        HeldRigTrust::clear();
        HeldRigTrust::remember(QByteArrayLiteral("held"));

        ProductionPairingSeam seam;
        std::atomic<bool> warmOk{true};
        seam.setHandshake([&](const PairingTarget&) {
            PairingHandshakeResult result = handshakeResult(true, QStringLiteral("pin-identity"));
            result.host = std::make_shared<PairedHost>();
            return result;
        });
        seam.setWarmReturnCheck([&](const PairingTarget&, const QByteArray&) {
            PairingHandshakeResult result;
            result.ok = warmOk.load();
            result.clientIdentity = result.ok ? QStringLiteral("warm-identity") : QString();
            if (result.ok) {
                result.host = std::make_shared<PairedHost>();
            }
            return result;
        });

        QVector<bool> warmFlags;
        QObject::connect(&seam, &ProductionPairingSeam::handshakeClassified,
                         [&](const PairingHandshakeResult& result) { warmFlags.append(result.warm); });

        PairingTarget target = pairingTarget();
        target.warmReturn = true;
        SeamReport report;
        seam.pair(target, recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QCOMPARE(report.identities.first(), QStringLiteral("warm-identity"));

        warmOk = false;
        seam.pair(target, recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 2, 5000);
        QCOMPARE(report.identities.last(), QStringLiteral("pin-identity"));

        QCOMPARE(warmFlags, (QVector<bool>{ true, false }));
    }

    void heldRigTrustIsReplacedByTheNextPairingAndClearedOnDemand()
    {
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        HeldRigTrust::clear();
        QVERIFY(HeldRigTrust::held().isEmpty());

        HeldRigTrust::remember(QByteArrayLiteral("first"));
        QCOMPARE(HeldRigTrust::held(), QByteArrayLiteral("first"));
        HeldRigTrust::remember(QByteArrayLiteral("second"));
        QCOMPARE(HeldRigTrust::held(), QByteArrayLiteral("second"));
        HeldRigTrust::remember(QByteArray());
        QVERIFY2(HeldRigTrust::held().isEmpty(), "an empty pin forgets what was held");

        HeldRigTrust::remember(QByteArrayLiteral("third"));
        HeldRigTrust::clear();
        QVERIFY(HeldRigTrust::held().isEmpty());
    }

    void noTelemetryEmitterAcceptsFreeText()
    {
        using StepFailureSignal =
            void (PairingController::*)(const QString&, const QString&, qint64);
        static_assert(std::is_same_v<decltype(&PairingController::stepFailed), StepFailureSignal>,
                      "pairing diagnostics may carry only closed tokens and elapsed time");

        const QString clientPath = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("../app/seathub/seathub_client.cpp"));
        const QString headerPath = QDir(QCoreApplication::applicationDirPath())
                                       .filePath(QStringLiteral("../app/seathub/seathub_client.h"));
        QFile headerFile(headerPath);
        QVERIFY2(headerFile.open(QIODevice::ReadOnly), qPrintable(headerPath));
        const QString headerSource = QString::fromUtf8(headerFile.readAll());
        const QRegularExpression helperSignature(
            QStringLiteral("void\\s+noteStepOutcome\\s*\\(([^)]*)\\)\\s*;"));
        const QRegularExpressionMatch helperMatch = helperSignature.match(headerSource);
        QVERIFY2(helperMatch.hasMatch(), "noteStepOutcome declaration was not found");
        QString parameterTypes = helperMatch.captured(1);
        parameterTypes.remove(QRegularExpression(QStringLiteral("\\s+")));
        QCOMPARE(parameterTypes,
                 QStringLiteral("constQString&step,constQString&outcome,constQString&failureClass,"
                                "qint64elapsedMs,intattempt=0,constQString&endReason={}"));

        QFile clientFile(clientPath);
        QVERIFY2(clientFile.open(QIODevice::ReadOnly), qPrintable(clientPath));
        const QString clientSource = QString::fromUtf8(clientFile.readAll());
        QVERIFY(QRegularExpression(
                    QStringLiteral("isClosedFailureClass\\s*\\(\\s*closedStep\\s*,\\s*failureClass\\s*\\)"))
                    .match(clientSource).hasMatch());

        const QRegularExpression callStart(QStringLiteral("\\bnoteStepOutcome\\s*\\("));
        auto match = callStart.globalMatch(clientSource);
        int checkedCalls = 0;
        const QRegularExpression forbidden(QStringLiteral("\\b(engineError|diagnostic|errorString)\\b"));
        while (match.hasNext()) {
            const QRegularExpressionMatch current = match.next();
            int open = clientSource.indexOf(QLatin1Char('('), current.capturedStart());
            int depth = 1;
            int close = open + 1;
            for (; close < clientSource.size() && depth > 0; ++close) {
                if (clientSource.at(close) == QLatin1Char('(')) {
                    ++depth;
                } else if (clientSource.at(close) == QLatin1Char(')')) {
                    --depth;
                }
            }
            QVERIFY(depth == 0);
            int after = close;
            while (after < clientSource.size() && clientSource.at(after).isSpace()) {
                ++after;
            }
            if (after < clientSource.size() && clientSource.at(after) == QLatin1Char('{')) {
                continue; // the helper definition, not an emitter call
            }
            const QString arguments = clientSource.mid(open + 1, close - open - 2);
            QVERIFY2(!forbidden.match(arguments).hasMatch(),
                     "a play-step emitter call must never receive raw engine or failure text");
            ++checkedCalls;
        }
        QVERIFY(checkedCalls > 0);
    }

    void thePairingDeadlineFiresOnItsOwnTimer()
    {
        // V9 (ADR-0066 decision 9, ADR-0083 decision 8): the 30 s pairing deadline keeps its own
        // timer. It must expire even when the request it is waiting on never answers - it is not a
        // check that only runs when a poll tick returns.
        PairingController controller;
        auto* fake = new StallingNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        // A poll interval far longer than the deadline: no second poll tick can arrive first.
        controller.setPollIntervalMs(60000);
        controller.setDeadlineMs(60);

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        // The first poll went out and stalled; nothing else can happen unless the timer fires.
        QVERIFY(fake->calls >= 1);
        QCOMPARE(failed.count(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
        QCOMPARE(controller.state(), QStringLiteral("failed"));
        QCOMPARE(seam->calls, 0);
    }

    // -----------------------------------------------------------------------------------------
    // SC18, ADR-0098: Client pairing instrumentation.
    // -----------------------------------------------------------------------------------------

    void pairingInstrumentation_startAndSuccessResultShapesAndOmissions()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult res = handshakeResult(true, QStringLiteral("fingerprint-sha256-hex"));
            res.host = std::make_shared<PairedHost>();
            res.attempts = 1;
            res.attemptStep = QStringLiteral("pair_handshake");
            res.stepClass = QStringLiteral("ok");
            return res;
        });

        PairingTarget target = pairingTarget();
        const QString testSessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000001");
        target.sessionId = testSessionId;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);

        const QVector<ShippedLine> lines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(lines.size(), 2);

        // Stage: start
        const ShippedLine& startLine = lines.at(0);
        QCOMPARE(startLine.level, LogLevel::Info);
        QCOMPARE(startLine.body,
                 QStringLiteral("pairing_instrumentation writer=client stage=start session_id=%1 client_attempt=1")
                     .arg(testSessionId));

        // Stage: result
        const ShippedLine& resultLine = lines.at(1);
        QCOMPARE(resultLine.level, LogLevel::Info);
        QCOMPARE(withMsNormalized(resultLine.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=success warm=false attempts=1 engine_step=pair_handshake engine_class=ok first_outcome=success retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(testSessionId));

        // Strict omission of server-only fields and secrets across all emitted lines
        const QStringList serverOnlyFields = {
            QStringLiteral("pending_pairings_at_submit"),
            QStringLiteral("pairing_id"),
            QStringLiteral("pin_ack_session"),
            QStringLiteral("pin_ack_age_ms"),
            QStringLiteral("since_sunshine_start_ms"),
            QStringLiteral("pin_salt_match")
        };

        for (const ShippedLine& line : lines) {
            for (const QString& field : serverOnlyFields) {
                QVERIFY2(!line.body.contains(field), qPrintable(field));
            }
            QVERIFY(!line.body.contains(QString::fromLatin1(kPin)));
            QVERIFY(!line.body.contains(QStringLiteral("salt")));
            QVERIFY(!line.body.contains(QStringLiteral("hash")));
        }
    }

    void pairingInstrumentation_failureAndPinRejected()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        const QString secretDiagnostic = QStringLiteral("SECRET_INTERNAL_ENGINE_ERROR_STATE");

        ProductionPairingSeam seam;
        seam.setHandshake([&secretDiagnostic](const PairingTarget&) {
            PairingHandshakeResult res = handshakeResult(false, QString(), secretDiagnostic);
            res.attempts = 2;
            res.attemptStep = QStringLiteral("pair_handshake");
            res.stepClass = QStringLiteral("pin_rejected");
            return res;
        });

        PairingTarget target = pairingTarget();
        const QString testSessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000002");
        target.sessionId = testSessionId;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QCOMPARE(report.outcomes.first(), false);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);

        const QVector<ShippedLine> lines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(lines.size(), 2);

        const ShippedLine& resultLine = lines.at(1);
        QCOMPARE(resultLine.level, LogLevel::Info);
        // The fake fails every run, so the Play's one retry happened (SC18): attempts=2, the
        // first failure stays on the record beside the final one, and the PIN is only a shape.
        QCOMPARE(withMsNormalized(resultLine.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=failure warm=false attempts=2 engine_step=pair_handshake engine_class=pin_rejected first_outcome=failure first_engine_step=pair_handshake first_engine_class=pin_rejected retried=true retry_reason=failure pin_shape_ok=true handshake_ms=N")
                     .arg(testSessionId));
        QVERIFY(!resultLine.body.contains(secretDiagnostic));
    }

    void pairingInstrumentation_warmSuccess()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult res = handshakeResult(true, QStringLiteral("warm-fingerprint"));
            res.host = std::make_shared<PairedHost>();
            res.attempts = 1;
            res.warm = true;
            return res;
        });

        PairingTarget target = pairingTarget();
        const QString testSessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000003");
        target.sessionId = testSessionId;
        target.warmReturn = true;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QCOMPARE(report.outcomes.first(), true);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);

        const QVector<ShippedLine> lines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(lines.size(), 2);

        const ShippedLine& resultLine = lines.at(1);
        QCOMPARE(resultLine.level, LogLevel::Info);
        QCOMPARE(withMsNormalized(resultLine.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=success warm=true attempts=1 first_outcome=success retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(testSessionId));
    }

    void pairingInstrumentation_emptyIdentityFailsClosedAsFailure()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult res = handshakeResult(true, QString());
            res.attempts = 1;
            return res;
        });

        PairingTarget target = pairingTarget();
        const QString testSessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000004");
        target.sessionId = testSessionId;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QCOMPARE(report.outcomes.first(), false);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);

        const QVector<ShippedLine> lines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(lines.size(), 2);

        const ShippedLine& resultLine = lines.at(1);
        QCOMPARE(resultLine.level, LogLevel::Info);
        // `ok` without an identity is a failure, but not a non-PAIRED result: nothing to retry.
        QCOMPARE(withMsNormalized(resultLine.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=failure warm=false attempts=1 first_outcome=failure retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(testSessionId));
    }

    void pairingInstrumentation_unclassifiedTimeoutFailsClosed()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        ProductionPairingSeam seam;
        seam.setDeadlineMs(20);
        seam.setHandshake([](const PairingTarget&) {
            QThread::msleep(200);
            return handshakeResult(true, QStringLiteral("too-late"));
        });

        PairingTarget target = pairingTarget();
        const QString testSessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000005");
        target.sessionId = testSessionId;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QCOMPARE(report.outcomes.first(), false);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);

        const QVector<ShippedLine> lines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(lines.size(), 2);

        const ShippedLine& resultLine = lines.at(1);
        QCOMPARE(resultLine.level, LogLevel::Info);
        QCOMPARE(withMsNormalized(resultLine.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=failure warm=false attempts=1 first_outcome=failure retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(testSessionId));
        QVERIFY(!resultLine.body.contains(QStringLiteral("engine_step=")));
        QVERIFY(!resultLine.body.contains(QStringLiteral("engine_class=")));
        // The deadline spoke before the worker answered: the timing it reports is the deadline's.
        QVERIFY(resultLine.body.contains(QRegularExpression(QStringLiteral("handshake_ms=\\d+$"))));
    }

    void pairingInstrumentation_cancelledAndSupersededStartsLackResult()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        // --- 1. Direct cancellation via clearPendingPairing while worker is held ---
        {
            QSemaphore blockCancel;
            const auto cancelGuard = qScopeGuard([&blockCancel]() { blockCancel.release(10); });

            ProductionPairingSeam seamCancel;
            std::atomic<int> cancelsCalled{0};
            seamCancel.setCancelRequest([&cancelsCalled](const QString&, int) {
                ++cancelsCalled;
                return true;
            });
            seamCancel.setHandshake([&blockCancel](const PairingTarget& target) {
                if (target.sessionId == QLatin1String("session-cancel")) {
                    blockCancel.acquire();
                    return handshakeResult(false, QString(), QStringLiteral("cancelled-late-result"));
                }
                return handshakeResult(true, QStringLiteral("identity-subsequent"));
            });

            PairingTarget targetCancel = pairingTarget();
            targetCancel.sessionId = QStringLiteral("session-cancel");
            SeamReport reportCancel;

            seamCancel.pair(targetCancel, recordInto(&reportCancel));
            QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=start session_id=session-cancel")), 1, 5000);

            // While worker is held in blockCancel, cancel the pending pairing:
            seamCancel.clearPendingPairing(targetCancel);

            // Release the held worker:
            blockCancel.release();
            QTest::qWait(100);

            // Assert honest unmatched start: no result, no callback:
            QCOMPARE(reportCancel.count(), 0);
            QCOMPARE(stub.countMatching(QStringLiteral("stage=result session_id=session-cancel")), 0);
            QCOMPARE(stub.countMatching(QStringLiteral("stage=result")), 0);

            // Now start a subsequent attempt and prove no record from the cancelled worker was attributed to later generation:
            PairingTarget targetSubsequent = pairingTarget();
            targetSubsequent.sessionId = QStringLiteral("session-subsequent");
            SeamReport reportSubsequent;

            seamCancel.pair(targetSubsequent, recordInto(&reportSubsequent));
            QTRY_COMPARE_WITH_TIMEOUT(reportSubsequent.count(), 1, 5000);
            QCOMPARE(reportSubsequent.outcomes.first(), true);
            QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=result session_id=session-subsequent")), 1, 5000);

            const QVector<ShippedLine> cancelSuiteLines = stub.matching(QStringLiteral("pairing_instrumentation"));
            // We expect:
            // 1) start session-cancel client_attempt=1
            // 2) start session-subsequent client_attempt=3 (clearPendingPairing incremented to 2, pair() incremented to 3)
            // 3) result session-subsequent client_attempt=3 outcome=success
            QCOMPARE(cancelSuiteLines.size(), 3);
            QCOMPARE(cancelSuiteLines.at(0).body, QStringLiteral("pairing_instrumentation writer=client stage=start session_id=session-cancel client_attempt=1"));
            QCOMPARE(cancelSuiteLines.at(1).body, QStringLiteral("pairing_instrumentation writer=client stage=start session_id=session-subsequent client_attempt=3"));
            QCOMPARE(withMsNormalized(cancelSuiteLines.at(2).body), QStringLiteral("pairing_instrumentation writer=client stage=result session_id=session-subsequent client_attempt=3 outcome=success warm=false attempts=1 first_outcome=success retried=false pin_shape_ok=true handshake_ms=N"));
            QCOMPARE(stub.countMatching(QStringLiteral("session-cancel")), 1);
        }

        // --- 2. Superseded start (targetA superseded by targetB while targetA is held in worker) ---
        {
            QSemaphore blockA;
            const auto cleanupGuard = qScopeGuard([&blockA]() { blockA.release(10); });

            ProductionPairingSeam seam;
            seam.setHandshake([&blockA](const PairingTarget& target) {
                if (target.sessionId == QLatin1String("session-a")) {
                    blockA.acquire();
                    return handshakeResult(false, QString(), QStringLiteral("stale-a"));
                }
                return handshakeResult(true, QStringLiteral("identity-b"));
            });

            PairingTarget targetA = pairingTarget();
            targetA.sessionId = QStringLiteral("session-a");
            PairingTarget targetB = pairingTarget();
            targetB.sessionId = QStringLiteral("session-b");

            SeamReport reportA;
            SeamReport reportB;

            seam.pair(targetA, recordInto(&reportA));
            QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=start session_id=session-a")), 1, 5000);

            seam.pair(targetB, recordInto(&reportB));
            QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=start session_id=session-b")), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=result session_id=session-b")), 1, 5000);
            QCOMPARE(reportB.count(), 1);
            QCOMPARE(reportB.outcomes.first(), true);

            blockA.release();
            QTest::qWait(100);

            QCOMPARE(reportA.count(), 0);
            QCOMPARE(stub.countMatching(QStringLiteral("stage=result session_id=session-a")), 0);
            QCOMPARE(stub.countMatching(QStringLiteral("stage=result session_id=session-b")), 1);

            const QVector<ShippedLine> bLines = stub.matching(QStringLiteral("session-b"));
            QCOMPARE(bLines.size(), 2);
            QCOMPARE(bLines.at(0).body, QStringLiteral("pairing_instrumentation writer=client stage=start session_id=session-b client_attempt=2"));
            QCOMPARE(withMsNormalized(bLines.at(1).body), QStringLiteral("pairing_instrumentation writer=client stage=result session_id=session-b client_attempt=2 outcome=success warm=false attempts=1 first_outcome=success retried=false pin_shape_ok=true handshake_ms=N"));
        }
    }

    void pairingInstrumentation_metadataSwitchBeforeDrainAndAmbientPublication()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        // Hold captured lines in queue/spool initially
        LogShipper::instance().setCanShip(false);

        const QString sessionOld = QStringLiteral("c0a80101-0000-4000-8000-000000000010");
        const QString sessionNew = QStringLiteral("c0a80101-0000-4000-8000-000000000020");
        const QString hostNew = QStringLiteral("host-rig-20");
        const QString traceOld = QStringLiteral("trace-10");
        const QString traceNew = QStringLiteral("trace-20");

        LogShipper::instance().publishIds(sessionOld, QString(), traceOld);

        QSemaphore inHandshakeOld;
        QSemaphore releaseHandshakeOld;
        const auto cleanupGuard = qScopeGuard([&]() { releaseHandshakeOld.release(10); });

        ProductionPairingSeam seam;
        seam.setHandshake([&](const PairingTarget& target) {
            if (target.sessionId == sessionOld) {
                inHandshakeOld.release();
                releaseHandshakeOld.acquire();
                return handshakeResult(true, QStringLiteral("identity-old"));
            }
            return handshakeResult(true, QStringLiteral("identity-new"));
        });

        PairingTarget targetOld = pairingTarget();
        targetOld.sessionId = sessionOld;

        SeamReport reportOld;
        seam.pair(targetOld, recordInto(&reportOld));

        // Wait until handshake worker starts executing (meaning pair() has emitted stage=start)
        inHandshakeOld.acquire();

        // Observable verification that start line has been spooled or enqueued while canShip is false
        QTRY_VERIFY_WITH_TIMEOUT(LogShipper::instance().backlogSpoolLines() >= 1 || QFileInfo::exists(ownSpoolFilePath(spoolDir.path())), 5000);
        // Assert no instrumentation handed off yet
        QCOMPARE(stub.countMatching(QStringLiteral("pairing_instrumentation")), 0);

        // Switch published identity BEFORE queue/spool drain:
        LogShipper::instance().publishIds(sessionNew, hostNew, traceNew);

        // Now drain the spool:
        LogShipper::instance().setCanShip(true);

        // Assert old start was drained and still retains its capture-time envelope (sessionOld, traceOld, absent host) and explicit body
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=start session_id=%1").arg(sessionOld)), 1, 5000);
        const QVector<ShippedLine> startLines = stub.matching(QStringLiteral("stage=start session_id=%1").arg(sessionOld));
        QCOMPARE(startLines.size(), 1);

        const ShippedLine& oldStart = startLines.first();
        QCOMPARE(oldStart.sessionId, sessionOld);
        QCOMPARE(oldStart.traceId, traceOld);
        QVERIFY(oldStart.hostId.isEmpty());
        QVERIFY(oldStart.body.contains(QStringLiteral("session_id=%1").arg(sessionOld)));
        QVERIFY(!oldStart.body.contains(QStringLiteral("session_id=%1").arg(sessionNew)));

        // Release old handshake: its stage=result is emitted NOW, after ambient identity changed to sessionNew
        releaseHandshakeOld.release();
        QTRY_COMPARE_WITH_TIMEOUT(reportOld.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("stage=result session_id=%1").arg(sessionOld)), 1, 5000);

        const QVector<ShippedLine> oldResultLines = stub.matching(QStringLiteral("stage=result session_id=%1").arg(sessionOld));
        QCOMPARE(oldResultLines.size(), 1);

        const ShippedLine& oldResult = oldResultLines.first();
        // Envelope reflects ambient published IDs at result emission time:
        QCOMPARE(oldResult.sessionId, sessionNew);
        QCOMPARE(oldResult.hostId, hostNew);
        QCOMPARE(oldResult.traceId, traceNew);
        // But explicit body is NOT relabeled - retains original target sessionOld:
        QVERIFY(oldResult.body.contains(QStringLiteral("session_id=%1").arg(sessionOld)));
        QVERIFY(!oldResult.body.contains(QStringLiteral("session_id=%1").arg(sessionNew)));
        QCOMPARE(withMsNormalized(oldResult.body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=1 outcome=success warm=false attempts=1 first_outcome=success retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(sessionOld));

        // Now replacement seam pair with targetNew:
        PairingTarget targetNew = pairingTarget();
        targetNew.sessionId = sessionNew;
        SeamReport reportNew;
        seam.pair(targetNew, recordInto(&reportNew));

        QTRY_COMPARE_WITH_TIMEOUT(reportNew.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("session_id=%1").arg(sessionNew)), 2, 5000);

        const QVector<ShippedLine> newLines = stub.matching(QStringLiteral("session_id=%1").arg(sessionNew));
        QCOMPARE(newLines.size(), 2);
        QCOMPARE(newLines.at(0).sessionId, sessionNew);
        QCOMPARE(newLines.at(0).hostId, hostNew);
        QCOMPARE(newLines.at(0).traceId, traceNew);
        QCOMPARE(newLines.at(0).body,
                 QStringLiteral("pairing_instrumentation writer=client stage=start session_id=%1 client_attempt=2")
                     .arg(sessionNew));
        QCOMPARE(newLines.at(1).sessionId, sessionNew);
        QCOMPARE(newLines.at(1).hostId, hostNew);
        QCOMPARE(newLines.at(1).traceId, traceNew);
        QCOMPARE(withMsNormalized(newLines.at(1).body),
                 QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=2 outcome=success warm=false attempts=1 first_outcome=success retried=false pin_shape_ok=true handshake_ms=N")
                     .arg(sessionNew));
    }

    void pairingInstrumentation_secretSentinelsAbsentBeforeScrubAndAfterHandoff()
    {
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        RawTeeCapture rawCapture;

        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&stub](const ShippedLine& l) { return stub(l); });
        LogShipper::instance().setCanShip(true);

        const QString secretPin = QStringLiteral("SECRET_PIN_9876");
        const QString secretDiagnostic = QStringLiteral("SECRET_RAW_ENGINE_DIAGNOSTIC_CONTAINING_KEY_AND_TARGET_DUMP");
        const QString secretTargetCredential = QStringLiteral("SECRET_TARGET_CREDENTIAL_DATA");

        ProductionPairingSeam seam;
        seam.setHandshake([&secretDiagnostic](const PairingTarget&) {
            PairingHandshakeResult res = handshakeResult(false, QString(), secretDiagnostic);
            res.attempts = 1;
            res.attemptStep = QStringLiteral("pair_handshake");
            res.stepClass = QStringLiteral("pin_rejected");
            return res;
        });

        PairingTarget target = pairingTarget();
        target.sessionId = QStringLiteral("c0a80101-0000-4000-8000-000000000099");
        target.pairingPin = secretPin;

        SeamReport report;
        seam.pair(target, recordInto(&report));

        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("pairing_instrumentation")), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(rawCapture.count(), 2, 5000);

        const QStringList rawLines = rawCapture.lines();
        QCOMPARE(rawLines.size(), 2);
        for (const QString& line : rawLines) {
            QVERIFY2(!line.contains(secretPin), "PIN must never appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(secretDiagnostic), "raw engine error must never appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(secretTargetCredential), "target credential must never appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pin=")), "PIN parameter must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("salt")), "salt must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("hash")), "hash must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pending_pairings_at_submit")), "server field must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pairing_id")), "server field must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pin_ack_session")), "server field must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pin_ack_age_ms")), "server field must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("since_sunshine_start_ms")), "server field must not appear in raw pairing_instrumentation");
            QVERIFY2(!line.contains(QStringLiteral("pin_salt_match")), "server field must not appear in raw pairing_instrumentation");
        }

        const QVector<ShippedLine> handedOffLines = stub.matching(QStringLiteral("pairing_instrumentation"));
        QCOMPARE(handedOffLines.size(), 2);
        for (const ShippedLine& line : handedOffLines) {
            QVERIFY2(!line.body.contains(secretPin), "PIN must never appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(secretDiagnostic), "raw engine error must never appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(secretTargetCredential), "target credential must never appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pin=")), "PIN parameter must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("salt")), "salt must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("hash")), "hash must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pending_pairings_at_submit")), "server field must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pairing_id")), "server field must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pin_ack_session")), "server field must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pin_ack_age_ms")), "server field must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("since_sunshine_start_ms")), "server field must not appear in handed-off pairing_instrumentation");
            QVERIFY2(!line.body.contains(QStringLiteral("pin_salt_match")), "server field must not appear in handed-off pairing_instrumentation");
        }
    }
    // -----------------------------------------------------------------------------------------
    // SC18, the ONE logged pairing retry. A Play whose first pairing attempt is not PAIRED gets
    // exactly one more, drawn from the same budget as the 409 clear, and the first failure stays
    // on the record beside the final result. Every wait below is on the worker's own effect
    // (`waitForDone` on the pool the seam runs it on), never a sleep.
    // -----------------------------------------------------------------------------------------

    void sc18_failureThenSuccess_retriesOnceAndKeepsTheFirstClass()
    {
        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        QVector<int> handshakePorts;
        QVector<int> cancelPorts;
        QMutex lock;
        seam.setHandshake([&](const PairingTarget& target) {
            {
                QMutexLocker guard(&lock);
                handshakePorts.append(target.controlPort);
            }
            if (++handshakes == 1) {
                PairingHandshakeResult rejected;
                rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
                rejected.attemptStep = QStringLiteral("pair_handshake");
                rejected.stepClass = QStringLiteral("pin_rejected");
                return rejected;
            }
            return handshakeResult(true, QStringLiteral("retry-identity"));
        });
        seam.setCancelRequest([&](const QString&, int port) {
            QMutexLocker guard(&lock);
            cancelPorts.append(port);
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        QCOMPARE(handshakes.load(), 2);
        // The only rig write the retry adds is the existing pending-slot clear, and both it and
        // the second handshake aim at the signed per-rig control port.
        QCOMPARE(cancelPorts, (QVector<int>{ 20000 }));
        QCOMPARE(handshakePorts, (QVector<int>{ 20000, 20000 }));
        QVERIFY(report.outcomes.first());
        QCOMPARE(report.identities.first(), QStringLiteral("retry-identity"));

        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY(delivered.ok);
        QCOMPARE(delivered.attempts, 2);
        // Not relabelled `in_progress`: the record says what went wrong first.
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstAttemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("failure"));
        QVERIFY(delivered.pinShapeOk);
        QVERIFY(delivered.handshakeMs >= 0);
        // The PIN never rides any of the new fields.
        const QString pin = QString::fromLatin1(kPin);
        QVERIFY(!delivered.firstOutcome.contains(pin) && !delivered.firstAttemptStep.contains(pin)
                && !delivered.firstStepClass.contains(pin) && !delivered.retryReason.contains(pin));
    }

    void sc18_conflictThenSuccess_keepsInProgressAndNamesTheConflict()
    {
        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        seam.setHandshake([&](const PairingTarget&) {
            if (++handshakes == 1) {
                PairingHandshakeResult conflict = handshakeResult(false, QString(), QStringLiteral("conflict"));
                conflict.pairingConflict = true;
                conflict.attemptStep = QStringLiteral("pair_server_info");
                conflict.stepClass = QStringLiteral("http_4xx");
                return conflict;
            }
            return handshakeResult(true, QStringLiteral("recovered"));
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        const PairingHandshakeResult delivered = seam.lastResult();
        QVERIFY(delivered.ok);
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("in_progress"));
        QCOMPARE(delivered.attempts, 2);
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstAttemptStep, QStringLiteral("pair_server_info"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("http_4xx"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("conflict"));
    }

    void sc18_conflictThenFailure_doesNotRetryASecondTime()
    {
        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        std::atomic<int> cancels{0};
        seam.setHandshake([&](const PairingTarget&) {
            const int n = ++handshakes;
            PairingHandshakeResult failed;
            failed.attemptStep = QStringLiteral("pair_handshake");
            failed.engineError = QStringLiteral("run %1").arg(n);
            if (n == 1) {
                failed.pairingConflict = true;
                failed.stepClass = QStringLiteral("in_progress");
            } else {
                failed.stepClass = QStringLiteral("pin_rejected");
            }
            return failed;
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        // One budget for both reasons: a conflict clear followed by a failure is not a second retry.
        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(cancels.load(), 1);
        const PairingHandshakeResult delivered = seam.lastResult();
        QVERIFY(!delivered.ok);
        QCOMPARE(delivered.attempts, 2);
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("in_progress"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("conflict"));
    }

    void sc18_failureThenFailure_reportsTheFinalClassAndTheFirstOne()
    {
        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        std::atomic<int> cancels{0};
        seam.setHandshake([&](const PairingTarget&) {
            PairingHandshakeResult failed;
            failed.attemptStep = QStringLiteral("pair_handshake");
            if (++handshakes == 1) {
                failed.stepClass = QStringLiteral("pin_rejected");
            } else {
                failed.stepClass = QStringLiteral("timeout");
            }
            return failed;
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(cancels.load(), 1);
        const PairingHandshakeResult delivered = seam.lastResult();
        QVERIFY(!delivered.ok);
        QCOMPARE(delivered.attempts, 2);
        QCOMPARE(delivered.stepClass, QStringLiteral("timeout"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.retryReason, QStringLiteral("failure"));
    }

    void sc18_aReconnectingTargetNeverRetries()
    {
        for (const bool conflict : { false, true }) {
            ProductionPairingSeam seam;
            std::atomic<int> handshakes{0};
            std::atomic<int> cancels{0};
            seam.setHandshake([&](const PairingTarget&) {
                ++handshakes;
                PairingHandshakeResult failed;
                failed.attemptStep = QStringLiteral("pair_handshake");
                failed.stepClass = QStringLiteral("pin_rejected");
                failed.pairingConflict = conflict;
                return failed;
            });
            seam.setCancelRequest([&](const QString&, int) {
                ++cancels;
                return true;
            });

            PairingTarget target = pairingTarget();
            target.reconnecting = true;
            SeamReport report;
            seam.pair(target, recordInto(&report));
            QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
            QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

            QCOMPARE(handshakes.load(), 1);
            QCOMPARE(cancels.load(), 1);
            const PairingHandshakeResult delivered = seam.lastResult();
            QVERIFY(!delivered.ok);
            QCOMPARE(delivered.attempts, 1);
            QVERIFY(!delivered.retried);
            QVERIFY(delivered.retryReason.isEmpty());
            QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
            QCOMPARE(delivered.firstStepClass, QStringLiteral("pin_rejected"));
        }
    }

    void sc18_aWarmReturnNewTargetWhoseWarmCheckFailsStillRetriesOnce()
    {
        // `warmReturn` is true for a NEW session on a free return hold too, so it is never the
        // reconnect guard: this target is NOT reconnecting and its PIN handshake gets its retry.
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        HeldRigTrust::clear();
        HeldRigTrust::remember(QByteArrayLiteral("held"));

        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        std::atomic<int> warmChecks{0};
        std::atomic<int> cancels{0};
        seam.setWarmReturnCheck([&](const PairingTarget&, const QByteArray&) {
            ++warmChecks;
            return PairingHandshakeResult(); // not ok: the rig did not confirm the held pin
        });
        seam.setHandshake([&](const PairingTarget&) {
            if (++handshakes == 1) {
                PairingHandshakeResult failed;
                failed.attemptStep = QStringLiteral("pair_handshake");
                failed.stepClass = QStringLiteral("pin_rejected");
                return failed;
            }
            return handshakeResult(true, QStringLiteral("pin-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        PairingTarget target = pairingTarget();
        target.warmReturn = true;
        QVERIFY(!target.reconnecting);
        SeamReport report;
        seam.pair(target, recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        QCOMPARE(warmChecks.load(), 1);
        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(cancels.load(), 1);
        QCOMPARE(report.identities.first(), QStringLiteral("pin-identity"));
        const PairingHandshakeResult delivered = seam.lastResult();
        QVERIFY(delivered.ok);
        QVERIFY(!delivered.warm);
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("failure"));
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
    }

    void sc18_aWarmSuccessNeverRetriesAndCountsAsAFirstSuccess()
    {
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        HeldRigTrust::clear();
        HeldRigTrust::remember(QByteArrayLiteral("held"));

        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        std::atomic<int> cancels{0};
        seam.setWarmReturnCheck([](const PairingTarget&, const QByteArray&) {
            PairingHandshakeResult result = handshakeResult(true, QStringLiteral("warm-identity"));
            result.host = std::make_shared<PairedHost>();
            return result;
        });
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            return handshakeResult(false, QString(), QStringLiteral("must not run"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        PairingTarget target = pairingTarget();
        target.warmReturn = true;
        SeamReport report;
        seam.pair(target, recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        QCOMPARE(handshakes.load(), 0);
        QCOMPARE(cancels.load(), 0);
        const PairingHandshakeResult delivered = seam.lastResult();
        QVERIFY(delivered.warm);
        QCOMPARE(delivered.attempts, 1);
        QCOMPARE(delivered.firstOutcome, QStringLiteral("success"));
        QVERIFY(delivered.firstAttemptStep.isEmpty());
        QVERIFY(delivered.firstStepClass.isEmpty());
        QVERIFY(!delivered.retried);
        QVERIFY(delivered.retryReason.isEmpty());
    }

    void sc18_noRetryOncePastTheDeadline()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(20);
        QSemaphore release;
        std::atomic<int> handshakes{0};
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            release.acquire();
            PairingHandshakeResult failed;
            failed.attemptStep = QStringLiteral("pair_handshake");
            failed.stepClass = QStringLiteral("pin_rejected");
            return failed;
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);

        // The deadline has spoken; wait (on a condition, not a bare sleep) until the worker's
        // clock copy is certain to read past it, since a coarse timer can fire a little early.
        QElapsedTimer pastDeadline;
        pastDeadline.start();
        QTRY_VERIFY_WITH_TIMEOUT(pastDeadline.elapsed() >= 60, 2000);
        release.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));

        QCOMPARE(handshakes.load(), 1);
        QCOMPARE(report.count(), 1);
    }

    // -----------------------------------------------------------------------------------------
    // T074 (review finding F3): the first attempt and the retry decision are published to the
    // generation's shared progress BEFORE the retry blocks, so a deadline (or a later generation)
    // that speaks for a blocked retry still reports them. Every wait is on the effect itself: the
    // semaphore the retry handshake releases the moment it is entered, then the delivered result.
    // The deadline is long enough (1 s) that the first attempt and the retry decision always land
    // inside it, even on a loaded PC.
    // -----------------------------------------------------------------------------------------

    void firstFailureBlockedRetryDeadlineKeepsProgress()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(1000);
        QSemaphore retryEntered;
        QSemaphore retryGate;
        std::atomic<int> handshakes{0};
        std::atomic<int> cancels{0};
        seam.setHandshake([&](const PairingTarget&) {
            if (++handshakes == 1) {
                PairingHandshakeResult rejected;
                rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
                rejected.attemptStep = QStringLiteral("pair_handshake");
                rejected.stepClass = QStringLiteral("pin_rejected");
                return rejected;
            }
            retryEntered.release();
            retryGate.acquire();
            // A late, proper worker result: it must not overwrite the deadline's record.
            return handshakeResult(true, QStringLiteral("late-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        QSignalSpy resolved(&seam, &ProductionPairingSeam::hostResolved);
        QVector<PairingHandshakeResult> sunk;
        seam.setAttemptSink([&sunk](const PairingHandshakeResult& r) { sunk.append(r); });
        const auto release = qScopeGuard([&retryGate]() { retryGate.release(10); });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QVERIFY(retryEntered.tryAcquire(1, 5000));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 10000);

        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QCOMPARE(classified.count(), 1);
        QCOMPARE(sunk.size(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY(!delivered.ok);
        QVERIFY(delivered.engineError.contains(QStringLiteral("did not finish")));
        // The deadline spoke for the retry, which had not answered: no final class of its own...
        QVERIFY(delivered.attemptStep.isEmpty());
        QVERIFY(delivered.stepClass.isEmpty());
        // ...but the first failure and the actual retry stay on the record.
        QCOMPARE(delivered.attempts, 2);
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(delivered.firstAttemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(delivered.retried);
        QCOMPARE(delivered.retryReason, QStringLiteral("failure"));
        QVERIFY(delivered.pinShapeOk);
        QVERIFY(delivered.handshakeMs >= 0);
        QCOMPARE(sunk.first().firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(sunk.first().retried);
        QCOMPARE(seam.lastResult().firstStepClass, QStringLiteral("pin_rejected"));
        const QString pin = QString::fromLatin1(kPin);
        QVERIFY(!delivered.engineError.contains(pin) && !delivered.firstOutcome.contains(pin)
                && !delivered.firstAttemptStep.contains(pin) && !delivered.firstStepClass.contains(pin)
                && !delivered.retryReason.contains(pin));

        // The late worker finishes now: nothing new is reported, and it never retries again.
        retryGate.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(50);
        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(cancels.load(), 1);
        QCOMPARE(report.count(), 1);
        QCOMPARE(classified.count(), 1);
        QCOMPARE(sunk.size(), 1);
        QCOMPARE(resolved.count(), 0);
        QCOMPARE(seam.lastResult().firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(seam.lastResult().engineError.contains(QStringLiteral("did not finish")));
    }

    void firstFailureBlockedRetryCancelRetainsProgressExactlyOnce()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(1000);
        QSemaphore retryEntered;
        QSemaphore retryGate;
        std::atomic<int> handshakes{0};
        seam.setHandshake([&](const PairingTarget&) {
            if (++handshakes == 1) {
                PairingHandshakeResult rejected;
                rejected.attemptStep = QStringLiteral("pair_handshake");
                rejected.stepClass = QStringLiteral("pin_rejected");
                return rejected;
            }
            retryEntered.release();
            retryGate.acquire();
            return handshakeResult(true, QStringLiteral("cancelled-late-identity"));
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        QSignalSpy resolved(&seam, &ProductionPairingSeam::hostResolved);
        QVector<PairingHandshakeResult> sunk;
        seam.setAttemptSink([&sunk](const PairingHandshakeResult& r) { sunk.append(r); });
        const auto release = qScopeGuard([&retryGate]() { retryGate.release(10); });

        const PairingTarget target = pairingTarget();
        SeamReport report;
        seam.pair(target, recordInto(&report));
        QVERIFY(retryEntered.tryAcquire(1, 5000));

        // Cancelling abandons the generation: no completion, no classification, no resolution -
        // but the first failure and the retry the customer already caused stay on the record, once.
        seam.clearPendingPairing(target);
        QCOMPARE(sunk.size(), 1);
        const PairingHandshakeResult kept = sunk.first();
        QVERIFY(!kept.ok);
        QVERIFY(kept.clientIdentity.isEmpty());
        QVERIFY(!kept.host);
        QVERIFY(kept.engineError.isEmpty());
        QVERIFY(!kept.pairingConflict);
        QVERIFY(kept.attemptStep.isEmpty());
        QVERIFY(kept.stepClass.isEmpty());
        QCOMPARE(kept.attempts, 2);
        QCOMPARE(kept.firstOutcome, QStringLiteral("failure"));
        QCOMPARE(kept.firstAttemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(kept.firstStepClass, QStringLiteral("pin_rejected"));
        QVERIFY(kept.retried);
        QCOMPARE(kept.retryReason, QStringLiteral("failure"));
        QVERIFY(kept.pinShapeOk);
        QVERIFY(kept.handshakeMs >= 0);
        const QString pin = QString::fromLatin1(kPin);
        QVERIFY(!kept.firstOutcome.contains(pin) && !kept.firstAttemptStep.contains(pin)
                && !kept.firstStepClass.contains(pin) && !kept.retryReason.contains(pin));

        // A second cancel finds nothing pending; the late worker finishes and reports nothing.
        seam.clearPendingPairing(target);
        retryGate.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(50);
        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(report.count(), 0);
        QCOMPARE(classified.count(), 0);
        QCOMPARE(resolved.count(), 0);
        QCOMPARE(sunk.size(), 1);
        QVERIFY(seam.lastResult().firstStepClass.isEmpty());

        // The abandoned run's progress is gone: the next Play starts from a clean record.
        SeamReport next;
        seam.setHandshake([](const PairingTarget&) {
            return handshakeResult(true, QStringLiteral("next-identity"));
        });
        seam.pair(target, recordInto(&next));
        QTRY_COMPARE_WITH_TIMEOUT(next.count(), 1, 5000);
        QCOMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QCOMPARE(delivered.firstOutcome, QStringLiteral("success"));
        QVERIFY(delivered.firstStepClass.isEmpty());
        QVERIFY(!delivered.retried);
        QCOMPARE(delivered.attempts, 1);
        QCOMPARE(sunk.size(), 2);
        QCOMPARE(sunk.last().firstOutcome, QStringLiteral("success"));
        QVERIFY(!sunk.last().retried);
    }

    void cancelWithoutAFirstFailureRecordsNothing()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(1000);
        QSemaphore entered;
        QSemaphore gate;
        seam.setHandshake([&](const PairingTarget&) {
            entered.release();
            gate.acquire();
            return handshakeResult(true, QStringLiteral("cancelled-late-identity"));
        });
        seam.setCancelRequest([](const QString&, int) { return true; });
        int sinkCalls = 0;
        seam.setAttemptSink([&sinkCalls](const PairingHandshakeResult&) { ++sinkCalls; });
        const auto release = qScopeGuard([&gate]() { gate.release(10); });

        const PairingTarget target = pairingTarget();
        SeamReport report;
        seam.pair(target, recordInto(&report));
        QVERIFY(entered.tryAcquire(1, 5000));

        // The first attempt is still running: there is no failure to keep.
        seam.clearPendingPairing(target);
        gate.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(50);
        QCOMPARE(report.count(), 0);
        QCOMPARE(sinkCalls, 0);
    }

    void firstFailureBlockedRetryReplacedGenerationDoesNotLeakProgress()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(1000);
        QSemaphore retryEntered;
        QSemaphore retryGate;
        QSemaphore secondGate;
        std::atomic<int> firstSessionHandshakes{0};
        seam.setHandshake([&](const PairingTarget& target) {
            if (target.sessionId == QLatin1String("session-a")) {
                if (++firstSessionHandshakes == 1) {
                    PairingHandshakeResult rejected;
                    rejected.attemptStep = QStringLiteral("pair_handshake");
                    rejected.stepClass = QStringLiteral("pin_rejected");
                    return rejected;
                }
                retryEntered.release();
                retryGate.acquire();
                return handshakeResult(false, QString(), QStringLiteral("stale-a"));
            }
            // The newer generation's first attempt never answers inside its own deadline.
            secondGate.acquire();
            return handshakeResult(true, QStringLiteral("late-b"));
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        QSignalSpy classified(&seam, &ProductionPairingSeam::handshakeClassified);
        int sinkCalls = 0;
        seam.setAttemptSink([&sinkCalls](const PairingHandshakeResult&) { ++sinkCalls; });
        const auto release = qScopeGuard([&]() {
            retryGate.release(10);
            secondGate.release(10);
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");
        SeamReport reportA;
        SeamReport reportB;

        seam.pair(targetA, recordInto(&reportA));
        QVERIFY(retryEntered.tryAcquire(1, 5000));
        seam.pair(targetB, recordInto(&reportB));
        QTRY_COMPARE_WITH_TIMEOUT(reportB.count(), 1, 10000);

        // B's deadline speaks for B alone: A's first failure and retry are not B's.
        QCOMPARE(reportB.outcomes.first(), false);
        QCOMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QCOMPARE(delivered.attempts, 1);
        QCOMPARE(delivered.firstOutcome, QStringLiteral("failure"));
        QVERIFY(delivered.firstAttemptStep.isEmpty());
        QVERIFY(delivered.firstStepClass.isEmpty());
        QVERIFY(!delivered.retried);
        QVERIFY(delivered.retryReason.isEmpty());

        retryGate.release();
        secondGate.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(50);
        QCOMPARE(reportA.count(), 0);
        QCOMPARE(reportB.count(), 1);
        QCOMPARE(classified.count(), 1);
        QCOMPARE(sinkCalls, 1);
    }

    void sc18_theControllerMarksOnlyAnActiveSessionAsReconnecting()
    {
        struct Case { const char* name; const char* state; QJsonValue warm; bool present; bool reconnecting; bool warmReturn; };
        const QVector<Case> cases = {
            { "ACTIVE", "ACTIVE", QJsonValue(), false, true, false },
            { "ACTIVE and warm", "ACTIVE", QJsonValue(true), true, true, true },
            { "NEW", "NEW", QJsonValue(), false, false, false },
            { "NEW and warm", "NEW", QJsonValue(true), true, false, true },
            { "NEW and not warm", "NEW", QJsonValue(false), true, false, false },
        };
        for (const Case& c : cases) {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);

            QJsonObject body = QJsonDocument::fromJson(
                authorizationBody(QString::fromLatin1(kPin), QString::fromLatin1(c.state))).object();
            if (c.present) {
                body.insert(QStringLiteral("warm_return"), c.warm);
            }
            fake->statuses = { 200 };
            fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

            QSignalSpy completed(&controller, &PairingController::pairingCompleted);
            controller.start(QString::fromLatin1(kSessionId));
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);

            QCOMPARE(seam->calls, 1);
            QVERIFY2(seam->lastTarget.reconnecting == c.reconnecting, c.name);
            QVERIFY2(seam->lastTarget.warmReturn == c.warmReturn, c.name);
        }
    }

    // ADR-0109: the key record is written on the pair worker, once per real pair attempt, carries
    // that attempt's own session and attempt number, and carries no PIN, salt, digest or key.
    void pairingKeyContextIsAttemptScopedAndPrivate()
    {
        // Public synthetic vector: salt 00..04 and PIN 1234; the digest has a zero at index 9.
        const QByteArray saltedPin = QByteArray::fromHex("00000000000000000000000000000004") + "1234";
        const QByteArray digest = QCryptographicHash::hash(saltedPin, QCryptographicHash::Sha256);
        const QString secretHex = QString::fromLatin1(digest.toHex());
        const QString keyHex = QString::fromLatin1(digest.left(16).toHex());

        // No scope on this thread: the helper derives the key and writes no record.
        RawTeeCapture rawCapture;
        QCOMPARE(seathub::derivePairingKey(saltedPin, QCryptographicHash::Sha256).size(), 16);
        QCOMPARE(rawCapture.count(), 0);

        ProductionPairingSeam seam;
        std::atomic<int> handshakes{0};
        QVector<int> keyLengths;
        QMutex lock;
        seam.setHandshake([&](const PairingTarget&) {
            const QByteArray key = seathub::derivePairingKey(saltedPin, QCryptographicHash::Sha256);
            {
                QMutexLocker guard(&lock);
                keyLengths.append(static_cast<int>(key.size()));
            }
            if (++handshakes == 1) {
                PairingHandshakeResult rejected;
                rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
                rejected.attemptStep = QStringLiteral("pair_handshake");
                rejected.stepClass = QStringLiteral("pin_rejected");
                return rejected;
            }
            return handshakeResult(true, QStringLiteral("retry-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) { return true; });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE_WITH_TIMEOUT(report.count(), 1, 5000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QCOMPARE(handshakes.load(), 2);
        QCOMPARE(keyLengths, (QVector<int>{ 16, 16 }));

        QStringList keyLines;
        QStringList startLines;
        const QStringList all = rawCapture.lines();
        for (const QString& line : all) {
            if (line.contains(QLatin1String("stage=key_material"))) {
                keyLines.append(line);
            } else if (line.contains(QLatin1String("stage=start"))) {
                startLines.append(line);
            }
        }
        // One record per real pair attempt: the first try and the single retry, nothing for the
        // derivation made outside a pair attempt.
        QCOMPARE(keyLines.size(), 2);
        QCOMPARE(startLines.size(), 1);

        // Both records name the session and the same seam attempt number as the start record.
        QRegularExpression attemptPattern(QStringLiteral("client_attempt=(\\d+)"));
        const QRegularExpressionMatch startMatch = attemptPattern.match(startLines.first());
        QVERIFY(startMatch.hasMatch());
        const QString attempt = startMatch.captured(1);
        for (const QString& line : keyLines) {
            QCOMPARE(line,
                     QStringLiteral("pairing_instrumentation writer=client stage=key_material "
                                    "session_id=%1 client_attempt=%2 aes_key_len=16 "
                                    "digest_has_nul_in_first16=true")
                         .arg(QString::fromLatin1(kSessionId), attempt));
        }

        // Private: none of the raw lines holds the PIN, the salt, the digest or the key.
        const QStringList forbidden = { QString::fromLatin1(kPin), QStringLiteral("1234"),
                                        QStringLiteral("00000000000000000000000000000004"),
                                        secretHex, keyHex };
        for (const QString& line : all) {
            for (const QString& secret : forbidden) {
                QVERIFY2(!line.contains(secret), "a pairing record leaked secret material");
            }
        }

        // The scope ended with the call: a derivation after the pair adds no record.
        const int before = rawCapture.count();
        seathub::derivePairingKey(saltedPin, QCryptographicHash::Sha256);
        QCOMPARE(rawCapture.count(), before);
    }
};

QTEST_MAIN(TstPairing)

#include "tst_pairing.moc"

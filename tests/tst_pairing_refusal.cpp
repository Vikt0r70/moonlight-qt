/*****************************************************************************
 * SeatHub fork - unit tests for the refused start (M002 T012, SC5).
 *
 * The control plane answers a pairing read with 412 `session.no_verified_path` when it has no
 * verified path to the rig (T011). This client must:
 *
 *   * turn that into one typed failure with the sentence for its reason, before any
 *     `PairingTarget` is built, so no port is dialled and the customer reads it at once;
 *   * keep treating 409 as a wait and a healthy 200 as a dial of the signed control port;
 *   * report the end of a refused start as `session_refused`, never `pairing`;
 *   * write one `attempt_record side=client ...` line and one `attempt` evidence entry for EVERY
 *     attempt, refused or dialled, with the contract's keys in the contract's order.
 *****************************************************************************/

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QBuffer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/control_plane_client.h"
#include "seathub/error_map.h"
#include "seathub/evidence_journal.h"
#include "seathub/log_tee.h"
#include "seathub/pairing_controller.h"

namespace {

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
        if (httpStatus == 0) {
            setError(QNetworkReply::HostNotFoundError, QStringLiteral("fixture lookup failed"));
        }
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
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
        const int status = statuses.at(qMax(0, index));
        const QByteArray body = bodies.at(qMax(0, index));
        ++calls;
        return new FakeReply(status, body, this);
    }
};

// Records what it was asked to dial. A refused start must leave `calls` at zero.
class RecordingSeam : public PairingSeam
{
public:
    int calls = 0;
    PairingTarget lastTarget;

    void pair(const PairingTarget& target,
              std::function<void(bool, const QString&, const QString&)> done) override
    {
        ++calls;
        lastTarget = target;
        QTimer::singleShot(0, [done]() { done(true, QString::fromLatin1(kClientUuid), QString()); });
    }

    void clearPendingPairing(const PairingTarget&) override {}
};

ControlPlaneClient* wire(PairingController& controller, QNetworkAccessManager* fake)
{
    auto* client = new ControlPlaneClient(&controller);
    client->setNetworkAccessManager(fake);
    controller.setControlPlane(client);
    return client;
}

// The `attempt_record` lines the process logged, in order.
class AttemptLineCapture
{
public:
    AttemptLineCapture()
    {
        m_handle = LogTee::addSink([this](LogLevel, int, int, const QString& text) {
            if (text.contains(QLatin1String("attempt_record side=client"))) {
                QMutexLocker locker(&m_mutex);
                m_lines.append(text);
            }
        });
    }
    ~AttemptLineCapture()
    {
        if (m_handle != 0) {
            LogTee::removeSink(m_handle);
        }
    }
    QStringList lines() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines;
    }

private:
    LogTee::SinkHandle m_handle{0};
    mutable QMutex m_mutex;
    QStringList m_lines;
};

// The 412 T011 sends: the first clause of the sentence in `error`, the closed reason beside it,
// no `ports` key.
QByteArray refusalBody(const QString& reason)
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 412);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), QStringLiteral("We could not verify a path."));
    object.insert(QStringLiteral("reference"), QStringLiteral("SH-4K2XQ1"));
    object.insert(QStringLiteral("kind"), QStringLiteral("session.no_verified_path"));
    object.insert(QStringLiteral("refusal_reason"), reason);
    object.insert(QStringLiteral("retryable"), false);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray conflictBody()
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 409);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), QStringLiteral("The session isn't ready yet."));
    object.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray authorizationBody()
{
    QJsonObject ports;
    ports.insert(QStringLiteral("https"), 47984);
    ports.insert(QStringLiteral("control"), 47989);
    ports.insert(QStringLiteral("rtsp"), 48010);

    QJsonObject body;
    body.insert(QStringLiteral("session_id"), QLatin1String(kSessionId));
    body.insert(QStringLiteral("pairing_pin"), QLatin1String(kPin));
    body.insert(QStringLiteral("host_address"), QStringLiteral("203.0.113.7"));
    body.insert(QStringLiteral("ports"), ports);
    body.insert(QStringLiteral("state"), QStringLiteral("READY"));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// copy.md "Refused starts": the four sentences, key `session_refused.<reason>`.
QString expectedSentence(const QString& reason)
{
    if (reason == QLatin1String("no_external_path")) {
        return QStringLiteral("We could not verify a usable connection from your network, so we did "
                              "not start your session. You were not charged.");
    }
    if (reason == QLatin1String("no_lease")) {
        return QStringLiteral("Your session has no connection permission yet, so we did not start "
                              "it. You were not charged.");
    }
    if (reason == QLatin1String("host_unknown")) {
        return QStringLiteral("We could not verify the address of your rig, so we did not start "
                              "your session. You were not charged.");
    }
    return QStringLiteral("We could not verify a working path to your rig, so we did not start "
                          "your session. You were not charged.");
}

const char* kRefusedLine =
    "^attempt_record side=client session_id=%1 attempt=1 handed_https=- handed_control=- "
    "handed_rtsp=- dial_host=- dial_port=- outcome=refused refusal_class=%2 engine_step=- "
    "engine_class=- duration_ms=\\d+ at=\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z "
    "server_time=- skew_ms=-$";

QStringList keysOf(const QJsonObject& object)
{
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}

} // namespace

class TstPairingRefusal : public QObject
{
    Q_OBJECT

private slots:
    void cancellingOrDestroyingAControllerFencesLateDnsBeforeAnyPairingSend()
    {
        FakeNetworkAccessManager network;
        network.statuses = {200};
        network.bodies = {authorizationBody()};
        ControlPlaneClient client;
        client.setNetworkAccessManager(&network);
        QList<std::function<void(const QHostInfo&)>> pending;
        client.setPairingResolver([&](const QString&, QObject*, auto callback) {
            pending.append(callback);
            return []() {};
        });
        RecordingSeam seam;
        int failures = 0;
        {
            PairingController controller;
            controller.setControlPlane(&client);
            controller.setSeam(&seam);
            connect(&controller, &PairingController::pairingFailed, &client,
                    [&](const auto&) { ++failures; });
            controller.start(QString::fromLatin1(kSessionId));
            QCOMPARE(pending.size(), 1);
            controller.cancel();
            controller.start(QString::fromLatin1(kSessionId));
            QCOMPARE(pending.size(), 2);
        }
        QHostInfo info;
        info.setAddresses({QHostAddress(QHostAddress::LocalHost)});
        for (const auto& callback : pending) callback(info);
        QTest::qWait(50);
        QCOMPARE(network.calls, 2); // Only the two independent session reads, no pairing GET.
        QCOMPARE(seam.calls, 0);
        QCOMPARE(failures, 0);
    }

    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
        qRegisterMetaType<AttemptRecord>("AttemptRecord");
        LogTee::install();
    }

    void aRefusalGivesItsClassAndSentenceWithNoDial_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::newRow("no_external_path") << QStringLiteral("no_external_path");
        QTest::newRow("no_lease") << QStringLiteral("no_lease");
        QTest::newRow("host_unknown") << QStringLiteral("host_unknown");
        QTest::newRow("port_unverified") << QStringLiteral("port_unverified");
    }

    void aRefusalGivesItsClassAndSentenceWithNoDial()
    {
        QFETCH(QString, reason);
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        fake->statuses = { 412 };
        fake->bodies = { refusalBody(reason) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy stepFailed(&controller, &PairingController::stepFailed);
        QElapsedTimer timer;
        timer.start();
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
        QVERIFY2(timer.elapsed() < 2000, "a refusal must reach the customer in under 2 s");
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        QCOMPARE(failure.error, expectedSentence(reason));
        QCOMPARE(failure.statusCode, 412);
        QCOMPARE(failure.failure, QStringLiteral("session_refused.") + reason);
        QCOMPARE(failure.reference, QStringLiteral("SH-4K2XQ1"));
        QVERIFY(isSessionRefusal(failure));
        // Nothing was dialled, nothing was granted, and no pairing step is blamed.
        QCOMPARE(seam->calls, 0);
        QCOMPARE(granted.count(), 0);
        QCOMPARE(stepFailed.count(), 0);
        // One poll tick is the pairing read plus the session read that rides it. A refusal is
        // terminal, so no second tick (no retry, no wait) may follow.
        QCOMPARE(fake->calls, 2);
        QTest::qWait(50);
        QCOMPARE(fake->calls, 2);
    }

    void aRefusalNeverEndsAsPairing()
    {
        SeatHubFailure refusal = SeatHubFailure::api(
            412, QStringLiteral("x"), QStringLiteral("SH-4K2XQ1"),
            QStringLiteral("session_refused.no_lease"));
        QCOMPARE(pairingFailureStage(refusal), QStringLiteral("session_refused"));
        // Any other pairing failure keeps the stage it always had.
        QCOMPARE(pairingFailureStage(SeatHubFailure::api(404, QStringLiteral("x"), QString())),
                 QStringLiteral("pairing"));
        QCOMPARE(pairingFailureStage(SeatHubFailure::local(QStringLiteral("x"))),
                 QStringLiteral("pairing"));
        QVERIFY(!isSessionRefusal(SeatHubFailure::generic()));
    }

    void aLocalTransportFailureIsATerminalRefusalNotARigDeadline()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        fake->setParent(&controller);
        RecordingSeam seam;
        wire(controller, fake);
        controller.setSeam(&seam);
        fake->statuses = { 0 };
        fake->bodies = { QByteArray() };
        QCOMPARE(controller.deadlineMs(), 30000);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy steps(&controller, &PairingController::stepFailed);
        QSignalSpy records(&controller, &PairingController::attemptRecorded);
        QElapsedTimer clock;
        clock.start();
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 1900);
        QVERIFY(clock.elapsed() < 2000);
        const auto failure = failed.first().first().value<SeatHubFailure>();
        QCOMPARE(failure.kind, FailureKind::Local);
        QCOMPARE(failure.statusCode, 0);
        QVERIFY(failure.reference.isEmpty());
        QCOMPARE(failure.error, expectedSentence(QStringLiteral("no_external_path")));
        QCOMPARE(failure.failure, QStringLiteral("session_refused.no_external_path"));
        QCOMPARE(pairingFailureStage(failure), QStringLiteral("session_refused"));
        QCOMPARE(seam.calls, 0);
        QCOMPARE(granted.count(), 0);
        QCOMPARE(steps.count(), 0);
        QCOMPARE(records.count(), 1);
        QCOMPARE(records.first().first().value<AttemptRecord>().refusalClass,
                 QStringLiteral("no_external_path"));
        QCOMPARE(fake->calls, 2);
        QTest::qWait(1100);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(fake->calls, 2);
    }

    void aConflictStillWaitsAndNeverShowsTheRefusalSentence()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);
        fake->statuses = { 409, 409, 200 };
        fake->bodies = { conflictBody(), conflictBody(), authorizationBody() };

        AttemptLineCapture capture;
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy recorded(&controller, &PairingController::attemptRecorded);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(recorded.count(), 0); // the waits are not attempts; only a refusal is one here
        QVERIFY(capture.lines().isEmpty());
    }

    void aHealthy200StillDialsTheSignedControlPort()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        fake->statuses = { 200 };
        fake->bodies = { authorizationBody() };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.controlPort, 47989);
        QCOMPARE(seam->lastTarget.httpsPort, 47984);
        QCOMPARE(seam->lastTarget.rtspPort, 48010);
    }

    void aRefusedAttemptWritesOneLineWithTheContractKeys()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        wire(controller, fake);
        controller.setSeam(new RecordingSeam);
        fake->statuses = { 412 };
        fake->bodies = { refusalBody(QStringLiteral("no_lease")) };

        AttemptLineCapture capture;
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy recorded(&controller, &PairingController::attemptRecorded);
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(failed.count(), 1);

        QCOMPARE(recorded.count(), 1);
        const AttemptRecord record = recorded.at(0).at(0).value<AttemptRecord>();
        QCOMPARE(record.outcome, QStringLiteral("refused"));
        QCOMPARE(record.refusalClass, QStringLiteral("no_lease"));
        QCOMPARE(record.sessionId, QString::fromLatin1(kSessionId));
        QCOMPARE(record.attempt, 1);
        QCOMPARE(record.dialPort, 0); // no dial
        QVERIFY(record.dialHost.isEmpty());

        const QStringList lines = capture.lines();
        QCOMPARE(lines.size(), 1);
        const QRegularExpression pattern(
            QString::fromLatin1(kRefusedLine).arg(QString::fromLatin1(kSessionId),
                                                  QStringLiteral("no_lease")));
        QVERIFY2(pattern.match(lines.at(0).trimmed()).hasMatch(), qPrintable(lines.at(0)));
        // The privacy line: no PIN, certificate or free text on the wire of the record.
        QVERIFY(!lines.at(0).contains(QString::fromLatin1(kPin)));
        QVERIFY(!lines.at(0).contains(QLatin1String("not charged")));
    }

    void aRefusalRecordsTheHandedPortsWhenTheServerSentThem()
    {
        AttemptRecord record;
        record.sessionId = QString::fromLatin1(kSessionId);
        record.handedHttps = 47984;
        record.handedControl = 47989;
        record.handedRtsp = 48010;
        record.outcome = QStringLiteral("refused");
        record.refusalClass = QStringLiteral("port_unverified");
        record.durationMs = 12;
        record.at = QDateTime::fromString(QStringLiteral("2026-10-26T10:00:00.250Z"), Qt::ISODateWithMs);
        const QString line = attemptRecordLine(record);
        QVERIFY2(line.contains(QLatin1String("handed_https=47984 handed_control=47989 handed_rtsp=48010 "
                                             "dial_host=- dial_port=- outcome=refused")),
                 qPrintable(line));
        QVERIFY(line.contains(QLatin1String("at=2026-10-26T10:00:00.250Z")));
    }

    void aDialledAttemptKeepsTheHandedPortsAndTheDialedControlPort()
    {
        PairingTarget target;
        target.sessionId = QString::fromLatin1(kSessionId);
        target.hostAddress = QStringLiteral("203.0.113.7");
        target.httpsPort = 47984;
        target.controlPort = 47989;
        target.rtspPort = 48010;
        target.pairingPin = QString::fromLatin1(kPin);

        AttemptRecord record = dialedAttemptRecord(target, 2, false, QStringLiteral("pair_exchange"),
                                                   QStringLiteral("timed_out"), 840);
        stampAttemptClock(&record,
                          QDateTime::fromString(QStringLiteral("2026-10-26T10:00:00.000Z"), Qt::ISODateWithMs),
                          QDateTime::fromString(QStringLiteral("2026-10-26T10:00:02.000Z"), Qt::ISODateWithMs));
        const QString line = attemptRecordLine(record);
        const QRegularExpression pattern(QStringLiteral(
            "^attempt_record side=client session_id=%1 attempt=2 handed_https=47984 "
            "handed_control=47989 handed_rtsp=48010 dial_host=203\\.0\\.113\\.7 dial_port=47989 "
            "outcome=failed refusal_class=- engine_step=pair_exchange engine_class=timed_out "
            "duration_ms=840 at=\\S+Z server_time=\\S+Z skew_ms=2000$").arg(QString::fromLatin1(kSessionId)));
        QVERIFY2(pattern.match(line).hasMatch(), qPrintable(line));
        QVERIFY(!line.contains(QString::fromLatin1(kPin)));

        const AttemptRecord ok = dialedAttemptRecord(target, 1, true, QStringLiteral("pair_exchange"),
                                                     QString(), 120);
        QCOMPARE(ok.outcome, QStringLiteral("connected"));
        QVERIFY(attemptRecordLine(ok).contains(QLatin1String("engine_class=-")));
    }

    void theEvidenceEntryCarriesExactlyTheContractKeys()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        EvidenceJournal journal(dir.path());
        QVERIFY(journal.open(QString::fromLatin1(kSessionId), QStringLiteral("acct-1")));

        AttemptRecord refused;
        refused.sessionId = QString::fromLatin1(kSessionId);
        refused.outcome = QStringLiteral("refused");
        refused.refusalClass = QStringLiteral("host_unknown");
        refused.durationMs = 30;
        const QJsonObject refusedEntry = EvidenceJournal::attempt(refused);
        QCOMPARE(keysOf(refusedEntry),
                 (QStringList{ "attempt", "duration_ms", "kind", "outcome", "refusal_class",
                               "session_id", "side" }));
        QCOMPARE(refusedEntry.value(QStringLiteral("kind")).toString(), QStringLiteral("attempt"));
        QCOMPARE(refusedEntry.value(QStringLiteral("side")).toString(), QStringLiteral("client"));

        PairingTarget target;
        target.sessionId = QString::fromLatin1(kSessionId);
        target.hostAddress = QStringLiteral("203.0.113.7");
        target.httpsPort = 47984;
        target.controlPort = 47989;
        target.rtspPort = 48010;
        AttemptRecord dialled = dialedAttemptRecord(target, 1, false, QStringLiteral("pair_exchange"),
                                                    QStringLiteral("timed_out"), 840);
        stampAttemptClock(&dialled,
                          QDateTime::fromString(QStringLiteral("2026-10-26T10:00:00.000Z"), Qt::ISODateWithMs),
                          QDateTime::fromString(QStringLiteral("2026-10-26T10:00:02.000Z"), Qt::ISODateWithMs));
        const QJsonObject dialledEntry = EvidenceJournal::attempt(dialled);
        QCOMPARE(keysOf(dialledEntry),
                 (QStringList{ "attempt", "dial_host", "dial_port", "duration_ms", "engine_class",
                               "engine_step", "handed_control", "handed_https", "handed_rtsp",
                               "kind", "outcome", "server_time", "session_id", "side", "skew_ms" }));
        QCOMPARE(dialledEntry.value(QStringLiteral("skew_ms")).toInt(), 2000);
        QVERIFY(!dialledEntry.contains(QStringLiteral("pairing_pin")));

        // Both entries are accepted by the journal, so they reach the next capsule.
        journal.record(refusedEntry);
        journal.record(dialledEntry);
        QCOMPARE(journal.entryCount(), 2);

        // A value outside the closed set is refused, not stored.
        AttemptRecord bad = refused;
        bad.outcome = QStringLiteral("exploded");
        QVERIFY(EvidenceJournal::attempt(bad).isEmpty());
    }
};

QTEST_MAIN(TstPairingRefusal)
#include "tst_pairing_refusal.moc"

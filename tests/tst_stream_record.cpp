/*****************************************************************************
 * SeatHub fork - the stream record's own tests (T054, SC23/SC24, ADR-0117).
 *
 * What is proved here:
 *   1. A 300-minute Play yields 300 minute rows across rolled-over capsules (each run's seq counts
 *      from 0, every minute appears once, the window start rises by a minute), zero-sample minutes
 *      included, with the closing entries kept in the last capsule (E36, E37).
 *   2. An unmapped engine line becomes `other` and no text, address or host name reaches a capsule;
 *      repeated codes are both kept and the 17th warning is counted, not stored.
 *   3. A bad end writes one `stream_tail` with exactly the distinct offsets the duration owes, and
 *      the journal itself refuses a tail with duplicate or missing offsets.
 *   4. The drop check makes one health request and one connect-and-close, with no retry.
 *   5. Offline, an undelivered stream capsule outlives cap eviction and goes out next connection.
 *   6. All of it THROUGH `SeatHubClient`: sampler-driven minute rows, and a bad end that writes the
 *      tail and the drop check into the same capsule sequence before the journal closes.
 *
 * Like tst_facade_wiring, this binary substitutes the engine and the pairing handshake at their own
 * seams (no engine is linked and no Sunshine host exists).
 *
 * Run: tests\run-all-suites.cmd tst_stream_record
 *****************************************************************************/

// This file defines `main()` via QTEST_MAIN. `<SDL.h>` otherwise renames `main` to `SDL_main`
// (see tst_stream_stats.cpp for the same fix).
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>

#include <atomic>
#include <functional>

#include "seathub/control_plane_client.h"
#include "seathub/drop_check.h"
#include "seathub/engine_session.h"
#include "seathub/evidence_journal.h"
#include "seathub/moonlight_engine_session.h"
#include "seathub/pairing_handshake.h"
#include "seathub/reporting_consent.h"
#include "seathub/seathub_client.h"
#include "seathub/stream_record.h"
#include "seathub/token_store.h"
#include "settings/streamingpreferences.h"

// ---------------------------------------------------------------- the stitched seams ------------
// Same substitutions as tst_facade_wiring.cpp, for the same reason: neither translation unit can
// come with the facade, and both answer the fail-closed way their production counterparts document.

MoonlightEngineSession* MoonlightEngineSession::create(const PairedHostPtr&, QObject*,
                                                       EngineLaunchReason* reason)
{
    if (reason != nullptr) {
        *reason = EngineLaunchReason::CreateFailed;
    }
    return nullptr;
}

void MoonlightEngineSession::setDebugLineFilter(const QStringList&)
{
}

void MoonlightEngineSession::setTextRasterizer(Overlay::OverlayManager::TextRasterizer, void*)
{
}

PairingHandshakeResult runUpstreamPairingHandshake(const PairingTarget&)
{
    PairingHandshakeResult result;
    result.ok = false;
    result.engineError = QStringLiteral("no Sunshine host exists in this test environment");
    return result;
}

PairingHandshakeResult runUpstreamWarmReturnCheck(const PairingTarget&, const QByteArray&)
{
    return PairingHandshakeResult();
}

QString ownClientCertificateFingerprint()
{
    return QString();
}

namespace {

const char* kAccessToken = "opaque-access-token";

class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent, int delayMs = 0)
        : QNetworkReply(parent)
    {
        if (httpStatus == 0) {
            setError(QNetworkReply::HostNotFoundError, QStringLiteral("no route to control plane"));
        }
        else {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant(httpStatus));
        }
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(delayMs, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
    qint64 bytesAvailable() const override { return m_buffer.size() + QNetworkReply::bytesAvailable(); }

private:
    QBuffer m_buffer;
};

/// The smallest control plane the Play needs: sign-in reads, the pairing poll refused as "rig not
/// ready", `/end`, `/evidence` and `/health`. It records every path it was asked for.
class MiniPlane : public QNetworkAccessManager
{
    Q_OBJECT

public:
    explicit MiniPlane(QObject* parent = nullptr) : QNetworkAccessManager(parent) {}

    void answerHealth(int status, int delayMs = 0)
    {
        QMutexLocker lock(&m_mutex);
        m_healthStatus = status;
        m_healthDelayMs = delayMs;
    }
    int count(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_paths.count(path);
    }
    QStringList paths() const
    {
        QMutexLocker lock(&m_mutex);
        return m_paths;
    }

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        const QString path = request.url().path();
        int healthStatus;
        int healthDelay;
        {
            QMutexLocker lock(&m_mutex);
            m_paths.append(path);
            healthStatus = m_healthStatus;
            healthDelay = m_healthDelayMs;
        }
        if (path == QLatin1String("/health")) {
            return new FakeReply(healthStatus, QByteArrayLiteral("{\"status\":\"ok\"}"), this, healthDelay);
        }
        if (path == QLatin1String("/api/me")) {
            return new FakeReply(
                200,
                QByteArrayLiteral(
                    "{\"id\":\"6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e\",\"display_name\":\"Lina\","
                    "\"username\":\"lina\",\"email\":\"lina@example.com\",\"phone_e164\":\"+962790000000\","
                    "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
                    "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}"),
                this);
        }
        if (path == QLatin1String("/api/wallet")) {
            return new FakeReply(200,
                                 QByteArrayLiteral("{\"balance_minutes\":90,\"updated_at\":\"2026-09-21T10:00:00Z\"}"),
                                 this);
        }
        if (path.endsWith(QLatin1String("/pairing"))) {
            return new FakeReply(409,
                                 QByteArrayLiteral("{\"status\":false,\"error\":\"The rig is not ready yet.\","
                                                   "\"reference\":\"SH-2K2XQ1\"}"),
                                 this);
        }
        if (path.endsWith(QLatin1String("/end"))) {
            return new FakeReply(200, QByteArrayLiteral("{\"status\":true}"), this);
        }
        if (path.endsWith(QLatin1String("/evidence"))) {
            // A 5xx keeps every capsule on disk (ADR-0105), so the test can read what was written.
            return new FakeReply(503, QByteArrayLiteral("{\"status\":false,\"error\":\"busy\"}"), this);
        }
        if (path.endsWith(QLatin1String("/quality"))) {
            return new FakeReply(200, QByteArrayLiteral("{\"status\":true}"), this);
        }
        return new FakeReply(200,
                             QByteArrayLiteral("{\"id\":\"session\",\"state\":\"COMPLETED\","
                                               "\"quality_profile\":\"1080p60\",\"minutes_billed\":7,"
                                               "\"reconnect_count\":0,\"requested_at\":\"2026-09-19T00:00:00Z\"}"),
                             this);
    }

private:
    mutable QMutex m_mutex;
    QStringList m_paths;
    int m_healthStatus = 200;
    int m_healthDelayMs = 0;
};

class FakeEngineSession : public EngineSession
{
    Q_OBJECT

public:
    using EngineSession::EngineSession;

    void run(QWindow*) override {}
    void interrupt() override {}
    bool publishOverlaySurface(SDL_Surface*) override { return true; }
};

/// A local listener that counts the connections it accepted - the rig's port P for the drop check.
class CountingListener : public QObject
{
    Q_OBJECT

public:
    CountingListener()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                ++m_accepted;
                socket->deleteLater();
            }
        });
        m_listening = m_server.listen(QHostAddress::LocalHost, 0);
    }
    bool listening() const { return m_listening; }
    quint16 port() const { return m_server.serverPort(); }
    int accepted() const { return m_accepted; }
    void stop() { m_server.close(); }

private:
    QTcpServer m_server;
    bool m_listening = false;
    int m_accepted = 0;
};

QDateTime serverStart()
{
    return QDateTime::fromString(QStringLiteral("2026-10-25T10:00:00Z"), Qt::ISODate);
}

VideoStats sample(double fps, double lossPct, double jitterPct, double rttMs, double decodeMs)
{
    VideoStats stats;
    stats.renderedFps = OptionalMetric::of(fps);
    stats.networkDroppedFramePct = OptionalMetric::of(lossPct);
    stats.jitterDroppedFramePct = OptionalMetric::of(jitterPct);
    stats.rttMs = OptionalMetric::of(rttMs);
    stats.decodeTimeMs = OptionalMetric::of(decodeMs);
    return stats;
}

struct Capsule
{
    QString runId;
    QJsonArray entries;
};

QList<Capsule> readCapsules(const QString& directory)
{
    QList<Capsule> capsules;
    const QDir dir(directory);
    for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        capsules.append({object.value(QStringLiteral("run_id")).toString(),
                         object.value(QStringLiteral("entries")).toArray()});
    }
    return capsules;
}

QList<QJsonObject> entriesOfKind(const QList<Capsule>& capsules, const QString& kind)
{
    QList<QJsonObject> found;
    for (const Capsule& capsule : capsules) {
        for (const QJsonValue& value : capsule.entries) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("kind")).toString() == kind) {
                found.append(entry);
            }
        }
    }
    return found;
}

bool anyNull(const QJsonObject& object)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isNull()) {
            return true;
        }
    }
    return false;
}

} // namespace

class TstStreamRecord : public QObject
{
    Q_OBJECT

private:
    QScopedPointer<QTemporaryDir> m_dir;
    MiniPlane* m_plane = nullptr;

    QString journalDir() const { return m_dir->path() + QStringLiteral("/evidence-journal"); }

    /// A tail entry as the journal's validator sees it, built by hand so the refusal of a bad
    /// offset set does not depend on the recorder producing one.
    static QJsonObject handBuiltTail(const QList<int>& offsets)
    {
        QJsonArray seconds;
        for (int offset : offsets) {
            QJsonObject second;
            second.insert(QStringLiteral("offset_s"), offset);
            second.insert(QStringLiteral("fps"), 60);
            second.insert(QStringLiteral("network_loss_pct"), 0);
            second.insert(QStringLiteral("delay_ms"), 20);
            seconds.append(second);
        }
        QJsonObject tail;
        tail.insert(QStringLiteral("kind"), QStringLiteral("stream_tail"));
        tail.insert(QStringLiteral("run_id"), QStringLiteral("00000000-0000-4000-8000-000000000001"));
        tail.insert(QStringLiteral("end_reason"), QStringLiteral("engine_failure"));
        tail.insert(QStringLiteral("ended_at_server_time"), QStringLiteral("2026-10-25T10:00:30.000Z"));
        tail.insert(QStringLiteral("seconds"), seconds);
        return tail;
    }

    /// One Play's worth of seconds: a sample in the middle of every second up to `seconds`.
    static void playSeconds(StreamRecorder& recorder, int seconds)
    {
        for (int s = 0; s < seconds; ++s) {
            recorder.addSample(qint64(s) * 1000 + 500, sample(s + 1, 0.5, 0.1, 20 + s, 3));
        }
    }

private slots:
    void init()
    {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        ReportingConsent::setStateForTests(false, reporting_consent_store::Record{});
    }

    // ---------------------------------------------------------------- AC1: minutes and rollover

    void aThreeHundredMinutePlayYieldsThreeHundredMinuteRowsAcrossRolledOverCapsules()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-long"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());

        // 299 whole minutes plus a 30 s partial one. Every 7th minute has no sample at all.
        for (int minute = 0; minute < 299; ++minute) {
            if (minute % 7 != 3) {
                recorder.addSample(qint64(minute) * 60000 + 1000, sample(60, 0.2, 0.1, 25, 3));
            }
            recorder.advance(qint64(minute + 1) * 60000);
        }
        recorder.addSample(299LL * 60000 + 1000, sample(60, 0.2, 0.1, 25, 3));
        recorder.finish(299LL * 60000 + 30000);
        QVERIFY(recorder.writeTail(299LL * 60000 + 30000, StreamRecord::EndReason::EngineFailure));
        StreamRecorder::DropCheckResult check;
        check.serverReachable = true;
        check.rigPortOpen = true;
        QVERIFY(recorder.writeDropCheck(299LL * 60000 + 30100, check));

        const QList<Capsule> capsules = readCapsules(journalDir());
        QVERIFY2(capsules.size() >= 2, "300 minutes cannot fit one 256-entry capsule");
        for (const Capsule& capsule : capsules) {
            QVERIFY(capsule.entries.size() <= EvidenceJournal::kDefaultEntryCap);
        }

        const QList<QJsonObject> minutes = entriesOfKind(capsules, QStringLiteral("stream_minute"));
        QCOMPARE(minutes.size(), 300);

        // (run_id, minute_seq) names a minute; each run counts from 0 with no gap.
        QHash<QString, QList<int>> seqsByRun;
        QSet<QString> starts;
        for (const QJsonObject& minute : minutes) {
            seqsByRun[minute.value(QStringLiteral("run_id")).toString()].append(
                minute.value(QStringLiteral("minute_seq")).toInt());
            starts.insert(minute.value(QStringLiteral("window_start_server_time")).toString());
            QVERIFY(!anyNull(minute));
        }
        QCOMPARE(starts.size(), 300);
        for (auto it = seqsByRun.constBegin(); it != seqsByRun.constEnd(); ++it) {
            for (int i = 0; i < it.value().size(); ++i) {
                QCOMPARE(it.value().at(i), i);
            }
        }
        // Across the whole Play the window start rises by exactly one minute per row.
        QDateTime expected = serverStart();
        for (const QJsonObject& minute : minutes) {
            QCOMPARE(minute.value(QStringLiteral("window_start_server_time")).toString(),
                     expected.toString(Qt::ISODateWithMs));
            expected = expected.addSecs(60);
        }

        // Zero-sample minutes are written, with no readings and no null.
        int zero = 0;
        for (const QJsonObject& minute : minutes) {
            if (minute.value(QStringLiteral("sample_count")).toInt() == 0) {
                ++zero;
                for (const char* reading : {"fps_avg", "network_loss_pct", "jitter_drop_pct",
                                            "delay_ms_p50", "delay_ms_p95", "decode_ms_p50"}) {
                    QVERIFY2(!minute.contains(QLatin1String(reading)), reading);
                }
            }
        }
        QVERIFY(zero >= 40);

        // The closing entries sit in the capsule the Play ended in, after its last minute.
        const Capsule& last = capsules.last();
        QCOMPARE(last.entries.at(last.entries.size() - 2).toObject().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("stream_tail"));
        QCOMPARE(last.entries.at(last.entries.size() - 1).toObject().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("drop_check"));
    }

    void aMinuteWithSamplesCarriesExactlyTheClosedFields()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-fields"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        recorder.setOverlay(StreamRecord::Overlay::Visible);
        recorder.addSample(1000, sample(58, 1.5, 0.5, 20, 3));
        recorder.addSample(2000, sample(60, 0.5, 0.1, 40, 5));
        recorder.addSample(3000, sample(60, 0.5, 0.1, 30, 4));
        recorder.advance(60000);

        const QList<QJsonObject> minutes = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute"));
        QCOMPARE(minutes.size(), 1);
        const QJsonObject& minute = minutes.first();
        QCOMPARE(minute.value(QStringLiteral("minute_seq")).toInt(), 0);
        QCOMPARE(minute.value(QStringLiteral("sample_count")).toInt(), 3);
        QCOMPARE(minute.value(QStringLiteral("overlay_visible")).toString(), QStringLiteral("true"));
        QCOMPARE(minute.value(QStringLiteral("warnings_dropped")).toInt(), 0);
        QVERIFY(minute.value(QStringLiteral("warnings")).toArray().isEmpty());
        QVERIFY(qAbs(minute.value(QStringLiteral("fps_avg")).toDouble() - 59.3333) < 0.01);
        QCOMPARE(minute.value(QStringLiteral("delay_ms_p50")).toInt(), 30);
        QCOMPARE(minute.value(QStringLiteral("delay_ms_p95")).toInt(), 40);
        QCOMPARE(minute.value(QStringLiteral("decode_ms_p50")).toInt(), 4);
        QCOMPARE(minute.value(QStringLiteral("poor_connection_flags")).toInt(), 0);
        QVERIFY(minute.value(QStringLiteral("coverage_pct")).toDouble() > 0);
        QVERIFY(minute.value(QStringLiteral("coverage_pct")).toDouble() <= 100);
        QVERIFY(!anyNull(minute));
    }

    void theOverlayIsUnknownUntilTheClientSaysOtherwise()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-overlay"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        recorder.advance(60000);
        recorder.setOverlay(StreamRecord::Overlay::Hidden);
        recorder.advance(120000);
        const QList<QJsonObject> minutes = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute"));
        QCOMPARE(minutes.size(), 2);
        QCOMPARE(minutes.at(0).value(QStringLiteral("overlay_visible")).toString(), QStringLiteral("unknown"));
        QCOMPARE(minutes.at(1).value(QStringLiteral("overlay_visible")).toString(), QStringLiteral("false"));
    }

    // ---------------------------------------------------------------- AC2: warnings

    void anUnmappedEngineLineIsOtherAndItsTextNeverReachesACapsule()
    {
        using StreamRecord::WarningCode;
        const QString secretLine = QStringLiteral("Unexpected failure talking to 10.1.2.3 (host rig-secret-7)");
        QCOMPARE(StreamRecord::warningCodeForLine(secretLine), WarningCode::Other);
        QCOMPARE(StreamRecord::warningCodeForLine(QStringLiteral("Resetting decoder due to consistent failure")),
                 WarningCode::DecoderReset);
        QCOMPARE(StreamRecord::warningCodeForLine(
                     QStringLiteral("Failed to open audio device. Audio will be unavailable during this session.")),
                 WarningCode::AudioUnavailable);
        QCOMPARE(StreamRecord::warningCodeForLine(QStringLiteral("Connection status update: 1")),
                 WarningCode::PoorConnection);
        QCOMPARE(StreamRecord::warningCodeForLine(QStringLiteral("Connection status update: 0")),
                 WarningCode::ConnectionRecovered);

        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-warn"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        recorder.addWarning(4000, StreamRecord::warningCodeForLine(secretLine));
        recorder.advance(60000);

        const QDir dir(journalDir());
        for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files)) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            QVERIFY(!bytes.contains("10.1.2.3"));
            QVERIFY(!bytes.contains("rig-secret"));
            QVERIFY(!bytes.contains("Unexpected"));
        }
        const QJsonObject minute = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute")).first();
        const QJsonArray warnings = minute.value(QStringLiteral("warnings")).toArray();
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings.at(0).toObject().value(QStringLiteral("code")).toString(), QStringLiteral("other"));
        QCOMPARE(warnings.at(0).toObject().value(QStringLiteral("offset_s")).toInt(), 4);
        QCOMPARE(warnings.at(0).toObject().keys().size(), 2);
    }

    void twoOccurrencesOfOneCodeAreBothKeptAndTheSeventeenthIsCounted()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-warn17"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        using StreamRecord::WarningCode;
        recorder.addWarning(3000, WarningCode::DecoderReset);
        recorder.addWarning(9000, WarningCode::DecoderReset);
        for (int i = 0; i < 15; ++i) {
            recorder.addWarning(10000 + i * 1000, WarningCode::Stall);
        }
        recorder.advance(60000);

        const QJsonObject minute = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute")).first();
        const QJsonArray warnings = minute.value(QStringLiteral("warnings")).toArray();
        QCOMPARE(warnings.size(), 16);
        QCOMPARE(minute.value(QStringLiteral("warnings_dropped")).toInt(), 1);
        int resets = 0;
        for (const QJsonValue& value : warnings) {
            if (value.toObject().value(QStringLiteral("code")).toString() == QLatin1String("decoder_reset")) {
                ++resets;
            }
        }
        QCOMPARE(resets, 2);
    }

    void warningsAreOrderedByOffsetThenCodeText()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-order"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        using StreamRecord::WarningCode;
        // Same second, arriving in the opposite order to the code text.
        recorder.addWarning(7100, WarningCode::Stall);
        recorder.addWarning(7200, WarningCode::PoorConnection);
        recorder.addWarning(7300, WarningCode::AudioUnavailable);
        recorder.advance(60000);

        const QJsonObject minute = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute")).first();
        QCOMPARE(minute.value(QStringLiteral("poor_connection_flags")).toInt(), 1);
        QStringList codes;
        for (const QJsonValue& value : minute.value(QStringLiteral("warnings")).toArray()) {
            codes.append(value.toObject().value(QStringLiteral("code")).toString());
        }
        QCOMPARE(codes, (QStringList{QStringLiteral("audio_unavailable"), QStringLiteral("poor_connection"),
                                     QStringLiteral("stall")}));
    }

    // ---------------------------------------------------------------- AC3: the tail

    void aFullTailHasTheThirtyDistinctOffsetsWithZeroTheLastSecond()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-tail"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        playSeconds(recorder, 45);
        QVERIFY(recorder.writeTail(45000, StreamRecord::EndReason::ConnectionTerminated));

        const QList<QJsonObject> tails = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_tail"));
        QCOMPARE(tails.size(), 1);
        const QJsonObject tail = tails.first();
        QCOMPARE(tail.value(QStringLiteral("end_reason")).toString(), QStringLiteral("connection_terminated"));
        QCOMPARE(tail.value(QStringLiteral("run_id")).toString(), journal.runId());
        QCOMPARE(tail.value(QStringLiteral("ended_at_server_time")).toString(),
                 serverStart().addSecs(45).toString(Qt::ISODateWithMs));
        const QJsonArray seconds = tail.value(QStringLiteral("seconds")).toArray();
        QCOMPARE(seconds.size(), 30);
        QSet<int> offsets;
        for (const QJsonValue& value : seconds) {
            offsets.insert(value.toObject().value(QStringLiteral("offset_s")).toInt());
            QCOMPARE(value.toObject().keys().size(), 4);
        }
        QCOMPARE(offsets.size(), 30);
        for (int offset = 0; offset < 30; ++offset) {
            QVERIFY(offsets.contains(offset));
        }
        // Offset 0 is the last second played (the 45th, fps 45); offset 29 is 29 s earlier (fps 16).
        for (const QJsonValue& value : seconds) {
            const QJsonObject second = value.toObject();
            QCOMPARE(second.value(QStringLiteral("fps")).toDouble(),
                     double(45 - second.value(QStringLiteral("offset_s")).toInt()));
        }
    }

    void aShortPlayOwesOnlyTheSecondsItPlayedIncludingThePartialOne()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-short"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        playSeconds(recorder, 12);
        recorder.addSample(12100, sample(99, 0, 0, 20, 3)); // the partial 13th second
        QVERIFY(recorder.writeTail(12400, StreamRecord::EndReason::StallTimeout));

        const QJsonObject tail = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_tail")).first();
        const QJsonArray seconds = tail.value(QStringLiteral("seconds")).toArray();
        QCOMPARE(seconds.size(), 13); // ceil(12.4)
        QSet<int> offsets;
        for (const QJsonValue& value : seconds) {
            offsets.insert(value.toObject().value(QStringLiteral("offset_s")).toInt());
        }
        QCOMPARE(offsets.size(), 13);
        for (int offset = 0; offset < 13; ++offset) {
            QVERIFY(offsets.contains(offset));
        }
        for (const QJsonValue& value : seconds) {
            if (value.toObject().value(QStringLiteral("offset_s")).toInt() == 0) {
                QCOMPARE(value.toObject().value(QStringLiteral("fps")).toDouble(), 99.0);
            }
        }
    }

    void aSecondWithoutASampleIsStillWrittenAndNoOffsetIsDuplicated()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-gap"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        recorder.addSample(500, sample(60, 0, 0, 20, 3));
        recorder.addSample(1500, sample(60, 0, 0, 20, 3));
        recorder.addSample(3500, sample(60, 0, 0, 20, 3)); // second 2 has none
        QVERIFY(recorder.writeTail(4000, StreamRecord::EndReason::Other));

        const QJsonObject tail = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_tail")).first();
        const QJsonArray seconds = tail.value(QStringLiteral("seconds")).toArray();
        QCOMPARE(seconds.size(), 4);
        QList<int> offsets;
        for (const QJsonValue& value : seconds) {
            offsets.append(value.toObject().value(QStringLiteral("offset_s")).toInt());
        }
        std::sort(offsets.begin(), offsets.end());
        QCOMPARE(offsets, (QList<int>{0, 1, 2, 3}));
    }

    void aPlayOfZeroDurationOwesNoSeconds()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-zero"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(1000, serverStart());
        QVERIFY(recorder.writeTail(1000, StreamRecord::EndReason::EngineFailure));
        const QJsonObject tail = entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_tail")).first();
        QVERIFY(tail.value(QStringLiteral("seconds")).toArray().isEmpty());
    }

    void theJournalRefusesATailWithDuplicateOrMissingOffsets()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-reject"), QStringLiteral("acct-1")));

        journal.record(handBuiltTail({0, 1, 1, 3}));  // duplicate
        journal.record(handBuiltTail({0, 2, 3}));      // missing 1
        journal.record(handBuiltTail({1, 2, 3}));      // no offset 0
        journal.record(handBuiltTail({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                      20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30})); // more than 30
        QCOMPARE(journal.entryCount(), 0);
        QCOMPARE(journal.pendingCounters().droppedEntries, qint64(4));

        journal.record(handBuiltTail({0, 1, 2, 3}));
        QCOMPARE(journal.entryCount(), 1);
    }

    // ---------------------------------------------------------------- AC4: offline cap

    void offlineAnUndeliveredStreamCapsuleOutlivesTheCapAndGoesOutNextConnection()
    {
        const QString dir = journalDir();
        // The stream capsule is the OLDEST one: a plain oldest-first cap would take it first.
        {
            EvidenceJournal first(dir);
            QVERIFY(first.open(QStringLiteral("s-stream"), QStringLiteral("acct-1")));
            StreamRecorder recorder(&first);
            recorder.begin(0, serverStart());
            for (int minute = 0; minute < 4; ++minute) {
                recorder.addSample(qint64(minute) * 60000 + 1000, sample(60, 0, 0, 20, 3));
                recorder.advance(qint64(minute + 1) * 60000);
            }
            first.close();
        }
        QThread::msleep(5);
        for (int i = 0; i < 6; ++i) {
            EvidenceJournal other(dir);
            other.setDirectoryCapBytes(3000);
            QVERIFY(other.open(QStringLiteral("s-other%1").arg(i), QStringLiteral("acct-1")));
            for (int n = 0; n < 4; ++n) {
                other.record(EvidenceJournal::stallWindow(2, 120000));
            }
            other.close();
            QThread::msleep(5);
        }

        EvidenceJournal later(dir);
        later.setDirectoryCapBytes(3000);
        const QList<EvidenceJournal::PendingCapsule> pending = later.deliverable(QStringLiteral("acct-1"));
        bool streamCapsuleKept = false;
        for (const EvidenceJournal::PendingCapsule& capsule : pending) {
            if (capsule.sessionId == QLatin1String("s-stream")) {
                streamCapsuleKept = true;
                QVERIFY(capsule.body.contains("stream_minute"));
                later.applyAnswer(capsule.sessionId, capsule.runId, 200);
            }
        }
        QVERIFY2(streamCapsuleKept, "the undelivered stream capsule must survive the cap");
        // Delivered, it is gone: the next connection has nothing left to resend.
        for (const EvidenceJournal::PendingCapsule& capsule : later.deliverable(QStringLiteral("acct-1"))) {
            QVERIFY(capsule.sessionId != QLatin1String("s-stream"));
        }
    }

    // ---------------------------------------------------------------- AC3: the drop check

    void theDropCheckMakesOneHealthRequestAndOneConnectAndNothingElse()
    {
        CountingListener rig;
        QVERIFY(rig.listening());
        MiniPlane plane;
        ControlPlaneClient client;
        client.setBaseUrl(QStringLiteral("https://control.invalid"));
        client.setNetworkAccessManager(&plane);

        DropCheck check(&client);
        QSignalSpy finished(&check, &DropCheck::finished);
        check.start(QStringLiteral("127.0.0.1"), rig.port());
        check.start(QStringLiteral("127.0.0.1"), rig.port()); // a second call is ignored
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);

        const auto result = finished.first().first().value<DropCheck::Result>();
        QVERIFY(result.serverReachable);
        QVERIFY(result.rigPortOpen);
        QVERIFY(result.serverMs >= 0);
        QVERIFY(result.rigMs >= 0);

        QTest::qWait(300); // a retry or a reconnect would show by now
        QCOMPARE(finished.count(), 1);
        QCOMPARE(plane.count(QStringLiteral("/health")), 1);
        QCOMPARE(plane.paths().size(), 1);
        QCOMPARE(rig.accepted(), 1);
    }

    void aRefusedHealthAndAClosedPortAreRecordedAsFalseWithoutARetry()
    {
        CountingListener rig;
        QVERIFY(rig.listening());
        const quint16 closedPort = rig.port();
        rig.stop();

        MiniPlane plane;
        plane.answerHealth(503);
        ControlPlaneClient client;
        client.setBaseUrl(QStringLiteral("https://control.invalid"));
        client.setNetworkAccessManager(&plane);

        DropCheck check(&client);
        QSignalSpy finished(&check, &DropCheck::finished);
        check.start(QStringLiteral("127.0.0.1"), closedPort);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished.first().first().value<DropCheck::Result>();
        QVERIFY(!result.serverReachable);
        QVERIFY(!result.rigPortOpen);
        QTest::qWait(300);
        QCOMPARE(plane.count(QStringLiteral("/health")), 1);
        QCOMPARE(finished.count(), 1);
    }

    void aHungHealthRequestIsGivenUpOnAtTheTimeout()
    {
        QCOMPARE(DropCheck::kTimeoutMs, 5000);
        MiniPlane plane;
        plane.answerHealth(200, 60000);
        ControlPlaneClient client;
        client.setBaseUrl(QStringLiteral("https://control.invalid"));
        client.setNetworkAccessManager(&plane);

        DropCheck check(&client);
        check.setTimeoutMsForTests(250);
        QSignalSpy finished(&check, &DropCheck::finished);
        check.start(QString(), 0); // the rig endpoint was never learned: no connect is made
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        const auto result = finished.first().first().value<DropCheck::Result>();
        QVERIFY(!result.serverReachable);
        QVERIFY(result.serverMs >= 200);
        QVERIFY(!result.rigPortOpen);
        QCOMPARE(result.rigMs, 0);
    }

    void theDropCheckInstantIsNeverEarlierThanTheBadEnd()
    {
        EvidenceJournal journal(journalDir());
        QVERIFY(journal.open(QStringLiteral("s-drop"), QStringLiteral("acct-1")));
        StreamRecorder recorder(&journal);
        recorder.begin(0, serverStart());
        QVERIFY(recorder.writeTail(10000, StreamRecord::EndReason::ConnectionTerminated));
        StreamRecorder::DropCheckResult check;
        check.serverReachable = true;
        check.serverMs = 40;
        check.rigPortOpen = false;
        check.rigMs = 12;
        QVERIFY(recorder.writeDropCheck(9000, check)); // a clock quirk: earlier than the tail

        const QList<Capsule> capsules = readCapsules(journalDir());
        const QJsonObject tail = entriesOfKind(capsules, QStringLiteral("stream_tail")).first();
        const QJsonObject drop = entriesOfKind(capsules, QStringLiteral("drop_check")).first();
        QCOMPARE(drop.value(QStringLiteral("run_id")).toString(), tail.value(QStringLiteral("run_id")).toString());
        QVERIFY(QDateTime::fromString(drop.value(QStringLiteral("checked_at_server_time")).toString(), Qt::ISODate)
                >= QDateTime::fromString(tail.value(QStringLiteral("ended_at_server_time")).toString(), Qt::ISODate));
        QCOMPARE(drop.value(QStringLiteral("server_ms")).toInt(), 40);
        QVERIFY(!drop.value(QStringLiteral("rig_port_open")).toBool());
    }

    // ---------------------------------------------------------------- AC5: through SeatHubClient

    void aSimulatedStreamEmitsMinuteRowsAndABadEndWritesTheTailAndTheDropCheckBeforeTheJournalCloses()
    {
        CountingListener rig;
        QVERIFY(rig.listening());

        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.credentialStore()->setDirectory(m_dir->path());
            QTemporaryDir backup;
            client.credentialStore()->setBackupRoot(backup.path());
            client.setEndOutboxDirectory(m_dir->path() + QStringLiteral("/end-outbox"));
            client.setEvidenceJournalDirectory(journalDir());
            client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
            ControlPlaneClient* planeClient = client.controlPlane();
            auto arm = [this, planeClient]() {
                m_plane = new MiniPlane(planeClient);
                planeClient->setNetworkAccessManager(m_plane);
            };
            // Same discipline as the facade suite: a blocking call onto the plane's own thread
            // would deadlock while the plane still lives on this one.
            if (planeClient->thread() == QThread::currentThread()) {
                arm();
            } else {
                QMetaObject::invokeMethod(planeClient, arm, Qt::BlockingQueuedConnection);
            }
            QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                         QString::fromLatin1(kAccessToken)));
            client.restoreSession();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

            client.setSamplerWindowMsForTests(40);
            client.setStreamRecordMinuteMsForTests(40);
            client.setRigEndpointForTests(QStringLiteral("127.0.0.1"), rig.port());
            client.session()->attachSession(engine);
            client.beginSession(QStringLiteral("s-live"));
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            // Samples reach the recorder from the sampler's own stats tap, overlay or no overlay.
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 400) {
                SDL_Surface* surface = OsdCompositor::rasterize(
                    Overlay::OverlayDebug,
                    "Rendering frame rate: 59.00 FPS\nAverage network latency: 23 ms (variance: 1 ms)", true,
                    SDL_Color{255, 255, 255, 255}, &client.hud()->compositor());
                if (surface) SDL_FreeSurface(surface);
                QTest::qWait(10);
            }
            QTRY_VERIFY_WITH_TIMEOUT(
                entriesOfKind(readCapsules(journalDir()), QStringLiteral("stream_minute")).size() >= 2, 10000);

            // A bad end: the engine fails mid-stream and the Play is torn down.
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), -1, QString());
            emit engine->readyForDeletion();

            QTRY_VERIFY_WITH_TIMEOUT(
                !entriesOfKind(readCapsules(journalDir()), QStringLiteral("drop_check")).isEmpty(), 15000);
            const QList<Capsule> capsules = readCapsules(journalDir());
            QCOMPARE(entriesOfKind(capsules, QStringLiteral("stream_tail")).size(), 1);
            QCOMPARE(entriesOfKind(capsules, QStringLiteral("drop_check")).size(), 1);

            // Same capsule sequence: the tail and the drop check carry the run of the minutes.
            const QJsonObject tail = entriesOfKind(capsules, QStringLiteral("stream_tail")).first();
            const QJsonObject drop = entriesOfKind(capsules, QStringLiteral("drop_check")).first();
            const QList<QJsonObject> minutes = entriesOfKind(capsules, QStringLiteral("stream_minute"));
            QCOMPARE(drop.value(QStringLiteral("run_id")).toString(), tail.value(QStringLiteral("run_id")).toString());
            QCOMPARE(minutes.last().value(QStringLiteral("run_id")).toString(),
                     tail.value(QStringLiteral("run_id")).toString());
            QVERIFY(tail.value(QStringLiteral("seq")).toInt() > minutes.last().value(QStringLiteral("seq")).toInt());
            QVERIFY(drop.value(QStringLiteral("seq")).toInt() > tail.value(QStringLiteral("seq")).toInt());
            QCOMPARE(tail.value(QStringLiteral("end_reason")).toString(), QStringLiteral("engine_failure"));
            QVERIFY(drop.value(QStringLiteral("server_reachable")).toBool());
            QVERIFY(drop.value(QStringLiteral("rig_port_open")).toBool());

            // Exactly one health request and one connect-and-close, no retry, no reconnect.
            QTest::qWait(300);
            QCOMPARE(m_plane->count(QStringLiteral("/health")), 1);
            QCOMPARE(rig.accepted(), 1);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void anOrdinaryEndWritesTheLastMinuteButNoTailAndNoDropCheck()
    {
        CountingListener rig;
        QVERIFY(rig.listening());

        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.credentialStore()->setDirectory(m_dir->path());
            QTemporaryDir backup;
            client.credentialStore()->setBackupRoot(backup.path());
            client.setEndOutboxDirectory(m_dir->path() + QStringLiteral("/end-outbox"));
            client.setEvidenceJournalDirectory(journalDir());
            client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
            ControlPlaneClient* planeClient = client.controlPlane();
            auto arm = [this, planeClient]() {
                m_plane = new MiniPlane(planeClient);
                planeClient->setNetworkAccessManager(m_plane);
            };
            // Same discipline as the facade suite: a blocking call onto the plane's own thread
            // would deadlock while the plane still lives on this one.
            if (planeClient->thread() == QThread::currentThread()) {
                arm();
            } else {
                QMetaObject::invokeMethod(planeClient, arm, Qt::BlockingQueuedConnection);
            }
            QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                         QString::fromLatin1(kAccessToken)));
            client.restoreSession();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

            client.setSamplerWindowMsForTests(40);
            client.setStreamRecordMinuteMsForTests(40);
            client.setRigEndpointForTests(QStringLiteral("127.0.0.1"), rig.port());
            client.session()->attachSession(engine);
            client.beginSession(QStringLiteral("s-calm"));
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));
            QTest::qWait(150);

            emit engine->sessionFinished(0);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

            const QList<Capsule> capsules = readCapsules(journalDir());
            QVERIFY(!entriesOfKind(capsules, QStringLiteral("stream_minute")).isEmpty());
            QVERIFY(entriesOfKind(capsules, QStringLiteral("stream_tail")).isEmpty());
            QVERIFY(entriesOfKind(capsules, QStringLiteral("drop_check")).isEmpty());
            QCOMPARE(m_plane->count(QStringLiteral("/health")), 0);
            QCOMPARE(rig.accepted(), 0);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }
};

QTEST_MAIN(TstStreamRecord)
#include "tst_stream_record.moc"

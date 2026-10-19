/*****************************************************************************
 * SeatHub fork - the evidence journal (ADR-0105, owner rulings (f) and (g) of 2026-10-17).
 *
 * A per-Play, closed-value, on-disk capsule: written only while a Play is open, replayable
 * idempotently, capped by size only (2 MiB folder, 256 entries per Play), reduced then dropped
 * oldest-first with every loss counted, answered per ADR-0105 section 2, and free of every
 * privacy sentinel (address, Wi-Fi name, path, PIN, salt, token, hash). Qt Core only: builds and
 * runs on Linux through tests/linux/run-core-suites.sh.
 *
 * Build recipe: see tst_evidence_journal.pro.
 *****************************************************************************/

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>

#include "seathub/evidence_journal.h"

namespace {

QStringList g_logLines;

void captureMessages(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_logLines.append(message);
}

QJsonObject parseBody(const QByteArray& body)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
}

QJsonObject outboxEntry(const QString& kind = QStringLiteral("end_outbox"))
{
    return EvidenceJournal::outbox(kind, QStringLiteral("put"), QStringLiteral("none"), 1);
}

QJsonObject pairingFailure()
{
    return EvidenceJournal::pairingAttempt(1, false, QStringLiteral("pair_handshake"),
                                           QStringLiteral("pin_rejected"), true,
                                           QStringLiteral("failure"), true, 4200);
}

QStringList capsuleFiles(const QString& dir)
{
    return QDir(dir).entryList(QStringList{QStringLiteral("evj-*.json")}, QDir::Files, QDir::Name);
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QDateTime utc(const QString& iso)
{
    return QDateTime::fromString(iso, Qt::ISODate);
}

QString allFilesText(const QString& dir)
{
    QString text;
    const QStringList names = QDir(dir).entryList(QDir::Files);
    for (const QString& name : names) {
        text += QString::fromUtf8(readAll(QDir(dir).filePath(name)));
    }
    return text;
}

} // namespace

class TstEvidenceJournal : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tmp;
    QString m_dir;

    // The fixed ids used throughout. Opaque ids, as the control plane's UUIDs are.
    const QString kAcct1 = QStringLiteral("acct-1");
    const QString kAcct2 = QStringLiteral("acct-2");
    const QString kSessA = QStringLiteral("sess-a");
    const QString kSessB = QStringLiteral("sess-b");
    const QString kSessC = QStringLiteral("sess-c");

    void fillPlay(EvidenceJournal& journal, const QString& session, const QString& account,
                  int outboxEntries = 1)
    {
        QVERIFY(journal.open(session, account));
        for (int i = 0; i < outboxEntries; ++i) {
            journal.record(outboxEntry());
        }
        journal.record(pairingFailure());
        journal.close();
    }

private slots:
    void init()
    {
        m_tmp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tmp->isValid());
        m_dir = QDir(m_tmp->path()).filePath(QStringLiteral("evidence-journal"));
        g_logLines.clear();
    }

    void cleanup() { m_tmp.reset(); }

    // ------------------------------------------------------------------ the recording window

    void aClosedJournalRecordsNothingAndWritesNothing()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(!journal.isOpen());
        journal.record(outboxEntry());
        journal.record(pairingFailure());
        journal.record(QJsonObject());
        QVERIFY(!QFileInfo::exists(m_dir)); // not even the folder
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
        QCOMPARE(journal.pendingCounters().droppedEntries, qint64(0));

        // After a Play closes, recording stops again.
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        journal.close();
        QVERIFY(!journal.isOpen());
        journal.record(outboxEntry());
        journal.record(outboxEntry());
        QCOMPARE(journal.deliverable(kAcct1).size(), 1);
        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 1);
        QCOMPARE(journal.pendingCounters().droppedEntries, qint64(0));
    }

    void openingWithoutRecordingLeavesNoFile()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        QVERIFY(journal.isOpen());
        journal.close();
        QVERIFY(capsuleFiles(m_dir).isEmpty());
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
    }

    void openRecordCloseWritesOneCapsulePerPlay()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        QVERIFY(journal.isOpen());
        journal.record(outboxEntry());
        QCOMPARE(capsuleFiles(m_dir).size(), 1); // disk first: written at the event, not at close
        journal.record(pairingFailure());
        journal.record(EvidenceJournal::stallWindow(2, 1500));
        QCOMPARE(capsuleFiles(m_dir).size(), 1);
        journal.close();

        const auto pending = journal.deliverable(kAcct1);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().sessionId, kSessA);
        const QJsonObject body = parseBody(pending.first().body);

        QStringList keys = body.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{QStringLiteral("counters"), QStringLiteral("entries"),
                                    QStringLiteral("reduced"), QStringLiteral("run_id")}));
        const QString runId = body.value(QStringLiteral("run_id")).toString();
        QCOMPARE(runId, pending.first().runId);
        QVERIFY(!QUuid::fromString(runId).isNull());
        QVERIFY(!runId.contains(QLatin1Char('{')));
        QCOMPARE(body.value(QStringLiteral("reduced")).toBool(true), false);

        const QJsonObject counters = body.value(QStringLiteral("counters")).toObject();
        QCOMPARE(counters.size(), 4);
        for (const QString& name : {QStringLiteral("reduced_capsules"), QStringLiteral("dropped_capsules"),
                                    QStringLiteral("unreadable_files"), QStringLiteral("dropped_entries")}) {
            QVERIFY2(counters.contains(name), qPrintable(name));
            QCOMPARE(counters.value(name).toInteger(-1), qint64(0));
        }

        const QJsonArray entries = body.value(QStringLiteral("entries")).toArray();
        QCOMPARE(entries.size(), 3);
        qint64 lastElapsed = -1;
        const QRegularExpression rfc3339(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z$"));
        for (int i = 0; i < entries.size(); ++i) {
            const QJsonObject e = entries.at(i).toObject();
            QCOMPARE(e.value(QStringLiteral("seq")).toInteger(-1), qint64(i));
            QVERIFY(rfc3339.match(e.value(QStringLiteral("at")).toString()).hasMatch());
            const qint64 elapsed = e.value(QStringLiteral("elapsed_ms")).toInteger(-1);
            QVERIFY(elapsed >= 0);
            QVERIFY(elapsed >= lastElapsed); // monotonic within the process
            lastElapsed = elapsed;
        }
    }

    void eachPlayHasItsOwnRunIdAndOpeningAgainClosesThePrevious()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        QVERIFY(journal.open(kSessB, kAcct1)); // without a close
        journal.record(outboxEntry());
        journal.close();

        const auto pending = journal.deliverable(kAcct1);
        QCOMPARE(pending.size(), 2);
        QCOMPARE(pending.at(0).sessionId, kSessA); // oldest first
        QCOMPARE(pending.at(1).sessionId, kSessB);
        QVERIFY(pending.at(0).runId != pending.at(1).runId);
    }

    void onlyOpaqueIdsOpenACapsule_data()
    {
        QTest::addColumn<QString>("session");
        QTest::addColumn<QString>("account");
        QTest::addColumn<bool>("expected");
        QTest::newRow("uuids") << QStringLiteral("0b8c1a7e-6f5d-4c1a-9a53-0c2f1d3e4b5a")
                               << QStringLiteral("5d1c2f3e-1111-4a2b-8c3d-9e8f7a6b5c4d") << true;
        QTest::newRow("short ids") << QStringLiteral("s-live") << QStringLiteral("acct_1") << true;
        QTest::newRow("empty session") << QString() << QStringLiteral("acct-1") << false;
        QTest::newRow("empty account") << QStringLiteral("sess-a") << QString() << false;
        QTest::newRow("ipv4 as session") << QStringLiteral("203.0.113.7") << QStringLiteral("acct-1") << false;
        QTest::newRow("ipv6 as account") << QStringLiteral("sess-a") << QStringLiteral("2001:db8::1") << false;
        QTest::newRow("path") << QStringLiteral("a/b") << QStringLiteral("acct-1") << false;
        QTest::newRow("windows path") << QStringLiteral("C:\\Users\\bob") << QStringLiteral("acct-1") << false;
        QTest::newRow("email") << QStringLiteral("sess-a") << QStringLiteral("bob@example.com") << false;
        QTest::newRow("ssid with a space") << QStringLiteral("My Home WiFi") << QStringLiteral("acct-1") << false;
        QTest::newRow("65 characters") << QString(65, QLatin1Char('a')) << QStringLiteral("acct-1") << false;
    }

    void onlyOpaqueIdsOpenACapsule()
    {
        QFETCH(QString, session);
        QFETCH(QString, account);
        QFETCH(bool, expected);
        EvidenceJournal journal(m_dir);
        QCOMPARE(journal.open(session, account), expected);
        QCOMPARE(journal.isOpen(), expected);
    }

    void theDefaultDirectoryIsTheEvidenceJournalFolder()
    {
        QVERIFY(EvidenceJournal::defaultDirectory().endsWith(QStringLiteral("evidence-journal")));
        EvidenceJournal journal;
        QCOMPARE(journal.directory(), EvidenceJournal::defaultDirectory());
    }

    // ------------------------------------------------------------------ persistence and replay

    void aCapsuleSurvivesAReopenAndKeepsItsRunIdAndSeq()
    {
        QByteArray first;
        {
            EvidenceJournal journal(m_dir);
            fillPlay(journal, kSessA, kAcct1, 2);
            first = journal.deliverable(kAcct1).first().body;
        }
        EvidenceJournal again(m_dir); // a new process, in effect
        const auto pending = again.deliverable(kAcct1);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().body, first);
        QCOMPARE(pending.first().sessionId, kSessA);
    }

    void aReplayCarriesTheSameBytesUntilTheServerHasIt()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1, 2);
        const auto one = journal.deliverable(kAcct1);
        const auto two = journal.deliverable(kAcct1);
        QCOMPARE(one.size(), 1);
        QCOMPARE(two.first().body, one.first().body);

        // A failed attempt does not change what is replayed.
        for (int status : {-1, 0, 401, 403, 500, 503}) {
            journal.applyAnswer(kSessA, one.first().runId, status);
            const auto again = journal.deliverable(kAcct1);
            QCOMPARE(again.size(), 1);
            QCOMPARE(again.first().body, one.first().body);
        }

        // seq is unique within the run_id.
        QSet<qint64> seqs;
        for (const QJsonValue& e : parseBody(one.first().body).value(QStringLiteral("entries")).toArray()) {
            seqs.insert(e.toObject().value(QStringLiteral("seq")).toInteger(-1));
        }
        QCOMPARE(seqs.size(), 3);
    }

    void anEntryIsExactlyItsKindsFieldsAndNothingElse()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(EvidenceJournal::pairingAttempt(2, true, QStringLiteral("pair_handshake"),
                                                       QString(), false, QString(), true, 1800));
        journal.record(pairingFailure());
        journal.record(EvidenceJournal::portTest(classifyPortTest(4u, 0x0204u, 0xFFFFFFFFu)));
        journal.record(EvidenceJournal::portTest(classifyPortTest(4u, 0u, 0xFFFFFFFFu)));
        journal.record(EvidenceJournal::portTest(classifyPortTest(4u, 0xFFFFFFFFu, 0xFFFFFFFFu)));
        journal.record(EvidenceJournal::outbox(QStringLiteral("end_outbox"), QStringLiteral("delivered"),
                                               QStringLiteral("2xx"), 3));
        journal.record(EvidenceJournal::outbox(QStringLiteral("quality_outbox"), QStringLiteral("kept"),
                                               QStringLiteral("5xx"), 1));
        journal.record(EvidenceJournal::stallWindow(4, 2500));
        journal.record(EvidenceJournal::networkChange(QStringLiteral("wifi"), QStringLiteral("online"),
                                                      false, true, 2));
        journal.record(EvidenceJournal::engineFailure(QStringLiteral("engine_connect"),
                                                      QStringLiteral("rtsp_handshake"), -1));
        journal.record(EvidenceJournal::clockSkew(
            utc(QStringLiteral("2026-10-17T12:00:00Z")), utc(QStringLiteral("2026-10-17T12:00:09Z"))));
        journal.close();

        const QJsonArray entries = parseBody(journal.deliverable(kAcct1).first().body)
                                       .value(QStringLiteral("entries")).toArray();
        QCOMPARE(entries.size(), 11);

        auto without = [](QJsonObject e) {
            e.remove(QStringLiteral("seq"));
            e.remove(QStringLiteral("at"));
            e.remove(QStringLiteral("elapsed_ms"));
            return e;
        };
        auto obj = [](std::initializer_list<std::pair<QString, QJsonValue>> fields) {
            QJsonObject o;
            for (const auto& f : fields) {
                o.insert(f.first, f.second);
            }
            return o;
        };

        QCOMPARE(without(entries.at(0).toObject()),
                 obj({{"kind", "pairing_attempt"}, {"attempt", 2}, {"outcome", "success"},
                      {"engine_step", "pair_handshake"}, {"engine_class", "ok"}, {"retried", false},
                      {"pin_shape_ok", true}, {"handshake_ms", 1800}}));
        QCOMPARE(without(entries.at(1).toObject()),
                 obj({{"kind", "pairing_attempt"}, {"attempt", 1}, {"outcome", "failure"},
                      {"engine_step", "pair_handshake"}, {"engine_class", "pin_rejected"},
                      {"retried", true}, {"retry_reason", "failure"}, {"pin_shape_ok", true},
                      {"handshake_ms", 4200}}));
        QCOMPARE(without(entries.at(2).toObject()),
                 obj({{"kind", "port_test"}, {"outcome", "failed"}, {"failed_mask", 516}}));
        QCOMPARE(without(entries.at(3).toObject()),
                 obj({{"kind", "port_test"}, {"outcome", "clean"}}));
        QCOMPARE(without(entries.at(4).toObject()),
                 obj({{"kind", "port_test"}, {"outcome", "inconclusive"}}));
        QCOMPARE(without(entries.at(5).toObject()),
                 obj({{"kind", "end_outbox"}, {"event", "delivered"}, {"answer", "2xx"}, {"count", 3}}));
        QCOMPARE(without(entries.at(6).toObject()),
                 obj({{"kind", "quality_outbox"}, {"event", "kept"}, {"answer", "5xx"}, {"count", 1}}));
        QCOMPARE(without(entries.at(7).toObject()),
                 obj({{"kind", "stall_window"}, {"windows", 4}, {"duration_ms", 2500}}));
        QCOMPARE(without(entries.at(8).toObject()),
                 obj({{"kind", "network_change"}, {"medium", "wifi"}, {"reachability", "online"},
                      {"captive_portal", false}, {"metered", true}, {"transitions", 2}}));
        QCOMPARE(without(entries.at(9).toObject()),
                 obj({{"kind", "engine_failure"}, {"step", "engine_connect"},
                      {"engine_class", "rtsp_handshake"}, {"engine_code", -1}}));
        QCOMPARE(without(entries.at(10).toObject()),
                 obj({{"kind", "clock_skew"}, {"bucket", "client_ahead"}}));
    }

    void theClockSkewBucketsUseTheFiveSecondBound()
    {
        const QDateTime server = utc(QStringLiteral("2026-10-17T12:00:00Z"));
        auto bucket = [&](qint64 skewMs) {
            return EvidenceJournal::clockSkew(server, server.addMSecs(skewMs))
                .value(QStringLiteral("bucket")).toString();
        };
        QCOMPARE(bucket(0), QStringLiteral("within_5s"));
        QCOMPARE(bucket(5000), QStringLiteral("within_5s"));
        QCOMPARE(bucket(-5000), QStringLiteral("within_5s"));
        QCOMPARE(bucket(5001), QStringLiteral("client_ahead"));
        QCOMPARE(bucket(-5001), QStringLiteral("client_behind"));
        QCOMPARE(EvidenceJournal::clockSkew(QDateTime(), server).value(QStringLiteral("bucket")).toString(),
                 QStringLiteral("unknown"));
        QCOMPARE(EvidenceJournal::clockSkew(server, QDateTime()).value(QStringLiteral("bucket")).toString(),
                 QStringLiteral("unknown"));
    }

    void aNotRunPortTestBuildsNoEntry()
    {
        QVERIFY(EvidenceJournal::portTest(classifyPortTest(0u, 0u, 0xFFFFFFFFu)).isEmpty());
        QVERIFY(EvidenceJournal::portTest(PortTestResult()).isEmpty());
        PortTestResult bad;
        bad.outcome = PortTestResult::Outcome::Failed;
        bad.failedMask = 0; // a failed outcome needs a positive mask
        QVERIFY(EvidenceJournal::portTest(bad).isEmpty());
        bad.failedMask = 0xFFFFFFFFu; // the sentinel is not a mask
        QVERIFY(EvidenceJournal::portTest(bad).isEmpty());
    }

    // ------------------------------------------------------------------ closed vocabulary

    void buildersRefuseEveryValueOutsideTheirClosedSet_data()
    {
        QTest::addColumn<QJsonObject>("entry");
        const QString ph = QStringLiteral("pair_handshake");
        QTest::newRow("unknown step") << EvidenceJournal::pairingAttempt(1, false, QStringLiteral("pair_handshake_x"), QStringLiteral("failed"), false, QString(), true, 1);
        QTest::newRow("class not of the step") << EvidenceJournal::pairingAttempt(1, false, ph, QStringLiteral("no_app"), false, QString(), true, 1);
        QTest::newRow("empty failure class") << EvidenceJournal::pairingAttempt(1, false, ph, QString(), false, QString(), true, 1);
        QTest::newRow("free-text class") << EvidenceJournal::pairingAttempt(1, false, ph, QStringLiteral("Incorrect PIN"), false, QString(), true, 1);
        QTest::newRow("success with a failure class") << EvidenceJournal::pairingAttempt(2, true, ph, QStringLiteral("failed"), false, QString(), true, 1);
        QTest::newRow("attempt 3") << EvidenceJournal::pairingAttempt(3, false, ph, QStringLiteral("failed"), false, QString(), true, 1);
        QTest::newRow("attempt 0") << EvidenceJournal::pairingAttempt(0, false, ph, QStringLiteral("failed"), false, QString(), true, 1);
        QTest::newRow("unknown retry reason") << EvidenceJournal::pairingAttempt(1, false, ph, QStringLiteral("failed"), true, QStringLiteral("because"), true, 1);
        QTest::newRow("reason without a retry") << EvidenceJournal::pairingAttempt(1, false, ph, QStringLiteral("failed"), false, QStringLiteral("conflict"), true, 1);
        QTest::newRow("negative duration") << EvidenceJournal::pairingAttempt(1, false, ph, QStringLiteral("failed"), false, QString(), true, -1);
        QTest::newRow("not an outbox") << EvidenceJournal::outbox(QStringLiteral("stall_window"), QStringLiteral("put"), QStringLiteral("none"), 1);
        QTest::newRow("outbox event") << EvidenceJournal::outbox(QStringLiteral("end_outbox"), QStringLiteral("sent"), QStringLiteral("none"), 1);
        QTest::newRow("outbox answer") << EvidenceJournal::outbox(QStringLiteral("end_outbox"), QStringLiteral("put"), QStringLiteral("418"), 1);
        QTest::newRow("outbox count 0") << EvidenceJournal::outbox(QStringLiteral("end_outbox"), QStringLiteral("put"), QStringLiteral("none"), 0);
        QTest::newRow("stall 0 windows") << EvidenceJournal::stallWindow(0, 10);
        QTest::newRow("stall negative duration") << EvidenceJournal::stallWindow(1, -1);
        QTest::newRow("medium") << EvidenceJournal::networkChange(QStringLiteral("MyHomeWiFi"), QStringLiteral("online"), false, false, 1);
        QTest::newRow("reachability") << EvidenceJournal::networkChange(QStringLiteral("wifi"), QStringLiteral("fast"), false, false, 1);
        QTest::newRow("transitions 0") << EvidenceJournal::networkChange(QStringLiteral("wifi"), QStringLiteral("online"), false, false, 0);
        QTest::newRow("engine step") << EvidenceJournal::engineFailure(QStringLiteral("pairing"), QStringLiteral("failed"), 1);
        QTest::newRow("engine class") << EvidenceJournal::engineFailure(QStringLiteral("engine_connect"), QStringLiteral("boom"), 1);
        QTest::newRow("engine empty class") << EvidenceJournal::engineFailure(QStringLiteral("engine_connect"), QString(), 1);
    }

    void buildersRefuseEveryValueOutsideTheirClosedSet()
    {
        QFETCH(QJsonObject, entry);
        QVERIFY(entry.isEmpty());
    }

    void anInvalidEntryStoresNothingAndCountsADroppedEntry()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));

        QJsonObject freeText = outboxEntry();
        freeText.insert(QStringLiteral("event"), QStringLiteral("the connection dropped at home"));
        QJsonObject extraField = outboxEntry();
        extraField.insert(QStringLiteral("note"), QStringLiteral("hello"));
        QJsonObject unknownKind;
        unknownKind.insert(QStringLiteral("kind"), QStringLiteral("free_text"));
        QJsonObject missingField = outboxEntry();
        missingField.remove(QStringLiteral("count"));
        QJsonObject wrongType = outboxEntry();
        wrongType.insert(QStringLiteral("count"), QStringLiteral("1"));
        QJsonObject presetEnvelope = outboxEntry();
        presetEnvelope.insert(QStringLiteral("seq"), 99);
        QJsonObject badFlavour = EvidenceJournal::pairingAttempt(1, false, QStringLiteral("pair_handshake"),
                                                                 QStringLiteral("failed"), false, QString(), true, 1);
        badFlavour.insert(QStringLiteral("engine_class"), QStringLiteral("Sentence with spaces"));

        const QList<QJsonObject> bad{QJsonObject(), freeText, extraField, unknownKind, missingField,
                                     wrongType, presetEnvelope, badFlavour};
        for (const QJsonObject& entry : bad) {
            journal.record(entry);
        }
        QCOMPARE(journal.pendingCounters().droppedEntries, qint64(bad.size()));
        QVERIFY(capsuleFiles(m_dir).isEmpty());

        // The drops ride the next delivered capsule.
        journal.record(outboxEntry());
        journal.close();
        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        QCOMPARE(body.value(QStringLiteral("counters")).toObject().value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(bad.size()));
        QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 1);
        QCOMPARE(journal.pendingCounters().droppedEntries, qint64(0));
    }

    // ------------------------------------------------------------------ the 256-entry cap

    void entriesPastTheEntryCapAreCountedNotRecorded()
    {
        EvidenceJournal journal(m_dir);
        journal.setEntryCap(3);
        QVERIFY(journal.open(kSessA, kAcct1));
        for (int i = 0; i < 5; ++i) {
            journal.record(outboxEntry());
        }
        journal.close();
        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 3);
        QCOMPARE(body.value(QStringLiteral("counters")).toObject().value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(2));
    }

    void theDefaultEntryCapIs256AndNeverMore()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        for (int i = 0; i < 300; ++i) {
            journal.record(outboxEntry());
        }
        journal.close();
        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 256);
        QCOMPARE(body.value(QStringLiteral("counters")).toObject().value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(44));

        // A cap above 256 is clamped: the server rejects a longer capsule.
        EvidenceJournal other(QDir(m_tmp->path()).filePath(QStringLiteral("other")));
        other.setEntryCap(1000);
        QVERIFY(other.open(kSessB, kAcct1));
        for (int i = 0; i < 300; ++i) {
            other.record(outboxEntry());
        }
        QCOMPARE(parseBody(other.deliverable(kAcct1).first().body).value(QStringLiteral("entries")).toArray().size(), 256);
    }

    void theEntryCapHoldsForThePlayAcrossADeliveryInTheMiddle()
    {
        EvidenceJournal journal(m_dir);
        journal.setEntryCap(4);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        journal.record(outboxEntry());
        const auto first = journal.deliverable(kAcct1);
        journal.applyAnswer(kSessA, first.first().runId, 200);
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
        for (int i = 0; i < 5; ++i) {
            journal.record(outboxEntry());
        }
        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        // seq continues (2, 3); the cap is per Play, so the rest are counted.
        const QJsonArray entries = body.value(QStringLiteral("entries")).toArray();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).toObject().value(QStringLiteral("seq")).toInteger(), qint64(2));
        QCOMPARE(entries.at(1).toObject().value(QStringLiteral("seq")).toInteger(), qint64(3));
        QCOMPARE(body.value(QStringLiteral("run_id")).toString(), first.first().runId);
        QCOMPARE(body.value(QStringLiteral("counters")).toObject().value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(3));
    }

    // ------------------------------------------------------------------ the folder cap

    void overTheFolderCapTheOldestCapsuleIsReducedToItsSummary()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());                          // seq 0 end_outbox
        journal.record(outboxEntry());                          // seq 1 end_outbox
        journal.record(EvidenceJournal::stallWindow(1, 700));   // seq 2 first failure entry
        journal.record(outboxEntry(QStringLiteral("quality_outbox"))); // seq 3 first quality_outbox
        journal.record(pairingFailure());                       // seq 4 a later failure, first pairing
        journal.record(EvidenceJournal::stallWindow(2, 900));   // seq 5 not in the summary
        journal.record(outboxEntry());                          // seq 6
        journal.close();
        const qint64 sizeA = QFileInfo(QDir(m_dir).filePath(capsuleFiles(m_dir).first())).size();

        journal.setDirectoryCapBytes(sizeA + sizeA / 2);
        QVERIFY(journal.open(kSessB, kAcct1));
        for (int i = 0; i < 40 && journal.pendingCounters().reducedCapsules == 0; ++i) {
            journal.record(outboxEntry());
        }
        QCOMPARE(journal.pendingCounters().reducedCapsules, qint64(1));
        QCOMPARE(journal.pendingCounters().droppedCapsules, qint64(0));
        journal.close();

        const auto pending = journal.deliverable(kAcct1);
        QCOMPARE(pending.size(), 2);
        const QJsonObject reduced = parseBody(pending.at(0).body);
        QCOMPARE(pending.at(0).sessionId, kSessA);
        QCOMPARE(reduced.value(QStringLiteral("reduced")).toBool(false), true);
        const QJsonArray kept = reduced.value(QStringLiteral("entries")).toArray();
        QList<qint64> seqs;
        for (const QJsonValue& v : kept) {
            seqs.append(v.toObject().value(QStringLiteral("seq")).toInteger(-1));
        }
        // The first failure entry (stall, seq 2), then the first entry of every other kind.
        QCOMPARE(seqs, (QList<qint64>{0, 2, 3, 4}));

        const QJsonObject untouched = parseBody(pending.at(1).body);
        QCOMPARE(pending.at(1).sessionId, kSessB);
        QCOMPARE(untouched.value(QStringLiteral("reduced")).toBool(true), false);

        // The reduction was counted and rides the first delivered capsule only.
        QCOMPARE(reduced.value(QStringLiteral("counters")).toObject().value(QStringLiteral("reduced_capsules")).toInteger(),
                 qint64(1));
        QCOMPARE(untouched.value(QStringLiteral("counters")).toObject().value(QStringLiteral("reduced_capsules")).toInteger(),
                 qint64(0));
        QCOMPARE(journal.pendingCounters().reducedCapsules, qint64(0));

        // A replay carries the same numbers.
        QCOMPARE(journal.deliverable(kAcct1).first().body, pending.first().body);
    }

    void aSummaryWithoutAnyFailureKeepsTheFirstEntryOfEachKind()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        journal.record(outboxEntry());
        journal.record(EvidenceJournal::networkChange(QStringLiteral("wifi"), QStringLiteral("online"), false, false, 1));
        journal.record(outboxEntry());
        journal.close();
        journal.setDirectoryCapBytes(1);
        QVERIFY(journal.open(kSessB, kAcct1)); // enforcement runs; the one closed capsule is reduced then dropped
        journal.close();
        QCOMPARE(journal.pendingCounters().reducedCapsules, qint64(1));
        QCOMPARE(journal.pendingCounters().droppedCapsules, qint64(1));
        QVERIFY(capsuleFiles(m_dir).isEmpty());
    }

    void onlyWhenSummariesAloneExceedTheCapIsTheOldestSummaryDropped()
    {
        EvidenceJournal journal(m_dir);
        // Three closed, single-entry capsules of equal size: reducing one does not shrink it, so
        // the cap can only be met by dropping.
        fillSingle(journal, kSessA);
        fillSingle(journal, kSessB);
        fillSingle(journal, kSessC);
        const QStringList files = capsuleFiles(m_dir);
        QCOMPARE(files.size(), 3);
        const qint64 one = QFileInfo(QDir(m_dir).filePath(files.last())).size();

        journal.setDirectoryCapBytes(one + one / 2); // room for one capsule, not two
        QVERIFY(journal.open(QStringLiteral("sess-d"), kAcct1));
        journal.close();

        // Every capsule was reduced first (3), then the OLDEST summaries dropped (A, then B).
        QCOMPARE(journal.pendingCounters().reducedCapsules, qint64(3));
        QCOMPARE(journal.pendingCounters().droppedCapsules, qint64(2));
        const auto left = journal.deliverable(kAcct1);
        QCOMPARE(left.size(), 1);
        QCOMPARE(left.first().sessionId, kSessC);
        const QJsonObject body = parseBody(left.first().body);
        QCOMPARE(body.value(QStringLiteral("reduced")).toBool(false), true);
        const QJsonObject c = body.value(QStringLiteral("counters")).toObject();
        QCOMPARE(c.value(QStringLiteral("reduced_capsules")).toInteger(), qint64(3));
        QCOMPARE(c.value(QStringLiteral("dropped_capsules")).toInteger(), qint64(2));
    }

    void aDroppedSummaryKeepsItsOwnCounts()
    {
        EvidenceJournal journal(m_dir);
        journal.setEntryCap(1);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        journal.record(outboxEntry()); // over the entry cap: dropped_entries 1, rides A when delivered
        journal.close();
        const auto first = journal.deliverable(kAcct1); // folds the 1 into A's file
        QCOMPARE(parseBody(first.first().body).value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("dropped_entries")).toInteger(), qint64(1));

        journal.setDirectoryCapBytes(1);
        QVERIFY(journal.open(kSessB, kAcct1)); // A is reduced, then dropped
        journal.close();
        QVERIFY(capsuleFiles(m_dir).isEmpty());
        const EvidenceJournal::Counters pending = journal.pendingCounters();
        QCOMPARE(pending.droppedCapsules, qint64(1));
        QCOMPARE(pending.reducedCapsules, qint64(1));
        QCOMPARE(pending.droppedEntries, qint64(1)); // not lost with the dropped file
    }

    void theCapNeverTouchesThePlayThatIsStillOpen()
    {
        EvidenceJournal journal(m_dir);
        journal.setDirectoryCapBytes(1);
        QVERIFY(journal.open(kSessA, kAcct1));
        for (int i = 0; i < 5; ++i) {
            journal.record(outboxEntry());
        }
        QCOMPARE(parseBody(journal.deliverable(kAcct1).first().body).value(QStringLiteral("entries")).toArray().size(), 5);
        QCOMPARE(journal.pendingCounters().droppedCapsules, qint64(0));
        QCOMPARE(journal.pendingCounters().reducedCapsules, qint64(0));
    }

    void countersRideTheNextDeliveredCapsuleExactlyOnce()
    {
        EvidenceJournal journal(m_dir);
        journal.setEntryCap(1);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(outboxEntry());
        journal.record(outboxEntry());
        journal.close();
        const auto first = journal.deliverable(kAcct1);
        QCOMPARE(parseBody(first.first().body).value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("dropped_entries")).toInteger(), qint64(1));
        journal.applyAnswer(kSessA, first.first().runId, 200);

        journal.setEntryCap(256);
        fillPlay(journal, kSessB, kAcct1);
        const QJsonObject next = parseBody(journal.deliverable(kAcct1).first().body);
        QCOMPARE(next.value(QStringLiteral("counters")).toObject().value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(0));
    }

    // ------------------------------------------------------------------ the answer decides the file

    void theAnswerDecidesTheFile_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("fate"); // removed, dropped or kept
        QTest::newRow("200") << 200 << "removed";
        QTest::newRow("201") << 201 << "removed";
        QTest::newRow("204") << 204 << "removed";
        QTest::newRow("404") << 404 << "removed";
        QTest::newRow("400") << 400 << "dropped";
        QTest::newRow("422") << 422 << "dropped";
        QTest::newRow("401") << 401 << "kept";
        QTest::newRow("403") << 403 << "kept";
        QTest::newRow("500") << 500 << "kept";
        QTest::newRow("503") << 503 << "kept";
        QTest::newRow("599") << 599 << "kept";
        QTest::newRow("transport -1") << -1 << "kept";
        QTest::newRow("transport 0") << 0 << "kept";
        QTest::newRow("429") << 429 << "kept";
        QTest::newRow("408") << 408 << "kept";
        QTest::newRow("302") << 302 << "kept";
    }

    void theAnswerDecidesTheFile()
    {
        QFETCH(int, status);
        QFETCH(QString, fate);
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        const auto pending = journal.deliverable(kAcct1);
        QCOMPARE(pending.size(), 1);

        journal.applyAnswer(kSessA, pending.first().runId, status);

        if (fate == QLatin1String("kept")) {
            QCOMPARE(capsuleFiles(m_dir).size(), 1);
            QCOMPARE(journal.deliverable(kAcct1).size(), 1);
            QCOMPARE(journal.pendingCounters().droppedCapsules, qint64(0));
        } else {
            QVERIFY(capsuleFiles(m_dir).isEmpty());
            QVERIFY(journal.deliverable(kAcct1).isEmpty());
            QCOMPARE(journal.pendingCounters().droppedCapsules, fate == QLatin1String("dropped") ? qint64(1) : qint64(0));
        }
    }

    void aDroppedCapsuleCountsRideTheNextCapsule()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        journal.applyAnswer(kSessA, journal.deliverable(kAcct1).first().runId, 422);
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
        fillPlay(journal, kSessB, kAcct1);
        const QJsonObject c = parseBody(journal.deliverable(kAcct1).first().body).value(QStringLiteral("counters")).toObject();
        QCOMPARE(c.value(QStringLiteral("dropped_capsules")).toInteger(), qint64(1));
    }

    void anAnswerForAnotherSessionOrAnUnknownRunChangesNothing()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        const auto pending = journal.deliverable(kAcct1);
        journal.applyAnswer(kSessB, pending.first().runId, 200);          // wrong session
        journal.applyAnswer(kSessA, QUuid::createUuid().toString(QUuid::WithoutBraces), 200); // unknown run
        journal.applyAnswer(kSessA, QStringLiteral("not-a-uuid"), 200);
        journal.applyAnswer(kSessA, QString(), 200);
        QCOMPARE(journal.deliverable(kAcct1).size(), 1);
        // A repeat answer for something already removed is harmless.
        journal.applyAnswer(kSessA, pending.first().runId, 200);
        journal.applyAnswer(kSessA, pending.first().runId, 200);
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
    }

    void entriesRecordedAfterASendSurviveItsAnswer()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(pairingFailure()); // seq 0, sent at the failure-class event
        const auto sent = journal.deliverable(kAcct1);
        journal.record(EvidenceJournal::stallWindow(1, 400)); // seq 1, recorded while in flight
        journal.applyAnswer(kSessA, sent.first().runId, 200);

        const auto rest = journal.deliverable(kAcct1);
        QCOMPARE(rest.size(), 1);
        const QJsonArray entries = parseBody(rest.first().body).value(QStringLiteral("entries")).toArray();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).toObject().value(QStringLiteral("seq")).toInteger(), qint64(1));
        QCOMPARE(rest.first().runId, sent.first().runId);

        journal.close();
        journal.applyAnswer(kSessA, rest.first().runId, 200);
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
        QVERIFY(capsuleFiles(m_dir).isEmpty());
    }

    // ------------------------------------------------------------------ accounts

    void aCapsuleOfAnotherAccountIsKeptAndOnlyHandedToItsOwner()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        fillPlay(journal, kSessB, kAcct2);

        const auto forOne = journal.deliverable(kAcct1);
        const auto forTwo = journal.deliverable(kAcct2);
        QCOMPARE(forOne.size(), 1);
        QCOMPARE(forOne.first().sessionId, kSessA);
        QCOMPARE(forTwo.size(), 1);
        QCOMPARE(forTwo.first().sessionId, kSessB);
        QVERIFY(journal.deliverable(QStringLiteral("acct-3")).isEmpty());

        // Account 2 signs in and its capsule is delivered; account 1's is still on disk.
        journal.applyAnswer(kSessB, forTwo.first().runId, 200);
        QCOMPARE(capsuleFiles(m_dir).size(), 1);
        QCOMPARE(journal.deliverable(kAcct1).size(), 1);

        // Counters never ride another account's capsule.
        EvidenceJournal other(QDir(m_tmp->path()).filePath(QStringLiteral("two")));
        other.setEntryCap(1);
        QVERIFY(other.open(kSessA, kAcct1));
        other.record(outboxEntry());
        other.record(outboxEntry());
        other.close();
        const QJsonObject mine = parseBody(other.deliverable(kAcct2).isEmpty() ? QByteArray()
                                                                              : other.deliverable(kAcct2).first().body);
        QVERIFY(mine.isEmpty());
        QCOMPARE(other.pendingCounters().droppedEntries, qint64(1));
    }

    // ------------------------------------------------------------------ unreadable files

    void anUnreadableFileIsCountedLoggedAndRemoved()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        const QString good = capsuleFiles(m_dir).first();

        const QString badUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString notJson = QStringLiteral("evj-0000000000000-0000-%1.json").arg(badUuid);
        const QString wrongSchema = QStringLiteral("evj-0000000000000-0001-%1.json")
                                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString emptyFile = QStringLiteral("evj-0000000000000-0002-%1.json")
                                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        auto put = [&](const QString& name, const QByteArray& data) {
            QFile f(QDir(m_dir).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        put(notJson, "{ this is not json");
        put(wrongSchema, "{\"run_id\":\"x\",\"entries\":[]}");
        put(emptyFile, QByteArray());

        qInstallMessageHandler(captureMessages);
        const auto pending = journal.deliverable(kAcct1);
        qInstallMessageHandler(nullptr);

        QCOMPARE(pending.size(), 1);
        QVERIFY(!QFileInfo::exists(QDir(m_dir).filePath(notJson)));
        QVERIFY(!QFileInfo::exists(QDir(m_dir).filePath(wrongSchema)));
        QVERIFY(!QFileInfo::exists(QDir(m_dir).filePath(emptyFile)));
        QVERIFY(QFileInfo::exists(QDir(m_dir).filePath(good)));
        const QJsonObject c = parseBody(pending.first().body).value(QStringLiteral("counters")).toObject();
        QCOMPARE(c.value(QStringLiteral("unreadable_files")).toInteger(), qint64(3));

        bool logged = false;
        for (const QString& line : g_logLines) {
            if (line.contains(QStringLiteral("event=unreadable"))) {
                logged = true;
            }
            QVERIFY2(!line.contains(m_dir), qPrintable(line)); // no path in a log line
            QVERIFY2(!line.contains(QStringLiteral("evj-")), qPrintable(line));
        }
        QVERIFY(logged);
    }

    void aCapsuleWithAnOutOfSetValueIsUnreadable()
    {
        EvidenceJournal journal(m_dir);
        fillPlay(journal, kSessA, kAcct1);
        const QString name = capsuleFiles(m_dir).first();
        const QString path = QDir(m_dir).filePath(name);
        QByteArray text = readAll(path);
        QVERIFY(text.contains("pin_rejected"));
        text.replace("pin_rejected", "Incorrect PIN 4821 for 203.0.113.7");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        f.close();
        // Free text read back from disk is not a token: the file is unreadable, never sent.
        QVERIFY(journal.deliverable(kAcct1).isEmpty());
        QVERIFY(capsuleFiles(m_dir).isEmpty());
        QCOMPARE(journal.pendingCounters().unreadableFiles, qint64(1));
    }

    // ------------------------------------------------------------------ logging

    void everyPutDrainDeliveryAndDropWritesALineWithoutAPathOrAnAddress()
    {
        qInstallMessageHandler(captureMessages);
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(pairingFailure());
        const auto pending = journal.deliverable(kAcct1);
        journal.applyAnswer(kSessA, pending.first().runId, 503);
        journal.applyAnswer(kSessA, pending.first().runId, 200);
        fillPlay(journal, kSessB, kAcct1);
        journal.applyAnswer(kSessB, journal.deliverable(kAcct1).first().runId, 400);
        qInstallMessageHandler(nullptr);

        const QString all = g_logLines.join(QLatin1Char('\n'));
        for (const char* event : {"event=put", "event=drain", "event=kept", "event=delivered", "event=dropped"}) {
            QVERIFY2(all.contains(QLatin1String(event)), event);
        }
        QVERIFY(all.contains(QStringLiteral("answer=5xx")));
        QVERIFY(all.contains(QStringLiteral("answer=2xx")));
        QVERIFY(all.contains(QStringLiteral("answer=400")));
        QVERIFY(all.contains(QStringLiteral("session=sess-a")));
        QVERIFY(!all.contains(m_tmp->path()));
        QVERIFY(!all.contains(QStringLiteral("evj-")));
        QVERIFY(!all.contains(QStringLiteral("acct-1"))); // not even the account id
        QRegularExpression address(QStringLiteral("\\d+\\.\\d+\\.\\d+\\.\\d+"));
        QVERIFY(!address.match(all).hasMatch());
    }

    // ------------------------------------------------------------------ privacy

    void noPrivacySentinelReachesACapsuleItsBodyOrALogLine()
    {
        const QStringList sentinels{
            QStringLiteral("203.0.113.77"),                    // IPv4 address
            QStringLiteral("2001:db8:abcd::77"),               // IPv6 address
            QStringLiteral("rig-7.example.net"),               // host name
            QStringLiteral("MyHomeWiFi"),                      // SSID
            QStringLiteral("Intel(R) Wi-Fi 6 AX201"),          // adapter name
            QStringLiteral("C:\\Users\\bob\\AppData\\x.txt"),  // path
            QStringLiteral("/home/bob/.config/seathub"),       // path
            QStringLiteral("bob@example.com"),                 // user name / contact
            QStringLiteral("4821"),                            // PIN
            QStringLiteral("0123456789abcdef0123456789abcdef"), // salt
            QStringLiteral("tok_live_ABCDEF123456"),           // token
            QStringLiteral("e3b0c44298fc1c149afbf4c8996fb924"), // hash
        };

        qInstallMessageHandler(captureMessages);
        EvidenceJournal journal(m_dir);
        // Ids are opaque server ids kept only in the local file and the session-id log line, never
        // in the body. Their character set refuses an address, a host name, a path, an e-mail
        // address and a name with spaces or punctuation (a bare word or number is a legal id).
        for (const QString& s : sentinels) {
            if (QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$")).match(s).hasMatch()) {
                continue;
            }
            QVERIFY2(!journal.open(s, kAcct1), qPrintable(s));
            QVERIFY2(!journal.open(kSessA, s), qPrintable(s));
        }
        QVERIFY(journal.open(kSessA, kAcct1));
        for (const QString& s : sentinels) {
            // Every string-taking builder: refused, in every string position.
            QVERIFY(EvidenceJournal::pairingAttempt(1, false, s, QStringLiteral("failed"), false, QString(), true, 1).isEmpty());
            QVERIFY(EvidenceJournal::pairingAttempt(1, false, QStringLiteral("pair_handshake"), s, false, QString(), true, 1).isEmpty());
            QVERIFY(EvidenceJournal::pairingAttempt(1, false, QStringLiteral("pair_handshake"), QStringLiteral("failed"), true, s, true, 1).isEmpty());
            QVERIFY(EvidenceJournal::outbox(s, QStringLiteral("put"), QStringLiteral("none"), 1).isEmpty());
            QVERIFY(EvidenceJournal::outbox(QStringLiteral("end_outbox"), s, QStringLiteral("none"), 1).isEmpty());
            QVERIFY(EvidenceJournal::outbox(QStringLiteral("end_outbox"), QStringLiteral("put"), s, 1).isEmpty());
            QVERIFY(EvidenceJournal::networkChange(s, QStringLiteral("online"), false, false, 1).isEmpty());
            QVERIFY(EvidenceJournal::networkChange(QStringLiteral("wifi"), s, false, false, 1).isEmpty());
            QVERIFY(EvidenceJournal::engineFailure(s, QStringLiteral("failed"), 1).isEmpty());
            QVERIFY(EvidenceJournal::engineFailure(QStringLiteral("engine_connect"), s, 1).isEmpty());

            // record() of a hand-built entry that carries the sentinel in any field: refused.
            for (const QString& field : {QStringLiteral("event"), QStringLiteral("answer"), QStringLiteral("note"),
                                         QStringLiteral("host"), QStringLiteral("ssid"), QStringLiteral("path"),
                                         QStringLiteral("pin")}) {
                QJsonObject entry = outboxEntry();
                entry.insert(field, s);
                journal.record(entry);
            }
        }
        // And a few valid entries so the capsule exists.
        journal.record(outboxEntry());
        journal.record(pairingFailure());
        journal.record(EvidenceJournal::portTest(classifyPortTest(4u, 0x0004u, 0xFFFFFFFFu)));
        journal.record(EvidenceJournal::networkChange(QStringLiteral("wifi"), QStringLiteral("online"), false, false, 1));
        journal.close();
        const auto pending = journal.deliverable(kAcct1);
        journal.applyAnswer(kSessA, pending.first().runId, 503);
        qInstallMessageHandler(nullptr);

        const QString body = QString::fromUtf8(pending.first().body);
        const QString disk = allFilesText(m_dir);
        const QString logs = g_logLines.join(QLatin1Char('\n'));
        for (const QString& s : sentinels) {
            QVERIFY2(!body.contains(s), qPrintable(s));
            QVERIFY2(!disk.contains(s), qPrintable(s));
            QVERIFY2(!logs.contains(s), qPrintable(s));
        }
        // The body carries no file path, no folder name and no account or session id either.
        QVERIFY(!body.contains(m_tmp->path()));
        QVERIFY(!body.contains(QStringLiteral("evidence-journal")));
        QVERIFY(!body.contains(kAcct1));
        QVERIFY(!body.contains(kSessA));
        QVERIFY(!disk.contains(m_tmp->path()));
        // Every refused hand-built entry was counted, not stored.
        QCOMPARE(parseBody(pending.first().body).value(QStringLiteral("entries")).toArray().size(), 4);
        QCOMPARE(parseBody(pending.first().body).value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("dropped_entries")).toInteger(),
                 qint64(sentinels.size() * 7));
    }

    void everyStringInACapsuleBodyIsATokenARunIdOrATimestamp()
    {
        EvidenceJournal journal(m_dir);
        QVERIFY(journal.open(kSessA, kAcct1));
        journal.record(pairingFailure());
        journal.record(EvidenceJournal::pairingAttempt(2, true, QStringLiteral("pair_handshake"), QString(), false, QString(), true, 10));
        journal.record(EvidenceJournal::portTest(classifyPortTest(4u, 0x4u, 0xFFFFFFFFu)));
        journal.record(outboxEntry());
        journal.record(EvidenceJournal::stallWindow(1, 1));
        journal.record(EvidenceJournal::networkChange(QStringLiteral("cellular"), QStringLiteral("site"), true, true, 1));
        journal.record(EvidenceJournal::engineFailure(QStringLiteral("stream"), QStringLiteral("no_video_frame"), -101));
        journal.record(EvidenceJournal::clockSkew(QDateTime(), QDateTime()));
        journal.close();

        const QJsonObject body = parseBody(journal.deliverable(kAcct1).first().body);
        const QRegularExpression token(QStringLiteral("^[a-z0-9_]{1,32}$"));
        const QRegularExpression timestamp(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T[\\d:.]+Z$"));
        for (const QJsonValue& v : body.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject e = v.toObject();
            for (auto it = e.constBegin(); it != e.constEnd(); ++it) {
                if (it.value().isString()) {
                    const QString s = it.value().toString();
                    const bool ok = (it.key() == QLatin1String("at"))
                        ? timestamp.match(s).hasMatch()
                        : (token.match(s).hasMatch() || s == QLatin1String("2xx") || s == QLatin1String("5xx")
                           || s.at(0).isDigit());
                    QVERIFY2(ok, qPrintable(it.key() + QLatin1Char('=') + s));
                } else {
                    QVERIFY2(it.value().isDouble() || it.value().isBool(), qPrintable(it.key()));
                }
            }
        }
    }

private:
    void fillSingle(EvidenceJournal& journal, const QString& session)
    {
        QVERIFY(journal.open(session, kAcct1));
        journal.record(outboxEntry());
        journal.close();
    }
};

QTEST_MAIN(TstEvidenceJournal)

#include "tst_evidence_journal.moc"

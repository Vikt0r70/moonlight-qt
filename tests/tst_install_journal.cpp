#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include "seathub/install_journal.h"
#include <limits>

class TstInstallJournal : public QObject
{
    Q_OBJECT
    const QDateTime now = QDateTime::fromString("2026-09-30T12:00:00Z", Qt::ISODate);
    const QString id = "0123456789abcdef";
    QJsonObject record(const QString& attempt = "0123456789abcdef") const
    {
        return {{"v", 1}, {"attempt", attempt}, {"from", "0.1.24"}, {"to", "0.1.25"},
                {"mode", "staged"}, {"started", now.addSecs(-3600).toString(Qt::ISODate)},
                {"ended", now.addSecs(-3500).toString(Qt::ISODate)}, {"outcome", "failed"},
                {"step", "swap_aside"}, {"class", "installer.locked_file"}, {"code", 5},
                {"rollback", "not_needed"}, {"state", "old_intact"},
                {"ms", QJsonObject{{"stage", -10}, {"swap", 4000000}, {"total", 10}}},
                {"elevated", true}};
    }
    void put(const QString& folder, const QString& name, const QByteArray& data,
             int ageSeconds = 60) const
    {
        QFile file(QDir(folder).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), qint64(data.size()));
        QVERIFY(file.setFileTime(now.addSecs(-ageSeconds), QFileDevice::FileModificationTime));
    }
    QString name(const QString& attempt = "0123456789abcdef", const QString& kind = "end") const
    { return "attempt-" + attempt + "." + kind + ".json"; }
    QMap<QString, QPair<qint64, QDateTime>> snapshot(const QString& folder) const
    {
        QMap<QString, QPair<qint64, QDateTime>> result;
        for (const auto& info : QDir(folder).entryInfoList(QDir::Files))
            result.insert(info.fileName(), {info.size(), info.lastModified()});
        return result;
    }
private slots:
    void aWellFormedEndFileParsesToTheRecord()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto raw = record();
        raw.insert("private_token", "DO_NOT_SHIP");
        raw.insert("code", 9000000000.0);
        put(dir.path(), name(), QJsonDocument(raw).toJson());
        auto rows = InstallJournal::adopt(dir.path(), now);
        QCOMPARE(rows.size(), 1);
        const auto r = rows.first();
        QCOMPARE(r.attempt, id);
        QCOMPARE(r.mode, QString("staged"));
        QCOMPARE(r.failureClass, QString("installer.locked_file"));
        QCOMPARE(r.code, std::numeric_limits<qint32>::max());
        QCOMPARE(r.ms.value("stage").toInteger(), qint64(0));
        QCOMPARE(r.ms.value("swap").toInteger(), qint64(3600000));
        QVERIFY(!r.toJson().contains("private_token"));
        QCOMPARE(r.toJson().keys(), record().keys());
    }
    void hostileFilesAreRejectedOrClamped_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("mode") << QString("mode") << QJsonValue("purge");
        QTest::newRow("outcome") << QString("outcome") << QJsonValue("oops");
        QTest::newRow("step") << QString("step") << QJsonValue("../secret");
        QTest::newRow("class") << QString("class") << QJsonValue("C:/Users/secret");
        QTest::newRow("rollback") << QString("rollback") << QJsonValue("maybe");
        QTest::newRow("state") << QString("state") << QJsonValue("live");
        QTest::newRow("from") << QString("from") << QJsonValue("https://private");
        QTest::newRow("to") << QString("to") << QJsonValue("../1.2");
        QTest::newRow("id-mismatch") << QString("attempt") << QJsonValue("ffffffffffffffff");
        QTest::newRow("code-type") << QString("code") << QJsonValue("private");
        QTest::newRow("ms-type") << QString("ms") << QJsonValue("private");
        QTest::newRow("date") << QString("started") << QJsonValue("private");
        QTest::newRow("elevated") << QString("elevated") << QJsonValue("yes");
    }
    void hostileFilesAreRejectedOrClamped()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        QTemporaryDir dir; auto raw = record(); raw.insert(field, value);
        put(dir.path(), name(), QJsonDocument(raw).toJson());
        QVERIFY(InstallJournal::adopt(dir.path(), now).isEmpty());
    }
    void boundsAndForgedNames()
    {
        QTemporaryDir dir; const auto raw = QJsonDocument(record()).toJson();
        put(dir.path(), name(), raw, 7 * 86400 + 1);
        QVERIFY(InstallJournal::adopt(dir.path(), now).isEmpty());
        put(dir.path(), name(), QByteArray(4097, ' '));
        QVERIFY(InstallJournal::adopt(dir.path(), now).isEmpty());
        put(dir.path(), name(), "[]");
        QVERIFY(InstallJournal::adopt(dir.path(), now).isEmpty());
        for (const auto& forged : {"attempt-0123456789ABCDEF.end.json",
                                  "attempt-0123.end.json", "attempt-..x.end.json"}) {
            QTemporaryDir isolated;
            put(isolated.path(), forged, raw);
            QVERIFY(InstallJournal::adopt(isolated.path(), now).isEmpty());
        }
    }
    void onlyNewestEightAreRead()
    {
        QTemporaryDir dir;
        for (int i = 0; i < 10; ++i) {
            const auto attempt = QString::number(i, 16).rightJustified(16, '0');
            put(dir.path(), name(attempt), QJsonDocument(record(attempt)).toJson(), 60 + i);
        }
        const auto rows = InstallJournal::adopt(dir.path(), now);
        QCOMPARE(rows.size(), 8);
        for (const auto& r : rows) QVERIFY(r.attempt.toInt(nullptr, 16) < 8);
    }
    void aStartFileWithNoEndAfterThirtyMinutesReadsAsInterrupted()
    {
        QTemporaryDir dir; auto raw = record(); raw.remove("ended");
        put(dir.path(), name(id, "start"), QJsonDocument(raw).toJson(), 1800);
        auto rows = InstallJournal::adopt(dir.path(), now);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().failureClass, QString("installer.interrupted"));
        QCOMPARE(rows.first().outcome, QString("failed"));
        put(dir.path(), name(id, "start"), QJsonDocument(raw).toJson(), 1799);
        QVERIFY(InstallJournal::adopt(dir.path(), now).isEmpty());
        put(dir.path(), name(id, "start"), QJsonDocument(raw).toJson(), 1800);
        put(dir.path(), name(), QJsonDocument(record()).toJson());
        QCOMPARE(InstallJournal::adopt(dir.path(), now).size(), 1);
    }
    void theReaderNeverWritesOrDeletesInTheJournalFolder()
    {
        QTemporaryDir dir;
        put(dir.path(), name(), QJsonDocument(record()).toJson());
        const auto before = snapshot(dir.path());
        QCOMPARE(InstallJournal::adopt(dir.path(), now).size(), 1);
        QCOMPARE(snapshot(dir.path()), before);
    }
};
QTEST_GUILESS_MAIN(TstInstallJournal)
#include "tst_install_journal.moc"

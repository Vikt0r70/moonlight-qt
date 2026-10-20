/*****************************************************************************
 * SeatHub fork - the binary-safe pairing key (T090, ADR-0109).
 *
 * Upstream's pair method built its 16-byte AES key as
 *   QByteArray aesKey = QCryptographicHash::hash(saltedPin, hashAlgo).constData();
 *   aesKey.truncate(16);
 * `QByteArray(const char*)` copies up to the first 0x00 byte, so a digest with a zero among its
 * first sixteen bytes produced a SHORT key, `truncate(16)` cannot lengthen it, and the rig then
 * reported "Incorrect PIN". About 6.1% of pairings (16 / 256) are affected.
 *
 * These tests call `seathub::derivePairingKey`, the very helper the pair method binds `aesKey` to
 * (`tests/tst_d28_boundary` grades that binding in the upstream cpp). The expected keys below were
 * computed independently with Python hashlib, never with the code under test.
 *
 * Every input here is a fixed, public, synthetic fixture: a counter-style salt and the PIN "1234".
 * Nothing is a captured customer credential.
 *
 * Build recipe: see tst_pairing_key.pro
 *****************************************************************************/

#include <QtTest>
#include <QCryptographicHash>

#include <functional>

#include "seathub/pairing_key.h"

namespace {

QByteArray saltedPin(const char* saltHex)
{
    // Mirrors NvPairingManager::saltPin: the raw salt followed by the PIN's Latin-1 bytes.
    return QByteArray::fromHex(saltHex) + QByteArray("1234");
}

/// What the unfixed upstream expression produced: the digest read as a C string, then truncated.
QByteArray oldConstruction(const QByteArray& input, QCryptographicHash::Algorithm algo)
{
    QByteArray key = QCryptographicHash::hash(input, algo).constData();
    key.truncate(16);
    return key;
}

QStringList* g_lines = nullptr;

void captureHandler(QtMsgType, const QMessageLogContext&, const QString& message)
{
    if (g_lines != nullptr) {
        g_lines->append(message);
    }
}

/// The messages `body` logs on this thread. Nests: the enclosing capture resumes afterwards.
QStringList captureLog(const std::function<void()>& body)
{
    QStringList lines;
    QStringList* const outer = g_lines;
    g_lines = &lines;
    const QtMessageHandler previous = qInstallMessageHandler(captureHandler);
    body();
    qInstallMessageHandler(previous);
    g_lines = outer;
    return lines;
}

} // namespace

class TstPairingKey : public QObject
{
    Q_OBJECT

private slots:
    // The public fixed vector: salt 00..04, PIN 1234, SHA-256 digest
    //   f4b8571c3339f352690015b6399fec3a9172df688b788a6c03d80ab23552ae27
    // has a zero at index 9. The old expression keeps only the nine bytes before it.
    void binaryKeyKeepsEmbeddedNul()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000004");
        const QByteArray digest = QCryptographicHash::hash(input, QCryptographicHash::Sha256);
        QCOMPARE(digest.toHex(),
                 QByteArray("f4b8571c3339f352690015b6399fec3a9172df688b788a6c03d80ab23552ae27"));
        QCOMPARE(digest.at(9), '\0');

        // The defect, reproduced from the unfixed expression: nine bytes, not sixteen.
        QCOMPARE(oldConstruction(input, QCryptographicHash::Sha256).size(), 9);

        const QByteArray key = seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        QCOMPARE(key.size(), 16);
        QCOMPARE(key, digest.left(16));
        QCOMPARE(key.toHex(), QByteArray("f4b8571c3339f352690015b6399fec3a"));
    }

    void zeroAtTheFirstByteKeepsSixteenBytes()
    {
        const QByteArray input = saltedPin("000000000000000000000000000000cb");
        QCOMPARE(oldConstruction(input, QCryptographicHash::Sha256).size(), 0);
        const QByteArray key = seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        QCOMPARE(key.size(), 16);
        QCOMPARE(key.toHex(), QByteArray("00841e3462aba1733f8c0503d1f7bb6d"));
    }

    // The digest's only zero is at index 27, after the 16-byte prefix: the key is the prefix, and
    // the old expression happened to be right here too.
    void zeroOnlyAfterThePrefixIsUnchanged()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000009");
        const QByteArray key = seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        QCOMPARE(key.size(), 16);
        QCOMPARE(key.toHex(), QByteArray("106fe1f527e1485118fa0bf9a7fbff17"));
        QCOMPARE(key, oldConstruction(input, QCryptographicHash::Sha256));
    }

    void noZeroInThePrefixIsUnchanged()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000000");
        const QByteArray key = seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        QCOMPARE(key.size(), 16);
        QCOMPARE(key.toHex(), QByteArray("e4ae0c82639990744974cc3495a82432"));
        QCOMPARE(key, oldConstruction(input, QCryptographicHash::Sha256));
    }

    // Servers before generation 7 hash with SHA-1: a 20-byte digest, still a 16-byte key.
    void sha1KeepsTheSameSixteenByteRule()
    {
        const QByteArray plain = saltedPin("00000000000000000000000000000000");
        const QByteArray plainKey = seathub::derivePairingKey(plain, QCryptographicHash::Sha1);
        QCOMPARE(plainKey.size(), 16);
        QCOMPARE(plainKey.toHex(), QByteArray("064b32c89099e2b9a716931a01d3f9f5"));

        // SHA-1 digest bb2d0ccc0c58f700... carries a zero at index 7.
        const QByteArray zero = saltedPin("00000000000000000000000000000020");
        QCOMPARE(oldConstruction(zero, QCryptographicHash::Sha1).size(), 7);
        const QByteArray zeroKey = seathub::derivePairingKey(zero, QCryptographicHash::Sha1);
        QCOMPARE(zeroKey.size(), 16);
        QCOMPARE(zeroKey.toHex(), QByteArray("bb2d0ccc0c58f7001b24a3b8e6035d88"));
    }

    // --- the attempt-scoped record -------------------------------------------------------------

    void noScopeMeansNoRecord()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000004");
        const QStringList lines = captureLog([&] {
            seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        });
        QVERIFY(lines.isEmpty());
        QVERIFY(seathub::PairingKeyLogScope::active() == nullptr);
    }

    void scopeCarriesOnlyItsOwnAttemptAndRestoresOnExit()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000004");
        QStringList inner;
        const QStringList outer = captureLog([&] {
            seathub::PairingKeyLogScope first(QStringLiteral("session-one"), 1);
            {
                seathub::PairingKeyLogScope second(QStringLiteral("session-two"), 2);
                inner = captureLog([&] {
                    seathub::derivePairingKey(input, QCryptographicHash::Sha256);
                });
            }
            QCOMPARE(seathub::PairingKeyLogScope::active()->sessionId(), QStringLiteral("session-one"));
            seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        });
        QCOMPARE(inner.size(), 1);
        QVERIFY(inner.first().contains(QStringLiteral("session_id=session-two client_attempt=2 ")));
        QCOMPARE(outer.size(), 1);
        QVERIFY(outer.first().contains(QStringLiteral("session_id=session-one client_attempt=1 ")));
        QVERIFY(seathub::PairingKeyLogScope::active() == nullptr);
    }

    void recordCarriesTheLengthAndTheNulFlagAndNothingElse()
    {
        const QByteArray input = saltedPin("00000000000000000000000000000004");
        const QStringList lines = captureLog([&] {
            seathub::PairingKeyLogScope scope(QStringLiteral("s-1"), 3);
            seathub::derivePairingKey(input, QCryptographicHash::Sha256);
        });
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines.first(),
                 QStringLiteral("pairing_instrumentation writer=client stage=key_material "
                                "session_id=s-1 client_attempt=3 aes_key_len=16 "
                                "digest_has_nul_in_first16=true"));
    }
};

QTEST_MAIN(TstPairingKey)

#include "tst_pairing_key.moc"

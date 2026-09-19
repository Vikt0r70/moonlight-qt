/*****************************************************************************
 * SeatHub fork - unit tests for the DPAPI token store (Plan 03-03 Task 1, D-30).
 *
 * D-30's requirement is negative and therefore easy to fake: "no plaintext token in any
 * file". These tests prove it the only way that means anything - by storing a token, reading
 * the bytes that actually landed on disk, and failing with a hex dump if the token's own bytes
 * appear in them. The DPAPI round trip itself runs against the real Windows API, which is one
 * of the few production paths this environment can exercise for real.
 *****************************************************************************/

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "seathub/token_store.h"

namespace {

QString hexDump(const QByteArray& bytes)
{
    QString dump = QStringLiteral("\n  %1 bytes:").arg(bytes.size());
    for (int i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0) {
            dump += QStringLiteral("\n  %1  ").arg(i, 6, 16, QLatin1Char('0'));
        }
        dump += QString::asprintf("%02x ", static_cast<unsigned char>(bytes.at(i)));
    }
    return dump;
}

// A token shaped like the ones the control plane issues: opaque, long, high-entropy-ish, and
// with a distinctive marker so a substring search on disk is meaningful.
const char* kToken = "sb_at_6f1c6f5e3a1e4b1e9f2e0f1a2b3c4d5e.PLAINTEXT-MARKER-9F27";

} // namespace

class TstTokenStore : public QObject
{
    Q_OBJECT

private:
    // A fresh directory per test function. Qt Test runs the slots in declaration order, so a
    // shared directory would let one test's stored token satisfy the next test's
    // "there is nothing stored" assertion.
    QScopedPointer<QTemporaryDir> m_dir;

private slots:
    void init()
    {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
    }

    void cleanup()
    {
        m_dir.reset();
    }

    // --- DPAPI primitives ------------------------------------------------------------------

    void protect_unprotect_roundTripsThroughTheRealApi()
    {
        const QByteArray plaintext(QByteArrayLiteral("a-token-value"));

        const QByteArray blob = TokenStore::protect(plaintext);
        QVERIFY2(!blob.isEmpty(), "CryptProtectData refused");

        // A DPAPI user-scope blob is not the plaintext, and is larger than it: the header
        // alone carries the user's master-key GUID.
        QVERIFY(blob != plaintext);
        QVERIFY(!blob.contains(plaintext));
        QVERIFY(blob.size() > plaintext.size());

        QCOMPARE(TokenStore::unprotect(blob), plaintext);
    }

    void unprotect_rejectsSomethingThatIsNotABlob()
    {
        QVERIFY2(TokenStore::unprotect(QByteArrayLiteral("not a dpapi blob")).isEmpty(),
                 "a non-blob must not decrypt");
        QVERIFY(TokenStore::unprotect(QByteArray()).isEmpty());
        QVERIFY(TokenStore::protect(QByteArray()).isEmpty());
    }

    void protect_isNotDeterministic()
    {
        // DPAPI mixes in a random session key, so the same plaintext never produces the same
        // ciphertext. Two files holding the same token are not byte-identical, which is what
        // stops a copied blob from being recognised as "the same token" without decrypting.
        const QByteArray a = TokenStore::protect(QByteArrayLiteral("same"));
        const QByteArray b = TokenStore::protect(QByteArrayLiteral("same"));
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        QVERIFY(a != b);
    }

    // --- the store -------------------------------------------------------------------------

    void storeToken_writesNoPlaintextToDisk()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QVERIFY(store.storeToken(TokenStore::refreshTokenName(), QString::fromLatin1(kToken)));

        const QString path = store.pathFor(TokenStore::refreshTokenName());
        QVERIFY2(QFile::exists(path), qPrintable(path));

        const QByteArray onDisk = TokenStore::readFileBytes(path);

        // The whole point of D-30. If this fails the hex dump above the message says exactly
        // what leaked.
        QVERIFY2(!onDisk.contains(QByteArrayLiteral(kToken)),
                 qPrintable(QStringLiteral("the token appears verbatim in the file:%1")
                                .arg(hexDump(onDisk))));
        QVERIFY2(!TokenStore::fileContainsPlaintext(path, QString::fromLatin1(kToken)),
                 qPrintable(QStringLiteral("plaintext token found in %1%2").arg(path, hexDump(onDisk))));
        // And no fragment of it either.
        QVERIFY2(!onDisk.contains(QByteArrayLiteral("PLAINTEXT-MARKER-9F27")),
                 qPrintable(QStringLiteral("a token fragment appears in the file:%1")
                                .arg(hexDump(onDisk))));
    }

    void storeToken_thenRetrieve_returnsTheToken()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QVERIFY(store.storeToken(TokenStore::accessTokenName(), QString::fromLatin1(kToken)));
        QVERIFY(store.hasToken(TokenStore::accessTokenName()));
        QCOMPARE(store.retrieveToken(TokenStore::accessTokenName()), QString::fromLatin1(kToken));
    }

    void retrieveToken_withoutAStoredToken_isEmpty()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());
        QVERIFY(!store.hasToken(TokenStore::accessTokenName()));
        QVERIFY(store.retrieveToken(TokenStore::accessTokenName()).isEmpty());
    }

    void retrieveToken_fromACorruptedFile_isEmptyNotCiphertext()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QFile file(store.pathFor(TokenStore::refreshTokenName()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("someone replaced the blob"));
        file.close();

        // A file that does not decrypt is "no credential" - and the caller must never receive
        // the bytes back as if they were the token.
        QVERIFY(store.retrieveToken(TokenStore::refreshTokenName()).isEmpty());
    }

    void storeToken_replacesTheOldBlobAtomically()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QVERIFY(store.storeToken(TokenStore::refreshTokenName(), QStringLiteral("first")));
        QVERIFY(store.storeToken(TokenStore::refreshTokenName(), QStringLiteral("second")));
        QCOMPARE(store.retrieveToken(TokenStore::refreshTokenName()), QStringLiteral("second"));
    }

    // --- teardown (STREAM-10) --------------------------------------------------------------

    void clearToken_removesTheFile()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QVERIFY(store.storeToken(TokenStore::accessTokenName(), QString::fromLatin1(kToken)));
        QVERIFY(store.clearToken(TokenStore::accessTokenName()));
        QVERIFY(!store.hasToken(TokenStore::accessTokenName()));

        // Clearing a token that is already gone is success, not an error: teardown may run
        // twice and the second run must not look like a failure.
        QVERIFY(store.clearToken(TokenStore::accessTokenName()));
    }

    void clearAll_leavesNothingBehind()
    {
        TokenStore store;
        store.setDirectory(m_dir->path());

        QVERIFY(store.storeToken(TokenStore::accessTokenName(), QString::fromLatin1(kToken)));
        QVERIFY(store.storeToken(TokenStore::refreshTokenName(), QString::fromLatin1(kToken)));

        QVERIFY(store.clearAll());

        QVERIFY(!store.hasToken(TokenStore::accessTokenName()));
        QVERIFY(!store.hasToken(TokenStore::refreshTokenName()));
        QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files).size(), 0);
    }

    void clearAll_leavesForeignFilesAlone()
    {
        // The store deletes only what it wrote. A neighbouring file in the same directory is
        // not this class's to remove.
        QFile foreign(QDir(m_dir->path()).filePath(QStringLiteral("settings.json")));
        QVERIFY(foreign.open(QIODevice::WriteOnly));
        foreign.write("{}");
        foreign.close();

        TokenStore store;
        store.setDirectory(m_dir->path());
        QVERIFY(store.storeToken(TokenStore::accessTokenName(), QString::fromLatin1(kToken)));
        QVERIFY(store.clearAll());

        QVERIFY(QFile::exists(QDir(m_dir->path()).filePath(QStringLiteral("settings.json"))));
    }

    void defaultDirectory_isThePerUserApplicationDataDirectory()
    {
        // The names the client actually installs itself under (`app/main.cpp`): organization
        // "Seven Hills", application "SeatHub". Set here because this test binary is not the
        // client, and the resolved path - not the store's code - is what those two names decide.
        const QString previousOrganization = QCoreApplication::organizationName();
        const QString previousApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("Seven Hills"));
        QCoreApplication::setApplicationName(QStringLiteral("SeatHub"));

        // One store per user account, which is the same scope DPAPI encrypts for. The store must
        // not invent a path of its own - it uses the platform's per-user location, named after the
        // application.
        const QString directory = TokenStore::defaultDirectory();
        QVERIFY(!directory.isEmpty());
        QCOMPARE(directory, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        QVERIFY2(directory.contains(QCoreApplication::applicationName()),
                 qPrintable(QStringLiteral("%1 does not name the application (%2)")
                                .arg(directory, QCoreApplication::applicationName())));

        // F-5: pin the resolution, so the divergence between it and D-30/D-45's
        // `%LocalAppData%\SeatHub` is visible in a test run rather than in a comment nobody reads.
        // `%APPDATA%` is the Roaming profile because the store is per-user and must roam with the
        // account, exactly like the DPAPI scope it relies on.
        QVERIFY2(directory.contains(QStringLiteral("Seven Hills")),
                 qPrintable(QStringLiteral("resolved store directory: %1").arg(directory)));
        QVERIFY2(directory.endsWith(QStringLiteral("SeatHub")),
                 qPrintable(QStringLiteral("resolved store directory: %1").arg(directory)));
#ifdef Q_OS_WIN
        QVERIFY2(directory.contains(QStringLiteral("Roaming")),
                 qPrintable(QStringLiteral("resolved store directory: %1").arg(directory)));
#endif

        QCoreApplication::setOrganizationName(previousOrganization);
        QCoreApplication::setApplicationName(previousApplication);
    }
};

QTEST_MAIN(TstTokenStore)

#include "tst_token_store.moc"

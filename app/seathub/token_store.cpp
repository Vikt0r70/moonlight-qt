#include "token_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

// `NOMINMAX` before <windows.h>: the Win32 headers define `min`/`max` as macros, which breaks
// every `std::min`/`std::max` in a translation unit that includes them after Qt.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// `wincrypt.h` declares `CryptProtectData` / `CryptUnprotectData`; `dpapi.h` is the narrower
// header the same functions also live in. Both are included so the build does not depend on
// which one the SDK happens to make available.
#include <dpapi.h>
#include <wincrypt.h>

Q_LOGGING_CATEGORY(seathubTokenStore, "seathub.token_store")

namespace {

// One file per token, all of them DPAPI blobs. The suffix is what `clearAll()` sweeps, so a
// file this store did not write is never deleted.
const char* kBlobSuffix = ".dpapi";

// The folder the installer's control script parks the token directory in while it removes the old
// install (`installer/config/controlscript.qs`, `seathubBackUpTokens`): `SeatHub-sign-in-<epoch
// ms>` under %TEMP%, holding a copy of the token directory under its own leaf name. The digits
// are what orders two of them; a folder that does not match is never read and never deleted.
const char* kBackupFolderPattern = "^SeatHub-sign-in-(\\d+)$";

// DPAPI refuses with a Win32 error rather than a message. It is diagnostic-only - the
// customer gets a SeatHub sentence - but without it a store failure is unattributable.
QString lastError()
{
    return QStringLiteral("DPAPI error 0x%1")
        .arg(static_cast<quint32>(GetLastError()), 8, 16, QLatin1Char('0'));
}

} // namespace

QString TokenStore::accessTokenName()
{
    return QStringLiteral("access");
}

QString TokenStore::refreshTokenName()
{
    return QStringLiteral("refresh");
}

TokenStore::TokenStore(QObject* parent)
    : QObject(parent),
      m_directory(defaultDirectory()),
      m_backupRoot(QDir::tempPath())
{
}

void TokenStore::setBackupRoot(const QString& directory)
{
    m_backupRoot = directory;
}

QString TokenStore::defaultDirectory()
{
    // `%APPDATA%/SeatHub` on Windows. AppDataLocation is per-user, which is the right scope
    // for a credential belonging to the signed-in customer.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

void TokenStore::setDirectory(const QString& directory)
{
    m_directory = directory;
}

QString TokenStore::pathFor(const QString& name) const
{
    return QDir(m_directory).filePath(name + QLatin1String(kBlobSuffix));
}

// ---------------------------------------------------------------- DPAPI

QByteArray TokenStore::protect(const QByteArray& plaintext)
{
    if (plaintext.isEmpty()) {
        return QByteArray();
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};

    // User scope (no CRYPTPROTECT_LOCAL_MACHINE): the blob decrypts only for this account on
    // this machine. `CRYPTPROTECT_UI_FORBIDDEN` is required, not optional - D-30's store must
    // never raise a Windows prompt, because the pairing it protects is silent by design.
    if (!CryptProtectData(&input, L"SeatHub credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return QByteArray();
    }

    const QByteArray blob(reinterpret_cast<const char*>(output.pbData),
                          static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return blob;
}

QByteArray TokenStore::unprotect(const QByteArray& blob)
{
    if (blob.isEmpty()) {
        return QByteArray();
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()));
    input.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB output{};

    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return QByteArray();
    }

    const QByteArray plaintext(reinterpret_cast<const char*>(output.pbData),
                               static_cast<int>(output.cbData));
    // DPAPI hands back a plain buffer; zero it before releasing so the decrypted token does
    // not linger in freed heap.
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

// ---------------------------------------------------------------- files

bool TokenStore::storeToken(const QString& name, const QString& value)
{
    const QByteArray blob = protect(value.toUtf8());
    if (blob.isEmpty()) {
        emit storeFailed(SeatHubFailure::local(
            QStringLiteral("The credential store refused to encrypt. ") + lastError()));
        return false;
    }

    QDir().mkpath(m_directory);

    // QSaveFile: the previous blob stays intact unless the new one is fully written and
    // committed, so a crash mid-write cannot leave a half-token behind.
    QSaveFile file(pathFor(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(blob) != blob.size() || !file.commit()) {
        emit storeFailed(SeatHubFailure::local(
            QStringLiteral("The credential store could not be written.")));
        return false;
    }

    return true;
}

QString TokenStore::retrieveToken(const QString& name) const
{
    QFile file(pathFor(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    const QByteArray plaintext = unprotect(file.readAll());
    if (plaintext.isEmpty()) {
        // Either the file is not a DPAPI blob or it belongs to another user. Both are
        // "no credential", and neither is worth a plaintext fallback.
        return QString();
    }

    return QString::fromUtf8(plaintext);
}

bool TokenStore::hasToken(const QString& name) const
{
    return QFileInfo::exists(pathFor(name));
}

bool TokenStore::clearToken(const QString& name)
{
    const QString path = pathFor(name);
    if (!QFileInfo::exists(path)) {
        return true;
    }
    QFile::remove(path);
    return !QFileInfo::exists(path);
}

bool TokenStore::clearAll()
{
    QDir dir(m_directory);
    const QStringList blobs = dir.entryList({ QStringLiteral("*") + QLatin1String(kBlobSuffix) },
                                            QDir::Files);
    bool clean = true;
    for (const QString& blob : blobs) {
        const QString path = dir.filePath(blob);
        QFile::remove(path);
        if (QFileInfo::exists(path)) {
            clean = false;
        }
    }
    return clean;
}

// ---------------------------------------------------------------- keeping sign-in through an update

TokenStore::RecoveryReport TokenStore::recoverAtStartup()
{
    RecoveryReport report;

    if (!retrieveToken(accessTokenName()).isEmpty()) {
        report.alreadySignedIn = true;
        qCInfo(seathubTokenStore) << "credential present in the access slot; nothing to recover";
        return report;
    }

    if (migrateLegacySlot(&report)) {
        return report;
    }

    if (recoverFromUpdateBackup(&report)) {
        // What the installer parked is whatever 0.1.4 wrote: the 0.1.x slot. Carry it across the
        // same way a credential that never left the token directory is.
        migrateLegacySlot(&report);
    }

    if (report.discardedBlobs > 0) {
        qCInfo(seathubTokenStore) << "discarded" << report.discardedBlobs
                                  << "unusable credential blob(s)";
    }
    return report;
}

bool TokenStore::migrateLegacySlot(RecoveryReport* report)
{
    const QString legacy = refreshTokenName();
    if (!hasToken(legacy)) {
        return false;
    }

    const QString value = retrieveToken(legacy);
    if (value.isEmpty()) {
        // Not a blob this account can read (corrupt, or copied from another account): no
        // credential, and nothing worth keeping.
        clearToken(legacy);
        ++report->discardedBlobs;
        return false;
    }

    // Written to the access slot first and the old slot cleared only after, so a failed write
    // leaves the customer's credential exactly where it was.
    if (!storeToken(accessTokenName(), value)) {
        return false;
    }
    clearToken(legacy);

    report->migratedLegacySlot = true;
    qCInfo(seathubTokenStore) << "moved the credential from the 0.1.x slot to the access slot";
    return true;
}

bool TokenStore::recoverFromUpdateBackup(RecoveryReport* report)
{
    static const QRegularExpression folderName(QString::fromLatin1(kBackupFolderPattern));

    struct Candidate {
        QString name;
        quint64 stamp = 0;
    };

    QList<Candidate> candidates;
    const QDir root(m_backupRoot);
    const QStringList folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& folder : folders) {
        const QRegularExpressionMatch match = folderName.match(folder);
        if (!match.hasMatch()) {
            continue;
        }
        bool ok = false;
        const quint64 stamp = match.captured(1).toULongLong(&ok);
        if (ok) {
            candidates.append({ folder, stamp });
        }
    }

    // Newest first: the folder the update that just ran left is the one that matters.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });

    // Only the two slot names this store writes are ever taken back. A file with any other name in
    // a folder under TEMP is not ours to copy into the token directory.
    const QStringList slotFiles = {
        accessTokenName() + QLatin1String(kBlobSuffix),
        refreshTokenName() + QLatin1String(kBlobSuffix),
    };

    for (const Candidate& candidate : candidates) {
        const QString folderPath = root.filePath(candidate.name);

        // The control script copies the token directory in under its own leaf name
        // (`<folder>/SeatHub/*.dpapi`); the blobs are also accepted directly under the folder, so
        // this does not depend on the leaf name staying what it is today.
        QStringList searchDirs = { folderPath };
        const QDir folder(folderPath);
        const QStringList subdirs = folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& subdir : subdirs) {
            searchDirs.append(folder.filePath(subdir));
        }

        int usable = 0;
        int copied = 0;
        for (const QString& dirPath : searchDirs) {
            for (const QString& fileName : slotFiles) {
                const QString source = QDir(dirPath).filePath(fileName);
                if (!QFileInfo::exists(source)) {
                    continue;
                }
                const QByteArray blob = readFileBytes(source);
                if (unprotect(blob).isEmpty()) {
                    // Another account's blob, or not a blob at all: never trusted, never copied.
                    ++report->discardedBlobs;
                    continue;
                }
                ++usable;

                QDir().mkpath(m_directory);
                QSaveFile out(QDir(m_directory).filePath(fileName));
                if (out.open(QIODevice::WriteOnly) && out.write(blob) == blob.size()
                        && out.commit()) {
                    ++copied;
                }
            }
        }

        if (usable > 0 && copied == 0) {
            // A usable credential that could not be put back is the one case where deleting the
            // folder would lose it. Leave it for the next launch.
            qCWarning(seathubTokenStore) << "found an update backup but could not restore it";
            return false;
        }

        // Taken back, or holding nothing usable: either way it is not left lying around.
        QDir(folderPath).removeRecursively();

        if (copied > 0) {
            report->recoveredFromBackup = true;
            qCInfo(seathubTokenStore) << "credential taken back from an update backup; "
                                         "the backup folder was deleted";
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------- checks

QByteArray TokenStore::readFileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

bool TokenStore::fileContainsPlaintext(const QString& path, const QString& needle)
{
    if (needle.isEmpty()) {
        return false;
    }
    const QByteArray bytes = readFileBytes(path);
    return bytes.contains(needle.toUtf8());
}

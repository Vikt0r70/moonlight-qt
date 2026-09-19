#include "token_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

namespace {

// One file per token, all of them DPAPI blobs. The suffix is what `clearAll()` sweeps, so a
// file this store did not write is never deleted.
const char* kBlobSuffix = ".dpapi";

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
      m_directory(defaultDirectory())
{
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

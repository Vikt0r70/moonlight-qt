#pragma once

// SeatHub: ADR-0109. The binary-safe pairing key, and the attempt-scoped record of its length.
//
// Upstream's pair method derived the 16-byte AES key from a SHA digest with
// `QByteArray(QCryptographicHash::hash(...).constData())` followed by `truncate(16)`. That
// constructor reads the digest as a C string, so it stops at the first 0x00 byte (about 6.1% of
// digests carry a zero among their first sixteen bytes) and `truncate` cannot grow it again: the
// key came out short, and the rig answered "Incorrect PIN". `derivePairingKey` keeps the digest as
// bytes and takes the first sixteen by explicit length.
//
// Header-only, Qt Core only. The upstream cpp includes this file; nothing here reaches into the
// engine, and nothing here logs a PIN, salt, digest or key byte.

#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QString>
#include <QtGlobal>

namespace seathub {

/// The pairing attempt a key derivation belongs to. It is installed on the worker thread around the
/// real handshake call, carries only the captured session id and client attempt number, is
/// thread-local, and is restored on exit. There is no global "current session": a derivation made
/// outside a scope (a warm return never derives; any other caller) records nothing.
class PairingKeyLogScope
{
public:
    PairingKeyLogScope(const QString& sessionId, quint64 clientAttempt)
        : m_sessionId(sessionId), m_clientAttempt(clientAttempt), m_previous(slot())
    {
        slot() = this;
    }

    ~PairingKeyLogScope() { slot() = m_previous; }

    PairingKeyLogScope(const PairingKeyLogScope&) = delete;
    PairingKeyLogScope& operator=(const PairingKeyLogScope&) = delete;

    /// The scope active on the calling thread, or null.
    static const PairingKeyLogScope* active() { return slot(); }

    const QString& sessionId() const { return m_sessionId; }
    quint64 clientAttempt() const { return m_clientAttempt; }

private:
    static PairingKeyLogScope*& slot()
    {
        static thread_local PairingKeyLogScope* current = nullptr;
        return current;
    }

    QString m_sessionId;
    quint64 m_clientAttempt;
    PairingKeyLogScope* m_previous;
};

/// One `key_material` record for the active scope: two scalars about the real digest and key, plus
/// the session and attempt already on every other pairing record. Nothing else.
inline void logKeyMaterial(const QByteArray& digest, const QByteArray& aesKey)
{
    const PairingKeyLogScope* scope = PairingKeyLogScope::active();
    if (scope == nullptr) {
        return;
    }
    const QString sessionId = scope->sessionId();
    const quint64 clientAttempt = scope->clientAttempt();
    const int aesKeyLen = static_cast<int>(aesKey.size());
    const QString digestHasNul = digest.left(16).contains('\0') ? QStringLiteral("true") : QStringLiteral("false");
    qInfo().noquote() << QStringLiteral("pairing_instrumentation writer=client stage=key_material session_id=%1 client_attempt=%2 aes_key_len=%3 digest_has_nul_in_first16=%4").arg(sessionId).arg(clientAttempt).arg(aesKeyLen).arg(digestHasNul);
}

/// The 16-byte pairing AES key: the first sixteen bytes of the digest of the salted PIN, SHA-256
/// for server generation 7 and later and SHA-1 before it (the caller picks `hashAlgo`).
inline QByteArray derivePairingKey(const QByteArray& saltedPin, QCryptographicHash::Algorithm hashAlgo)
{
    const QByteArray digest = QCryptographicHash::hash(saltedPin, hashAlgo);
    // Explicit length: the digest is binary and may hold a zero byte anywhere in its prefix.
    const QByteArray aesKey = digest.left(16);
    logKeyMaterial(digest, aesKey);
    return aesKey;
}

} // namespace seathub

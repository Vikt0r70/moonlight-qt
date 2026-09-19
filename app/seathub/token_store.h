#pragma once

// The DPAPI token store (D-30).
//
// D-30 is a safety requirement, not a convenience: the bearer credentials this client holds
// are never written to disk in plaintext. Every value that reaches a file first goes through
// Windows DPAPI (`CryptProtectData`), which ties the ciphertext to the current user account,
// and comes back through `CryptUnprotectData`.
//
// Scope is the **user**, not the machine. This client runs as the customer's own interactive
// user, so a machine-scope blob would be readable by every account on the box - which is the
// opposite of what a token store is for. (`docs/spec/agents.md` uses machine scope for *agent*
// tokens, because those services run as LocalSystem; that reasoning does not transfer to an
// interactive client.)
//
// Files live under `QStandardPaths::AppDataLocation` (`%APPDATA%/SeatHub`), one file per named
// token, holding the raw DPAPI blob and nothing else - no header, no field name, no length.
// The on-disk artefact is proven to contain no plaintext by `tst_token_store`, which greps the
// blob for the token's own bytes and hex-dumps it on failure.
//
// Its other job is teardown: STREAM-10 requires that the client keeps no stored rig, address
// or pairing of its own. `clearAll()` is what makes that true, and teardown refuses to report
// success while anything is left behind.

#include <QByteArray>
#include <QObject>
#include <QString>

#include "error_map.h"

class TokenStore : public QObject
{
    Q_OBJECT

public:
    /// The pair of credentials sign-in issues (ADR-0014): `access` and `refresh`.
    static QString accessTokenName();
    static QString refreshTokenName();

    explicit TokenStore(QObject* parent = nullptr);

    /// `QStandardPaths::AppDataLocation` - `%APPDATA%/SeatHub` on Windows.
    static QString defaultDirectory();

    /// Overrides the directory. Exists for tests and for a portable install; production uses
    /// the default.
    void setDirectory(const QString& directory);
    QString directory() const { return m_directory; }

    /// The file a named token lives in.
    QString pathFor(const QString& name) const;

    // --- the store

    /// Encrypts `value` with DPAPI and writes the blob. Returns false - and emits
    /// `storeFailed` - when DPAPI refuses or the file cannot be written. A plaintext write is
    /// not a fallback: there isn't one.
    bool storeToken(const QString& name, const QString& value);

    /// Reads and decrypts. Returns an empty string when the file is absent, unreadable, or
    /// does not decrypt for this user (which is what a copied blob from another account looks
    /// like). Never throws, never returns ciphertext.
    QString retrieveToken(const QString& name) const;

    bool hasToken(const QString& name) const;

    /// Deletes one token's file. True when the token is gone afterwards, whether or not this
    /// call removed it.
    bool clearToken(const QString& name);

    /// Deletes every token this store owns. The STREAM-10 guarantee: when this returns true
    /// there is no credential of ours left on disk.
    bool clearAll();

    // --- DPAPI primitives, static so they are testable against the real API

    /// `CryptProtectData` at user scope with `CRYPTPROTECT_UI_FORBIDDEN` - a silent operation,
    /// which is what a pairing that never prompts the customer requires. Returns an empty
    /// array on failure.
    static QByteArray protect(const QByteArray& plaintext);
    /// `CryptUnprotectData`. Returns an empty array when the blob was not produced by this
    /// user on this machine.
    static QByteArray unprotect(const QByteArray& blob);

    /// True when `needle` appears verbatim in the file at `path`. The plaintext check D-30
    /// needs, expressed as a test the test suite can also run against a good blob.
    static bool fileContainsPlaintext(const QString& path, const QString& needle);

    /// The file's bytes, for a hex dump in a failure message.
    static QByteArray readFileBytes(const QString& path);

signals:
    /// The store refused. `kind` is Local: this is a machine-local failure, and it is fatal
    /// to the credential path rather than something to retry silently.
    void storeFailed(const SeatHubFailure& failure);

private:
    QString m_directory;
};

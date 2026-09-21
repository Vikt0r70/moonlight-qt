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
// Files live under `QStandardPaths::AppDataLocation`, one file per named token, holding the raw
// DPAPI blob and nothing else - no header, no field name, no length. The on-disk artefact is
// proven to contain no plaintext by `tst_token_store`, which greps the blob for the token's own
// bytes and hex-dumps it on failure.
//
// The location resolves through the application and organization names the client sets in
// `app/main.cpp` ("Seven Hills" / "SeatHub"), so on Windows it is `%APPDATA%\Seven Hills\SeatHub` -
// the Roaming profile, not `%LocalAppData%`. That is deliberately recorded here as the *resolved*
// fact rather than restated as an aspiration: D-30 and D-45 name `%LocalAppData%\SeatHub`, an
// earlier version of this comment said `%APPDATA%\SeatHub`, and neither matches what the platform
// actually returns. Reconciling the documents with the code (or the code with the documents) is an
// ADR decision; changing the path silently would move existing customers' credentials.
//
// The store holds one thing: the customer's sign-in credential (the access slot). STREAM-10
// requires that the client keeps no stored rig, address or pairing of its own, and that holds
// because none of the three is ever written here. Teardown therefore does NOT clear this store -
// a customer who plays once stays signed in (CUST-08) - and only sign-out (`clearAll()`) removes
// the credential.

#include <QByteArray>
#include <QObject>
#include <QString>

#include "error_map.h"

class TokenStore : public QObject
{
    Q_OBJECT

public:
    /// The slot the credential lives in: ADR-0050 made sessions permanent, so the one non-expiring
    /// access token is the whole credential and this is where every sign-in writes it.
    static QString accessTokenName();
    /// The slot 0.1.x wrote the same credential under, back when sign-in stored a refresh token
    /// (its OTP path stored the access token there once refresh tokens stopped existing). Nothing
    /// writes it any more. It survives as a name because an installed 0.1.4 that updates to this
    /// build has its credential there, and `recoverAtStartup()` carries it across (WINDOWS #22).
    static QString refreshTokenName();

    explicit TokenStore(QObject* parent = nullptr);

    /// What `recoverAtStartup()` did, for the caller's log and for the tests. No member ever holds
    /// a credential.
    struct RecoveryReport
    {
        /// The access slot already held a usable credential, so nothing was touched.
        bool alreadySignedIn = false;
        /// A credential found in the 0.1.x slot was moved into the access slot.
        bool migratedLegacySlot = false;
        /// A credential parked by the installer's update backup was taken back.
        bool recoveredFromBackup = false;
        /// Blobs that would not unprotect for this account (corrupt, or written by another
        /// account) and were deleted rather than trusted.
        int discardedBlobs = 0;
    };

    /// Keeps a customer signed in through an update, as a property of the client rather than of the
    /// installer's ordering. Run once at startup, before the credential is read:
    ///
    ///   1. If the access slot already holds a credential, do nothing.
    ///   2. Slot migration: if the 0.1.x slot holds one, move it to the access slot and clear the
    ///      old one.
    ///   3. Update-backup recovery: if neither slot holds one, look in the backup root (the user's
    ///      temporary directory) for the folder the installer's control script parks the token
    ///      directory in during an update (`SeatHub-sign-in-<epoch ms>`, see
    ///      `installer/config/controlscript.qs`), take the newest usable one back, and delete the
    ///      folder so a copy of the credential is not left lying around.
    ///
    /// The bytes stay DPAPI-protected throughout. A blob that will not unprotect for this account
    /// is discarded. The outcome is logged, never the credential.
    ///
    /// Deliberately NOT run from the constructor: tests (and a portable install) point the store at
    /// another directory with `setDirectory()` after constructing it, and a constructor-time
    /// recovery would have run against the real per-user directory first.
    RecoveryReport recoverAtStartup();

    /// Where `recoverAtStartup()` looks for update backups. Defaults to the user's temporary
    /// directory (`QDir::tempPath()`); a test points it at a scratch folder.
    void setBackupRoot(const QString& directory);
    QString backupRoot() const { return m_backupRoot; }

    /// `QStandardPaths::AppDataLocation` - `%APPDATA%\Seven Hills\SeatHub` on Windows, derived
    /// from the organization and application names set in `app/main.cpp` (see the note at the top
    /// of this file about D-30/D-45 naming `%LocalAppData%\SeatHub` instead).
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
    /// Step 2 of `recoverAtStartup()`. True when a credential was moved.
    bool migrateLegacySlot(RecoveryReport* report);
    /// Step 3 of `recoverAtStartup()`. True when a credential was taken back.
    bool recoverFromUpdateBackup(RecoveryReport* report);

    QString m_directory;
    QString m_backupRoot;
};

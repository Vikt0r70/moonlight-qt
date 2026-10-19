#pragma once

// EvidenceJournal (ADR-0105, owner rulings (f) and (g) of 2026-10-17): the per-Play, closed-value,
// on-disk evidence capsule. Pure Qt Core logic - file I/O through QSaveFile, JSON through
// QJsonDocument, no network, no GUI, no SDL, no Windows API - so it builds and is tested on Linux
// (`tests/tst_evidence_journal.pro`). It posts nothing: the facade (T063) asks `deliverable()`
// what is waiting, sends each body to `POST /api/sessions/{id}/evidence`, and reports the status
// to `applyAnswer()`.
//
// Shape (the same one-JSON-file-per-record style as `EndOutbox`/`QualityOutbox`):
//   * One capsule file per Play in the journal folder, written atomically (QSaveFile) at every
//     recorded event: disk first, then send. A capsule opens when the customer presses Play
//     (`open`) and closes at the session's end, a failed start or teardown (`close`). While no
//     Play is open `record` does nothing, so nothing is written while the app is idle, signed out
//     or browsing.
//   * The file IS the request body plus the session and account it belongs to. `run_id` is a random
//     UUID per Play; `seq` is unique within it; so a replay carries the same `run_id`/`seq` and the
//     server stores each entry once.
//   * Closed values only. The static builders below refuse any value outside the kind's set and
//     return an EMPTY object; `record` of an empty or invalid object stores nothing and counts
//     `dropped_entries`. No API accepts an address, host name, SSID or adapter name, file path,
//     user name, PIN, salt, token or hash; the only strings that reach a capsule are members of a
//     closed set or ADR-0072 vocabulary tokens. `open` accepts only opaque ids made of
//     `[A-Za-z0-9_-]`, 1 to 64 characters (a UUID passes), which refuses an address, a host
//     name, a path, an e-mail address and any name with a space or punctuation. The ids are kept
//     in the local file and in the session-id log line only; they are never part of the request
//     body (the session id is in the URL).
//   * Caps: at most `entryCap` (256) entries per Play record and a `directoryCapBytes` (2 MiB)
//     folder. Over the folder cap the oldest closed capsule is reduced to its summary, and only
//     when every closed capsule is already a summary is the oldest summary dropped. Every
//     reduction, drop, unreadable file and over-cap entry is counted; the counters ride the next
//     delivered capsule (they are written into the first capsule `deliverable()` hands out, so a
//     replay of it carries the same numbers). No age expiry. The open Play's capsule is never
//     reduced or dropped by the cap.
//   * The answer decides the file: 2xx and 404 remove what was sent; 400 and 422 drop the capsule
//     and count it; everything else (401, 403, 5xx, transport) keeps it. Another account's capsule
//     is never touched by an account switch and is only handed out for its own account.
//
// `deliverable()` is const because it hands out data, but it also settles housekeeping on disk
// (removes and counts unreadable files, folds the pending counters into the first capsule). All
// state lives in the folder (plus the in-flight high-water marks), so this is logical constness.

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

#include "port_test_result.h"

class EvidenceJournal
{
public:
    static constexpr int kDefaultEntryCap = 256;
    static constexpr qint64 kDefaultDirectoryCapBytes = 2 * 1024 * 1024;

    /// The four loss counters of the wire `counters` object.
    struct Counters {
        qint64 reducedCapsules = 0;
        qint64 droppedCapsules = 0;
        qint64 unreadableFiles = 0;
        qint64 droppedEntries = 0;
    };

    /// One capsule ready to send: the session it belongs to, its `run_id` and the exact request
    /// body (compact JSON).
    struct PendingCapsule {
        QString sessionId;
        QString runId;
        QByteArray body;
    };

    explicit EvidenceJournal(QString directory = QString());

    void setDirectory(const QString& directory);
    QString directory() const;
    /// `<AppDataLocation>/evidence-journal`, beside `end-outbox`.
    static QString defaultDirectory();

    /// Folder cap in bytes (default 2 MiB). A value below 1 is ignored.
    void setDirectoryCapBytes(qint64 bytes);
    /// Entries per Play record (default and maximum 256). Values outside 1..256 are clamped.
    void setEntryCap(int entries);

    /// Open a capsule for a Play: a fresh random `run_id`, no file until the first entry. Closes a
    /// capsule that is still open. False (nothing opened) when an id is not an opaque id.
    bool open(const QString& sessionId, const QString& accountId);
    /// Close the capsule. What was recorded stays on disk until the server has it.
    void close();
    bool isOpen() const;

    /// Add one entry (a builder's result) to the open capsule, stamping `seq`, `at` and
    /// `elapsed_ms`. A no-op while closed. An empty or invalid entry, or one past the entry cap,
    /// stores nothing and counts `dropped_entries`.
    void record(const QJsonObject& entry);

    // Closed-vocabulary builders. Each returns the entry's kind and fields (no seq/at/elapsed_ms)
    // or an EMPTY object when any value is outside the kind's closed set.

    /// `engineStep` is one of the 11 ADR-0072 attempt steps. A failure's `engineClass` is a
    /// closed class of that step; a success's is empty or `ok`. `retryReason` is empty,
    /// `conflict` or `failure` and only with `retried`.
    static QJsonObject pairingAttempt(int attempt, bool success, const QString& engineStep,
                                      const QString& engineClass, bool retried,
                                      const QString& retryReason, bool pinShapeOk,
                                      qint64 handshakeMs);
    /// Empty for `NotRun` (nothing was tested, so nothing is reported).
    static QJsonObject portTest(const PortTestResult& result);
    /// `kind` is `end_outbox` or `quality_outbox`.
    static QJsonObject outbox(const QString& kind, const QString& event, const QString& answer,
                              int count);
    static QJsonObject stallWindow(int windows, qint64 durationMs);
    static QJsonObject networkChange(const QString& medium, const QString& reachability,
                                     bool captivePortal, bool metered, int transitions);
    static QJsonObject engineFailure(const QString& step, const QString& engineClass,
                                     int engineCode);
    /// `within_5s` when |local - server| <= 5 s, `client_ahead` / `client_behind` beyond it,
    /// `unknown` when either instant is invalid.
    static QJsonObject clockSkew(const QDateTime& serverUtc, const QDateTime& localUtc);

    /// Capsules of `accountId` waiting for the server, oldest first. The open Play's capsule is
    /// included once it holds an entry (failure-class events send while the Play runs).
    QList<PendingCapsule> deliverable(const QString& accountId) const;

    /// The server's answer for a capsule `deliverable()` handed out: an HTTP status, or -1 for a
    /// transport failure. 2xx and 404 remove what was sent (entries recorded after the capsule was
    /// handed out stay); 400 and 422 drop the capsule and count it; any other status keeps it.
    void applyAnswer(const QString& sessionId, const QString& runId,
                     int httpStatusOrMinusOneForTransport);

    /// Loss counts not yet written into a capsule (diagnostic; tests read it).
    Counters pendingCounters() const;

    /// Milliseconds on the monotonic clock since this process started (the static timer starts
    /// when the program is loaded).
    static qint64 processElapsedMs();

private:
    struct Capsule;

    QString capsulePath(const QString& fileName) const;
    QStringList capsuleFileNames() const;
    bool readCapsule(const QString& fileName, Capsule* out) const;
    bool writeCapsule(const QString& fileName, const Capsule& capsule) const;
    Counters readPendingCounters() const;
    void addPendingCounters(const Counters& delta) const;
    void enforceCapLocked();
    void removeUnreadable(const QString& fileName) const;

    mutable QMutex m_mutex;
    QString m_directory;
    qint64 m_directoryCapBytes = kDefaultDirectoryCapBytes;
    int m_entryCap = kDefaultEntryCap;

    // The open Play. The capsule's entries and counters live on disk, not here.
    bool m_open = false;
    QString m_sessionId;
    QString m_accountId;
    QString m_runId;
    QString m_fileName;
    int m_nextSeq = 0;

    // run_id -> highest seq handed out by deliverable(), so a 2xx removes only what was sent.
    mutable QHash<QString, qint64> m_inflightMaxSeq;
};

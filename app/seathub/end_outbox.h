#pragma once

// The durable End outbox (V-OUTBOX, row L06, ADR-0083 decision 2).
//
// An End the customer presses is written to disk *before* the first `POST
// /api/sessions/{id}/end`, tagged with the account it belongs to, and re-sent until the server
// confirms it (2xx). It survives a crash or a restart, is dropped on 404 (the session is already
// gone - that is a confirmation), and is kept on status 0, 5xx or 401. While an entry is queued
// for the signed-in account, Home shows the ending state and Play stays disabled.
//
// It is deliberately the same shape as `QualityOutbox` (`quality_outbox.{h,cpp}`): one JSON file
// per session under a per-user directory, written with `QSaveFile` (atomic - a half-written file
// is never left behind for the next launch to read), and drained by the facade, which owns the
// retry cadence. This class does no I/O on a timer and posts nothing itself: the facade asks it
// what is queued and tells it what the server answered.
//
// It never carries a credential: the account id is the only thing stored beside the End body, and
// the POST that drains it uses the facade's own access manager.

#include <functional>

#include <QMutex>
#include <QString>
#include <QStringList>

#include "control_plane_client.h"

class EndOutbox
{
public:
    /// The classification of one End POST's answer, the same vocabulary `QualityOutbox` uses.
    /// `Delivered` (2xx) and `Refused` (404) both mean the entry is done with and is removed;
    /// `AuthFailed` (401/403) and `Retryable` (status 0, 5xx, anything else) keep it.
    enum Outcome { Delivered, Refused, AuthFailed, Retryable, Aborted };

    static Outcome classify(const ControlPlaneResult& result);

    /// The closed answer token for one End POST's answer: `2xx`, `400`, `401`, `403`, `404`, `422`,
    /// `5xx`, `transport` (status 0) or `none`. The same vocabulary `EvidenceJournal::outbox` takes.
    static QString answerToken(const ControlPlaneResult& result);

    /// One log line per outbox event (ADR-0105): `event` is `put`, `drain`, `delivered`, `kept`,
    /// `dropped` or `unreadable`; `answer` is `answerToken()` or `none`; `count` is how many
    /// entries the event covers. The line carries the session id and these closed values only -
    /// never a path, a token or a body. Also calls the observer, if one is set.
    void noteEvent(const QString& event, const QString& sessionId, const QString& answer,
                   int count) const;

    /// Called after every `noteEvent()` (outside the lock), so the facade can record the event in
    /// the evidence journal. Set once, before the outbox is used from another thread.
    using Observer = std::function<void(const QString& event, const QString& answer, int count)>;
    void setObserver(Observer observer);

    explicit EndOutbox(QString directory = QString());

    void setDirectory(const QString& directory);
    QString directory() const;

    /// `<AppDataLocation>/end-outbox`.
    static QString defaultDirectory();

    /// One queued End, as read back from disk.
    struct Entry {
        QString sessionId;
        bool failed = false;
        EndReport report;
    };

    /// Write (or overwrite) the entry for `sessionId`. Called before the first End POST. Returns
    /// false only when the file could not be written - the caller still sends, but has no durable
    /// record, so it must keep Home unconfirmed and must not report the End as saved: a failure of
    /// the send then reaches the customer through the ordinary failure screen, never a ready Home.
    bool put(const QString& sessionId, const QString& accountId, bool failed,
             const EndReport& report);

    /// Remove the entry for `sessionId`. Called once the server confirmed the end (2xx) or the
    /// session is already gone (404).
    void remove(const QString& sessionId);

    /// True when an entry for `sessionId` is on disk.
    bool hasQueued(const QString& sessionId) const;

    /// True when any entry is queued for `accountId`. Entries for other accounts are left alone:
    /// an account switch must not send the previous account's End with the new account's token,
    /// and must not lose the previous account's record either.
    bool hasQueuedForAccount(const QString& accountId) const;

    /// Every entry queued for `accountId`, oldest filename first.
    QList<Entry> entriesForAccount(const QString& accountId) const;

private:
    QString pathFor(const QString& sessionId) const;

    mutable QMutex m_mutex;
    QString m_directory;
    Observer m_observer;
};

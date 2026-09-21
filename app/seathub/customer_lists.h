#pragma once

// The profile's three histories, as list models the QML draws and pages through (Phase 5 plan 09,
// CUST-14, D-14, D-15).
//
//   SessionListModel        finished sessions:   date and time | how it ended | length
//   CreditHistoryModel      the ledger:          date | plain kind | signed amount
//   TopupListModel          the top-up notices:  date sent | waiting or credited | minutes added
//
// Three rules the shape of these classes exists to hold:
//
//   * A row is already the text the screen draws. Dates are Jordan time (`jordan_time.h`), lengths
//     and amounts go through the one duration formatter (`duration_text.h`), and how a session ended
//     or what a ledger entry was are the copy deck's short words - a key from the server never
//     reaches QML. Nothing here counts, adds or infers anything: every minute a row shows is a number
//     the server sent, printed as it came (T-05-39).
//   * No row names a rig. The contract's session row has no such member, and nothing in this file
//     reads one (CUST-01, T-05-37).
//   * Paging appends. A page is asked for once: a second request while one is in flight is ignored,
//     asking for more with no cursor left does nothing, and a page that fails leaves the rows already
//     loaded where they are and records why. Each list is independent of the others (D-14): one
//     failing never blanks another.
//
// The cursor is the server's, opaque here, held only to be sent back unchanged (T-05-38).
//
// The control plane client answers on its own thread once a stream has begun (see its header), so
// every reply is marshalled back to this model's thread before it is applied. A reply that was asked
// for before the list was reset (a sign-out, or the customer leaving and returning to the profile) is
// dropped: the generation it was asked under is no longer the list's.

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include "control_plane_client.h"

/// `copy.md` § Session end reasons, short forms (C6d): how a session ended in a few plain words. The
/// key is the server's `end_reason`; it is never shown. Any reason the deck has no form for reads
/// `Ended`, and no reason at all reads `Ended` too.
QString endReasonShortText(const QString& endReason);

/// `copy.md` § Menu and profile, ledger kinds (C6e): what a ledger entry was, in plain words. The key
/// is the server's `kind`; it is never shown. A kind the deck has no word for reads as nothing, not
/// as a guess.
QString ledgerKindText(const QString& kind);

class CustomerListModel : public QAbstractListModel
{
    Q_OBJECT

    /// The first page's state: "idle" (not asked for yet), "loading", "ready" (rows, or none), or
    /// "error" (the first page failed; there are no rows). A failure of a later page is not "error":
    /// the loaded rows stay and `moreFailed` says so.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)

    /// True while the next page is being fetched (the footer row's spinner).
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY stateChanged)

    /// True when the next page failed. The rows already loaded are still there.
    Q_PROPERTY(bool moreFailed READ moreFailed NOTIFY stateChanged)

    /// True while the server has said there is another page.
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)

    /// The last failure's sentence (the server's own, or the deck's offline or generic one) and its
    /// ADR-0008 reference when the server gave one. Empty when the last request succeeded.
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString errorReference READ errorReference NOTIFY stateChanged)

    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        /// `Thu 12 Sep, 21:40`
        WhenRole = Qt::UserRole + 1,
        /// How it ended, what it was, or waiting/credited.
        KindRole,
        /// The length, the signed amount, or the minutes added (blank while waiting).
        AmountRole,
        /// "waiting" | "credited" for a top-up row's status dot; empty for every other row.
        ToneRole,
    };
    Q_ENUM(Role)

    /// About fifteen rows at a time (`screens.md` §27, `05-UI-SPEC` Profile).
    static constexpr int kPageSize = 15;

    QString status() const;
    bool loadingMore() const { return m_loadingMore; }
    bool moreFailed() const { return m_moreFailed; }
    bool hasMore() const { return !m_nextCursor.isEmpty(); }
    QString errorText() const { return m_errorText; }
    QString errorReference() const { return m_errorReference; }
    int count() const { return m_rows.size(); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Asks for the first page. Does nothing unless the list has never been asked (a list that is
    /// loading, loaded or failed is left as it is: `reload()` is how a failed one starts again).
    Q_INVOKABLE void loadFirstPage();

    /// Asks for the next page when the server has one and no request is in flight; otherwise does
    /// nothing. After a failed next page it asks for that same page again.
    Q_INVOKABLE void loadNextPage();

    /// Starts again from the top: forgets the rows and any failure, drops whatever reply is still on
    /// its way and asks for the first page.
    Q_INVOKABLE void reload();

    /// Forgets everything and returns to "idle": rows, cursor, failure, and any reply still on its
    /// way. Called when the customer leaves the profile or signs out, so nobody sees another
    /// customer's rows.
    void reset();

signals:
    void stateChanged();
    void countChanged();
    /// A request was answered 401: the credential is no longer valid. The facade clears it and goes
    /// to sign-in (`screens.md` §27: a rejected credential anywhere).
    void credentialRefused();

protected:
    /// One row as the screen draws it.
    struct Row
    {
        QString when;
        QString kind;
        QString amount;
        QString tone;
    };

    explicit CustomerListModel(ControlPlaneClient* plane, QObject* parent);

    ControlPlaneClient* plane() const { return m_plane; }

    /// Sends the request for the page at `cursor` (empty for the first).
    virtual void fetch(const QString& cursor, ControlPlaneClient::Callback callback) = 0;

    /// Turns a page's body into rows and the server's next cursor. False when the body is not a page.
    virtual bool parsePage(const QJsonObject& body, QVector<Row>* rows, QString* nextCursor) const = 0;

private:
    void begin(bool firstPage, const QString& cursor);
    void apply(quint64 generation, bool firstPage, const ControlPlaneResult& result);

    ControlPlaneClient* m_plane = nullptr;
    QVector<Row> m_rows;
    QString m_nextCursor;
    QString m_errorText;
    QString m_errorReference;
    bool m_asked = false;
    bool m_firstFailed = false;
    bool m_loadingFirst = false;
    bool m_loadingMore = false;
    bool m_moreFailed = false;
    /// Bumped by every reset. A reply carries the value it was asked under and is dropped when the
    /// list has moved on.
    quint64 m_generation = 0;
};

class SessionListModel : public CustomerListModel
{
    Q_OBJECT

public:
    explicit SessionListModel(ControlPlaneClient* plane, QObject* parent = nullptr);

protected:
    void fetch(const QString& cursor, ControlPlaneClient::Callback callback) override;
    bool parsePage(const QJsonObject& body, QVector<Row>* rows, QString* nextCursor) const override;
};

class CreditHistoryModel : public CustomerListModel
{
    Q_OBJECT

public:
    explicit CreditHistoryModel(ControlPlaneClient* plane, QObject* parent = nullptr);

protected:
    void fetch(const QString& cursor, ControlPlaneClient::Callback callback) override;
    bool parsePage(const QJsonObject& body, QVector<Row>* rows, QString* nextCursor) const override;
};

class TopupListModel : public CustomerListModel
{
    Q_OBJECT

public:
    explicit TopupListModel(ControlPlaneClient* plane, QObject* parent = nullptr);

protected:
    void fetch(const QString& cursor, ControlPlaneClient::Callback callback) override;
    bool parsePage(const QJsonObject& body, QVector<Row>* rows, QString* nextCursor) const override;
};

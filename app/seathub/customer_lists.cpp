#include "customer_lists.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include "duration_text.h"
#include "jordan_time.h"

namespace {

// `copy.md` § Session end reasons, short forms (C6d). Verbatim; the key is the server's and is never
// drawn. Two keys share the rig-did-not-come-back form, as the deck's table has it.
struct EndReasonForm
{
    const char* key;
    const char* text;
};

const EndReasonForm kEndReasonForms[] = {
    { "CUSTOMER_ENDED", "You ended it" },
    { "CONNECT_FAILED", "Didn't start, not charged" },
    { "BALANCE_EXHAUSTED", "Balance ran out" },
    { "HOST_LOST", "Lost contact with the rig" },
    { "CLIENT_SILENT", "Lost contact with your device" },
    { "CONNECT_TIMEOUT", "Not started in time, not charged" },
    { "READINESS_TIMEOUT", "Rig didn't come back, not charged" },
    { "MODE_BOOT_TIMEOUT", "Rig didn't come back, not charged" },
    { "GRACE_EXPIRED", "Couldn't reconnect" },
    { "OWNER_RESERVATION", "Owner reserved the rig" },
    { "TEARDOWN_TIMEOUT", "Closed after a problem" },
    { "OPERATOR_FORCED", "Ended by support" },
};

// `copy.md` § Menu and profile, the plain kind of each ledger entry (C6e). Verbatim.
const EndReasonForm kLedgerKinds[] = {
    { "topup_credit", "Top-up" },
    { "first_bonus", "First top-up bonus" },
    { "session_debit", "Played" },
    { "refund", "Refund" },
    { "adjustment", "Adjustment by support" },
    { "shortfall", "Unpaid minutes" },
};

// `copy.md` § Menu and profile: a top-up's status in a row.
const char* kTopupWaiting = "Waiting";
const char* kTopupCredited = "Credited";

// Runs `fn` on `owner`'s thread: at once when this already is it, queued otherwise. The control
// plane client answers on its own thread once a stream has begun.
template <typename Fn>
void onOwnerThread(QObject* owner, Fn fn)
{
    if (QThread::currentThread() == owner->thread()) {
        fn();
        return;
    }
    QMetaObject::invokeMethod(owner, std::move(fn), Qt::QueuedConnection);
}

} // namespace

QString endReasonShortText(const QString& endReason)
{
    for (const EndReasonForm& form : kEndReasonForms) {
        if (endReason == QLatin1String(form.key)) {
            return QString::fromLatin1(form.text);
        }
    }
    // The deck's own catch-all: any other reason, and no reason at all.
    return QStringLiteral("Ended");
}

QString ledgerKindText(const QString& kind)
{
    for (const EndReasonForm& form : kLedgerKinds) {
        if (kind == QLatin1String(form.key)) {
            return QString::fromLatin1(form.text);
        }
    }
    // The deck has no word for a kind it does not list, and a guess would be an invented string.
    return QString();
}

// ---------------------------------------------------------------- the shared paging behaviour

CustomerListModel::CustomerListModel(ControlPlaneClient* plane, QObject* parent)
    : QAbstractListModel(parent),
      m_plane(plane)
{
}

QString CustomerListModel::status() const
{
    if (!m_asked) {
        return QStringLiteral("idle");
    }
    if (m_loadingFirst) {
        return QStringLiteral("loading");
    }
    if (m_firstFailed) {
        return QStringLiteral("error");
    }
    return QStringLiteral("ready");
}

int CustomerListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant CustomerListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
    case WhenRole:
        return row.when;
    case KindRole:
        return row.kind;
    case AmountRole:
        return row.amount;
    case ToneRole:
        return row.tone;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> CustomerListModel::roleNames() const
{
    return {
        { WhenRole, "whenText" },
        { KindRole, "kindText" },
        { AmountRole, "amountText" },
        { ToneRole, "tone" },
    };
}

void CustomerListModel::loadFirstPage()
{
    if (m_asked) {
        return;
    }
    begin(true, QString());
}

void CustomerListModel::loadNextPage()
{
    // Nothing to ask for: not loaded yet, still loading, a request already out, or the last page.
    // Each of these is a plain no-op, so a view that asks every time it reaches the end can never
    // double-fetch a page.
    if (!m_asked || m_loadingFirst || m_firstFailed || m_loadingMore || m_nextCursor.isEmpty()) {
        return;
    }
    begin(false, m_nextCursor);
}

void CustomerListModel::reload()
{
    reset();
    begin(true, QString());
}

void CustomerListModel::reset()
{
    ++m_generation;

    const bool hadRows = !m_rows.isEmpty();
    if (hadRows) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
    }
    m_nextCursor.clear();
    m_errorText.clear();
    m_errorReference.clear();
    m_asked = false;
    m_firstFailed = false;
    m_loadingFirst = false;
    m_loadingMore = false;
    m_moreFailed = false;

    emit stateChanged();
    if (hadRows) {
        emit countChanged();
    }
}

void CustomerListModel::begin(bool firstPage, const QString& cursor)
{
    if (firstPage) {
        m_asked = true;
        m_loadingFirst = true;
        m_firstFailed = false;
    }
    else {
        m_loadingMore = true;
        m_moreFailed = false;
    }
    m_errorText.clear();
    m_errorReference.clear();
    emit stateChanged();

    const quint64 generation = m_generation;
    QPointer<CustomerListModel> self(this);
    fetch(cursor, [self, generation, firstPage](const ControlPlaneResult& result) {
        if (!self) {
            return;
        }
        onOwnerThread(self.data(), [self, generation, firstPage, result]() {
            if (self) {
                self->apply(generation, firstPage, result);
            }
        });
    });
}

void CustomerListModel::apply(quint64 generation, bool firstPage, const ControlPlaneResult& result)
{
    // Asked for before the list was reset: it belongs to a list that no longer exists.
    if (generation != m_generation) {
        return;
    }

    QString failure;
    QString reference;

    if (result.ok) {
        QVector<Row> rows;
        QString next;
        if (parsePage(result.body, &rows, &next)) {
            if (!rows.isEmpty()) {
                const int first = m_rows.size();
                beginInsertRows(QModelIndex(), first, first + static_cast<int>(rows.size()) - 1);
                for (const Row& row : rows) {
                    m_rows.append(row);
                }
                endInsertRows();
                emit countChanged();
            }
            m_nextCursor = next;
            m_loadingFirst = false;
            m_loadingMore = false;
            m_firstFailed = false;
            m_moreFailed = false;
            m_errorText.clear();
            m_errorReference.clear();
            emit stateChanged();
            return;
        }
        // A 2xx that is not a page: the deck's generic sentence, and no reference (the client never
        // makes one up).
        const SeatHubFailure generic = SeatHubFailure::generic();
        failure = generic.error;
    }
    else {
        const SeatHubFailure fail = result.toFailure();
        failure = fail.error;
        reference = fail.reference;
    }

    if (firstPage) {
        m_loadingFirst = false;
        m_firstFailed = true;
    }
    else {
        m_loadingMore = false;
        m_moreFailed = true;
    }
    m_errorText = failure;
    m_errorReference = reference;
    emit stateChanged();

    if (!result.ok && result.statusCode == 401) {
        emit credentialRefused();
    }
}

// ---------------------------------------------------------------- the three lists

SessionListModel::SessionListModel(ControlPlaneClient* plane, QObject* parent)
    : CustomerListModel(plane, parent)
{
}

void SessionListModel::fetch(const QString& cursor, ControlPlaneClient::Callback callback)
{
    plane()->fetchSessionList(cursor, kPageSize, callback);
}

bool SessionListModel::parsePage(const QJsonObject& body, QVector<Row>* rows,
                                 QString* nextCursor) const
{
    CustomerSessionPage page;
    if (!CustomerSessionPage::parse(body, &page)) {
        return false;
    }
    for (const CustomerSessionRow& row : page.rows) {
        // The length is the server's own billed-minutes count for the session.
        rows->append({ jordanDateTimeText(row.requestedAt), endReasonShortText(row.endReason),
                       durationText(row.minutesBilled), QString() });
    }
    *nextCursor = page.nextCursor;
    return true;
}

CreditHistoryModel::CreditHistoryModel(ControlPlaneClient* plane, QObject* parent)
    : CustomerListModel(plane, parent)
{
}

void CreditHistoryModel::fetch(const QString& cursor, ControlPlaneClient::Callback callback)
{
    plane()->fetchWalletHistory(cursor, kPageSize, callback);
}

bool CreditHistoryModel::parsePage(const QJsonObject& body, QVector<Row>* rows,
                                   QString* nextCursor) const
{
    LedgerPage page;
    if (!LedgerPage::parse(body, &page)) {
        return false;
    }
    for (const LedgerRow& row : page.rows) {
        // The amount is the server's own signed number; its sign is the whole meaning of the row.
        rows->append({ jordanDateTimeText(row.createdAt), ledgerKindText(row.kind),
                       signedDurationText(row.amountMinutes), QString() });
    }
    *nextCursor = page.nextCursor;
    return true;
}

TopupListModel::TopupListModel(ControlPlaneClient* plane, QObject* parent)
    : CustomerListModel(plane, parent)
{
}

void TopupListModel::fetch(const QString& cursor, ControlPlaneClient::Callback callback)
{
    plane()->fetchTopupNotices(cursor, kPageSize, callback);
}

bool TopupListModel::parsePage(const QJsonObject& body, QVector<Row>* rows,
                               QString* nextCursor) const
{
    TopupNoticePage page;
    if (!TopupNoticePage::parse(body, &page)) {
        return false;
    }
    for (const TopupNoticeRow& row : page.rows) {
        // Closed by an operator's credit, which stamps `credited_at`; the minutes are the ones that
        // credit added, exactly as the operator granted them, and are blank while the notice is open.
        const bool credited = !row.creditedAt.isEmpty();
        const QString minutes = credited && row.creditedMinutes >= 0
                                    ? signedDurationText(row.creditedMinutes)
                                    : QString();
        rows->append({ jordanDateTimeText(row.sentAt),
                       QString::fromLatin1(credited ? kTopupCredited : kTopupWaiting), minutes,
                       QString::fromLatin1(credited ? "credited" : "waiting") });
    }
    *nextCursor = page.nextCursor;
    return true;
}

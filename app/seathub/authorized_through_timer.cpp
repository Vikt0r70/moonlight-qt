#include "authorized_through_timer.h"

#include <QLoggingCategory>
#include <QThread>

#include <limits>

Q_LOGGING_CATEGORY(seathubHorizon, "seathub.horizon")

namespace {

// A QTimer takes an int. Caps and long horizons are clamped to this and re-evaluated when they
// fire, rather than overflowing into an immediate fire.
const int kMaxTimerMs = std::numeric_limits<int>::max() - 1;

} // namespace

AuthorizedThroughTimer::AuthorizedThroughTimer(QObject* parent)
    : QObject(parent),
      m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        // Re-check before firing: the horizon may have moved while this timer was armed, and a
        // stale fire would end a session the control plane had just extended.
        if (remainingMs() > 0) {
            schedule();
            return;
        }

        qCWarning(seathubHorizon)
            << "authorized_through horizon reached; stopping locally (D-33)";
        emit horizonReached();
    });
}

bool AuthorizedThroughTimer::onOwnThread() const
{
    // `thread()` goes null only when the QThread that owned this object has been destroyed, in
    // which case there is no queue left to hand work to and running here is the only option.
    QThread* owner = thread();
    return owner == nullptr || owner == QThread::currentThread();
}

qint64 AuthorizedThroughTimer::msUntil(const QDateTime& nowUtc, const QDateTime& horizonUtc)
{
    if (!nowUtc.isValid() || !horizonUtc.isValid()) {
        return 0;
    }
    return nowUtc.msecsTo(horizonUtc);
}

qint64 AuthorizedThroughTimer::remainingMs() const
{
    if (!m_horizon.isValid()) {
        return 0;
    }
    return msUntil(QDateTime::currentDateTimeUtc(), m_horizon);
}

void AuthorizedThroughTimer::arm(const QString& authorizedThroughIso)
{
    if (!onOwnThread()) {
        // The arming call comes from the facade on the main thread; the QTimer lives on the network
        // thread. Qt refuses `QTimer::start()` from a foreign thread, which is how the horizon
        // became unreachable and left the billing-safety stop (D-33/D-34) armed on paper only.
        QMetaObject::invokeMethod(this, [this, authorizedThroughIso]() { arm(authorizedThroughIso); },
                                  Qt::QueuedConnection);
        return;
    }

    const QDateTime parsed = QDateTime::fromString(authorizedThroughIso, Qt::ISODate);

    if (authorizedThroughIso.isEmpty() || !parsed.isValid()) {
        // `authorized_through` is nullable in the contract. Null means this session has no
        // stated horizon - which is not a reason to invent one.
        disarm();
        return;
    }

    const QDateTime horizon = parsed.toUTC();
    if (horizon <= QDateTime::currentDateTimeUtc()) {
        // Already past: the authorization this session was given has run out. Firing on the
        // next event-loop turn is the only correct answer, and it is not the same as "disarm".
        m_horizon = horizon;
        emit horizonChanged();
        m_timer->start(0);
        return;
    }

    m_horizon = horizon;
    emit horizonChanged();
    schedule();
}

void AuthorizedThroughTimer::extend(const QString& authorizedThroughIso)
{
    // Delegates rather than marshalling twice: `arm()` is the single guarded entry point.
    arm(authorizedThroughIso);
}

void AuthorizedThroughTimer::disarm()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { disarm(); }, Qt::QueuedConnection);
        return;
    }

    if (m_timer->isActive()) {
        m_timer->stop();
    }
    if (m_horizon.isValid()) {
        m_horizon = QDateTime();
        emit horizonChanged();
    }
}

void AuthorizedThroughTimer::schedule()
{
    const qint64 remaining = remainingMs();
    if (remaining <= 0) {
        m_timer->start(0);
        return;
    }
    m_timer->start(static_cast<int>(qMin<qint64>(remaining, kMaxTimerMs)));
}

void AuthorizedThroughTimer::hardStop()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { hardStop(); }, Qt::QueuedConnection);
        return;
    }

    // The control plane said the session is over, or the customer pressed End. Same path as the
    // horizon: stop locally, then let the facade run teardown.
    disarm();
    emit horizonReached();
}

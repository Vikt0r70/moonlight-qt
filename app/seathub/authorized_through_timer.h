#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

// The billing-safety hard stop (D-33, WR-04).
//
// The session token carries `authorized_through`: the local stop horizon for this launch
// (`docs/spec/client.md` §Session Token Fields - "hard local stop horizon if the VPS becomes
// unreachable"). `GET /api/sessions/{id}` also reports it, and `Session.authorized_through` is
// described there as "the effective lease horizon - min of the high-water horizon and any owner
// reservation hard stop".
//
// This is the ONLY place a session is ended locally on a timer. Liveness failures deliberately
// are not (see `liveness_timer.h`): if the control plane is unreachable the stream is still
// working, and the honest local rule is "keep streaming until the authorization we were given
// runs out, then stop" - which is exactly what this class enforces. Without it, an unreachable
// control plane would let a session run past its authorization and the customer would be
// streaming on credit the server never approved.
//
// A later `authorized_through` (the control plane extending the session, renewing the lease)
// re-arms the timer to the new horizon. That is the extension path; there is no other.
//
// Threading: the facade moves this object onto the network thread `ControlPlaneClient` owns, so
// the QTimer fires while upstream has the Qt main thread suspended for the stream
// (`app/streaming/session.cpp:1965-1966`). `arm()`, `extend()` and `disarm()` are called from the
// main thread, so each re-invokes itself queued when it is not on the owning thread; without that
// the timer start was refused by Qt ("Timers cannot be started from another thread" - a warning and
// a no-op) and the billing-safety horizon could never fire (CR-02).
//
// There is no maximum session length (STREAM-11). This timer does not impose one - it enforces
// the horizon the control plane set, which moves forward for as long as the customer keeps
// paying. A session that is never extended and never ended locally will still be ended by the
// wallet or the customer, not by a clock this client invented.
class AuthorizedThroughTimer : public QObject
{
    Q_OBJECT

public:
    explicit AuthorizedThroughTimer(QObject* parent = nullptr);

    /// Milliseconds from `nowUtc` until `horizonUtc`. Zero or negative when the horizon has
    /// already passed. Pure, so the arithmetic is assertable without waiting for a clock.
    static qint64 msUntil(const QDateTime& nowUtc, const QDateTime& horizonUtc);

    bool isArmed() const { return m_timer->isActive(); }
    QString horizon() const { return m_horizon.toString(Qt::ISODate); }
    qint64 remainingMs() const;

    /// Arm - or re-arm, if the horizon moved - from an RFC 3339 instant. An empty, unparsable
    /// or already-passed horizon disarms: a session with no stated horizon is governed by the
    /// control plane, not by a locally invented deadline.
    void arm(const QString& authorizedThroughIso);
    /// The extension path: same as `arm()`, named for the caller's intent.
    void extend(const QString& authorizedThroughIso);
    void disarm();

    /// Stop the session now, before the horizon. Used by the facade when the control plane
    /// reports the session over; the horizon is a backstop, not the only exit.
    void hardStop();

signals:
    /// The horizon arrived. The facade ends the stream and runs teardown.
    void horizonReached();
    /// The armed horizon changed, so the UI's remaining-budget display can follow.
    void horizonChanged();

private:
    void schedule();
    /// True when the calling thread is the one this object lives on (or when it lives on no
    /// thread at all, which only happens after its thread has been destroyed).
    bool onOwnThread() const;

    QTimer* m_timer = nullptr;
    QDateTime m_horizon;
};

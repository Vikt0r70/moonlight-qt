#include "drop_check.h"

#include <QMetaObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include "control_plane_client.h"

DropCheck::DropCheck(ControlPlaneClient* controlPlane, QObject* parent)
    : QObject(parent), m_controlPlane(controlPlane)
{
}

void DropCheck::start(const QString& rigHost, int rigPort)
{
    if (m_started) {
        return; // one check per bad end, never a retry
    }
    m_started = true;

    // Leg 1: the control plane's health route. ControlPlaneClient bounds the transfer at 5 s too;
    // this timer is the bound the record owns, so the result never waits on anything else.
    m_serverClock.start();
    m_serverTimer = new QTimer(this);
    m_serverTimer->setSingleShot(true);
    connect(m_serverTimer, &QTimer::timeout, this, [this]() { finishServerLeg(false); });
    m_serverTimer->start(m_timeoutMs);
    if (m_controlPlane != nullptr) {
        QPointer<DropCheck> self(this);
        m_controlPlane->checkHealth([self](const ControlPlaneResult& result) {
            const bool reachable = result.statusCode >= 200 && result.statusCode < 300;
            // The control plane may answer from its own thread; the result is recorded on ours.
            if (self) {
                QMetaObject::invokeMethod(self.data(), [self, reachable]() {
                    if (self) {
                        self->finishServerLeg(reachable);
                    }
                }, Qt::QueuedConnection);
            }
        });
    } else {
        finishServerLeg(false);
    }

    // Leg 2: one TCP connect-and-close to the rig's port P. No handshake, no data.
    if (rigHost.isEmpty() || rigPort <= 0 || rigPort > 65535) {
        m_result.rigMs = 0;
        finishRigLeg(false);
        return;
    }
    m_rigClock.start();
    m_socket = new QTcpSocket(this);
    m_rigTimer = new QTimer(this);
    m_rigTimer->setSingleShot(true);
    connect(m_socket, &QTcpSocket::connected, this, [this]() { finishRigLeg(true); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { finishRigLeg(false); });
    connect(m_rigTimer, &QTimer::timeout, this, [this]() { finishRigLeg(false); });
    m_rigTimer->start(m_timeoutMs);
    m_socket->connectToHost(rigHost, static_cast<quint16>(rigPort));
}

void DropCheck::finishServerLeg(bool reachable)
{
    if (m_serverDone) {
        return;
    }
    m_serverDone = true;
    m_serverTimer->stop();
    m_result.serverReachable = reachable;
    m_result.serverMs = static_cast<int>(m_serverClock.elapsed());
    maybeFinish();
}

void DropCheck::finishRigLeg(bool open)
{
    if (m_rigDone) {
        return;
    }
    m_rigDone = true;
    if (m_rigTimer != nullptr) {
        m_rigTimer->stop();
    }
    if (m_socket != nullptr) {
        m_result.rigMs = static_cast<int>(m_rigClock.elapsed());
        m_socket->abort(); // close at once; nothing is ever sent
    }
    m_result.rigPortOpen = open;
    maybeFinish();
}

void DropCheck::maybeFinish()
{
    if (m_emitted || !m_serverDone || !m_rigDone) {
        return;
    }
    m_emitted = true;
    emit finished(m_result);
}

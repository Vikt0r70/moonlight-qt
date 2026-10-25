#pragma once

// DropCheck (T054, SC24, ADR-0117, ADR-0090): after a bad end, exactly ONE HTTPS request to the
// control plane's health route and ONE TCP connect-and-close to the rig's port P, each bounded by
// 5 s. It starts at the first local bad-end signal and never waits for the server to finalise the
// Play. There is no handshake, pairing, stream or retry, and it never reconnects. The answer is a
// pair of booleans and two millisecond counts; no address, host name or text leaves this class.
//
// Lives on the thread that created it (needs an event loop). The facade hands the result to
// `StreamRecorder::writeDropCheck`.

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "stream_record.h"

class ControlPlaneClient;
class QTcpSocket;
class QTimer;

class DropCheck : public QObject
{
    Q_OBJECT

public:
    static constexpr int kTimeoutMs = 5000;

    using Result = StreamRecorder::DropCheckResult;

    explicit DropCheck(ControlPlaneClient* controlPlane, QObject* parent = nullptr);

    /// Runs the check once. A second call is ignored. `rigHost` empty or `rigPort` <= 0 means the
    /// rig endpoint was never learned: no connect is made and the rig leg reports closed in 0 ms.
    void start(const QString& rigHost, int rigPort);
    bool started() const { return m_started; }
    /// A shorter bound than 5 s so a test of the timeout path does not wait. Production never calls it.
    void setTimeoutMsForTests(int ms) { m_timeoutMs = ms; }

signals:
    /// Emitted once, after both legs have answered or timed out.
    void finished(const DropCheck::Result& result);

private:
    void finishServerLeg(bool reachable);
    void finishRigLeg(bool open);
    void maybeFinish();

    ControlPlaneClient* m_controlPlane;
    int m_timeoutMs = kTimeoutMs;
    bool m_started = false;
    bool m_serverDone = false;
    bool m_rigDone = false;
    bool m_emitted = false;
    Result m_result;
    QElapsedTimer m_serverClock;
    QElapsedTimer m_rigClock;
    QTcpSocket* m_socket = nullptr;
    QTimer* m_serverTimer = nullptr;
    QTimer* m_rigTimer = nullptr;
};

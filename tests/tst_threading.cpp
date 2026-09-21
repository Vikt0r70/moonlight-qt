// The multi-threaded paths, for real (CR-01, CR-02, HR-01).
//
// Why this file exists: every other suite in `tests/` runs entirely on one thread. That is why
// 159 tests passed while the control-plane client was deleted twice, both billing timers were
// refused their start, and teardown never reached `Clear`. Nothing in this tree ever called
// `moveToThread()`, so nothing ever hit the rules this suite exists to hold:
//
//   * a `QTimer` starts, stops and fires only on the thread that owns it - a start from a foreign
//     thread is a warning and a no-op, never an error;
//   * `moveToThread()` is refused when it comes from a thread that does not own the object;
//   * an object moved to a thread is destroyed by that thread (or not at all), so ownership has to
//     be explicit about who deletes what, and when.
//
// The three findings these tests pin down:
//   CR-01  `ControlPlaneClient::stopOwnedThread()` destroys the client exactly once, on its own
//          thread, inside the join - and the caller must not delete it afterwards.
//   CR-02  `LivenessTimer` and `AuthorizedThroughTimer` hand a call from another thread over to
//          the thread that owns their `QTimer`, so the heartbeat ticks and the billing horizon
//          fires when they are started or armed from the main thread.
//   HR-01  `TeardownController`, driven from the network thread, reaches `Done` - and leaves the
//          DPAPI store (the customer's sign-in) alone.
//
// Build recipe:
//   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
//   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
//   cd tests && qmake tst_threading.pro && jom release && tst_threading.exe -o out.txt,txt

#include <QtTest>

#include <QBuffer>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>

#include "seathub/authorized_through_timer.h"
#include "seathub/control_plane_client.h"
#include "seathub/error_map.h"
#include "seathub/liveness_timer.h"
#include "seathub/teardown_controller.h"
#include "seathub/token_store.h"

namespace {

const char* kSessionId = "aaaabbbb-cccc-dddd-eeee-ffff00001111";

QByteArray sessionBody(const QString& state)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::fromLatin1(kSessionId));
    object.insert(QStringLiteral("state"), state);
    object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    object.insert(QStringLiteral("minutes_billed"), 7);
    object.insert(QStringLiteral("reconnect_count"), 0);
    object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T01:00:00Z"));
    object.insert(QStringLiteral("authorized_through"), QStringLiteral("2026-09-19T02:00:00Z"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// Answers every request with one canned status and body. The counters are atomic and the path list
// is mutex-guarded because in the teardown test this manager lives on the network thread while the
// test reads it from the main one.
class FakeReply : public QNetworkReply
{
public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent)
        : QNetworkReply(parent)
    {
        if (httpStatus != 0) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus);
        }
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        if (httpStatus == 0) {
            setError(QNetworkReply::HostNotFoundError, QStringLiteral("no route to control plane"));
        }
        // Delivered on the thread this reply lives on, which is the manager's.
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
    qint64 bytesAvailable() const override { return m_buffer.bytesAvailable(); }

private:
    QBuffer m_buffer;
};

class FakeNetworkAccessManager : public QNetworkAccessManager
{
public:
    std::atomic<int> status{200};
    QByteArray body;
    std::atomic<int> calls{0};

    int callCount() const { return calls.load(); }

    QStringList recordedPaths()
    {
        QMutexLocker locker(&m_mutex);
        return m_paths;
    }

protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override
    {
        Q_UNUSED(operation)
        Q_UNUSED(outgoingData)
        {
            QMutexLocker locker(&m_mutex);
            m_paths.append(request.url().path());
        }
        ++calls;
        return new FakeReply(status.load(), body, this);
    }

private:
    QMutex m_mutex;
    QStringList m_paths;
};

// The thread handle the calling code runs on, read from the object's own thread. `quintptr` rather
// than a pointer so it can be compared in a test without dereferencing anything that has already
// been destroyed.
quintptr currentThreadHandleOf(QObject* object)
{
    quintptr handle = 0;
    QMetaObject::invokeMethod(object, [&handle]() {
        handle = reinterpret_cast<quintptr>(QThread::currentThreadId());
    }, Qt::BlockingQueuedConnection);
    return handle;
}

// Counts destructions of the object it is mixed into and records the thread that did it, so
// "exactly once, on its own thread" is an assertion rather than a promise.
class DestructionCounter
{
public:
    explicit DestructionCounter(std::atomic<int>* count, std::atomic<quintptr>* threadId)
        : m_count(count), m_threadId(threadId) {}

    ~DestructionCounter()
    {
        m_count->fetch_add(1);
        if (m_threadId != nullptr) {
            m_threadId->store(reinterpret_cast<quintptr>(QThread::currentThreadId()));
        }
    }

private:
    std::atomic<int>* m_count;
    std::atomic<quintptr>* m_threadId;
};

class CountingObject : public QObject, public DestructionCounter
{
public:
    CountingObject(std::atomic<int>* count, std::atomic<quintptr>* threadId)
        : QObject(nullptr), DestructionCounter(count, threadId) {}
};

class CountingControlPlaneClient : public ControlPlaneClient, public DestructionCounter
{
public:
    CountingControlPlaneClient(std::atomic<int>* count, std::atomic<quintptr>* threadId)
        : ControlPlaneClient(nullptr), DestructionCounter(count, threadId) {}
};

// A control-plane client, a fake control plane and a worker thread with a running event loop - the
// arrangement the facade builds in `SeatHubClient::startNetworkThreads()`.
class ThreadedFixture
{
public:
    ThreadedFixture() = default;
    ~ThreadedFixture() { shutdown(); }

    ThreadedFixture(const ThreadedFixture&) = delete;
    ThreadedFixture& operator=(const ThreadedFixture&) = delete;

    // Objects that live on the worker thread and are destroyed after it has stopped.
    QList<QObject*> living;
    // Runs on the worker thread before anything is moved home (e.g. `timer->stop()`), so the
    // shutdown is deterministic rather than racing a queued call.
    std::function<void()> prepareShutdown;

    ControlPlaneClient* startClient(std::atomic<int>* destroyed,
                                    std::atomic<quintptr>* destroyedOn)
    {
        m_client = new CountingControlPlaneClient(destroyed, destroyedOn);
        m_network = new FakeNetworkAccessManager;
        m_client->setNetworkAccessManager(m_network);
        m_client->moveToOwnThread();
        m_thread = m_client->thread();
        if (m_thread != nullptr && m_thread != QThread::currentThread()) {
            // Same thread as the client, so the replies it creates are answered there too.
            m_network->moveToThread(m_thread);
            living.append(m_network);
        }
        return m_client;
    }

    ControlPlaneClient* client() const { return m_client; }
    FakeNetworkAccessManager* network() const { return m_network; }
    QThread* thread() const { return m_thread; }

    void own(QObject* object)
    {
        if (object != nullptr) {
            living.append(object);
        }
    }

    // The test destroyed the client itself (CR-01's own path); the fixture must not touch it.
    void forgetClient() { m_client = nullptr; }

    void shutdown()
    {
        ControlPlaneClient* owned = m_client;
        m_client = nullptr;

        const QThread* home = QThread::currentThread();
        const bool joinable = owned != nullptr && owned->onOwnThread() && m_thread != nullptr
                              && m_thread != QThread::currentThread();
        if (joinable) {
            QMetaObject::invokeMethod(owned, [this, home]() {
                if (prepareShutdown) {
                    prepareShutdown();
                }
                QThread* owner = QThread::currentThread();
                for (QObject* object : living) {
                    if (object != nullptr && object->thread() == owner) {
                        object->moveToThread(const_cast<QThread*>(home));
                    }
                }
            }, Qt::BlockingQueuedConnection);
        }

        if (owned != nullptr) {
            if (owned->onOwnThread()) {
                owned->stopOwnedThread();     // destroys it, on its own thread, exactly once
            }
            else {
                delete owned;
            }
        }

        qDeleteAll(living);
        living.clear();
        m_network = nullptr;
        m_thread = nullptr;
    }

private:
    CountingControlPlaneClient* m_client = nullptr;
    FakeNetworkAccessManager* m_network = nullptr;
    QThread* m_thread = nullptr;
};

QString stageSequence(const QSignalSpy& stages)
{
    QStringList names;
    for (const QList<QVariant>& emission : stages) {
        switch (emission.at(0).value<TeardownStage>()) {
        case TeardownStage::Idle: names.append(QStringLiteral("Idle")); break;
        case TeardownStage::Disable: names.append(QStringLiteral("Disable")); break;
        case TeardownStage::Unpair: names.append(QStringLiteral("Unpair")); break;
        case TeardownStage::Verify: names.append(QStringLiteral("Verify")); break;
        case TeardownStage::Clear: names.append(QStringLiteral("Clear")); break;
        case TeardownStage::Done: names.append(QStringLiteral("Done")); break;
        case TeardownStage::Failed: names.append(QStringLiteral("Failed")); break;
        }
    }
    return names.join(QLatin1Char(','));
}

} // namespace

class TstThreading : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // The mechanism the CR-01 fix rests on, pinned so nobody has to take the comment's word.
    void deferredDeleteIsFlushedByTheJoin();

    // ME-01's mechanism: a foreign-thread move is refused, an owner-thread move is not.
    void moveToThreadIsRefusedFromAForeignThread();

    // CR-01
    void controlPlaneClientDestroysItselfExactlyOnceOnItsOwnThread();
    void controlPlaneClientThatWasNeverMovedIsDeletedByItsOwner();

    // CR-02
    void livenessTimerStartedFromAnotherThreadKeepsReporting();
    void livenessTimerStoppedFromAnotherThreadStops();
    void horizonArmedFromAnotherThreadFires();

    // HR-01
    void teardownDrivenFromTheNetworkThreadReachesDoneAndKeepsTheSignIn();
};

void TstThreading::initTestCase()
{
    qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
    qRegisterMetaType<TeardownStage>("TeardownStage");
}

void TstThreading::deferredDeleteIsFlushedByTheJoin()
{
    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    auto* probe = new CountingObject(&destroyed, &destroyedOn);
    QThread* thread = new QThread;

    // `ControlPlaneClient::moveToOwnThread()` does exactly this pair.
    probe->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, probe, &QObject::deleteLater);
    thread->start();
    const quintptr workerHandle = currentThreadHandleOf(probe);

    QCOMPARE(destroyed.load(), 0);

    thread->quit();
    thread->wait();

    // Qt flushes the thread's deferred-delete queue as the thread unwinds
    // (`QThreadPrivate::finish()`), so the object is destroyed inside `wait()`, on the thread that
    // owned it. This is the fact CR-01 turns on: everything a `stopOwnedThread()` used to do after
    // the join was a use-after-free, and a caller that then deleted the pointer freed it twice.
    QCOMPARE(destroyed.load(), 1);
    QCOMPARE(destroyedOn.load(), workerHandle);

    delete thread;
}

void TstThreading::moveToThreadIsRefusedFromAForeignThread()
{
    QObject probe;
    QThread thread;
    thread.start();

    QThread* const home = QThread::currentThread();
    probe.moveToThread(&thread);
    QCOMPARE(probe.thread(), &thread);

    // From a thread that does not own it: Qt warns and does nothing. A caller that believed this
    // had brought an object home was wrong - which is exactly what `~SeatHubClient` used to do.
    probe.moveToThread(home);
    QCOMPARE(probe.thread(), &thread);

    // From the owning thread it works, which is the route the destructor now takes.
    QMetaObject::invokeMethod(&probe, [&probe, home]() { probe.moveToThread(home); },
                              Qt::BlockingQueuedConnection);
    QCOMPARE(probe.thread(), home);

    thread.quit();
    thread.wait();
}

void TstThreading::controlPlaneClientDestroysItselfExactlyOnceOnItsOwnThread()
{
    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    ThreadedFixture fixture;
    ControlPlaneClient* client = fixture.startClient(&destroyed, &destroyedOn);
    QVERIFY(client->onOwnThread());
    QVERIFY(fixture.thread() != nullptr);
    QVERIFY(fixture.thread() != QThread::currentThread());

    // The thread's event loop is running and answers a queued invocation.
    QVERIFY(QMetaObject::invokeMethod(client, []() {}, Qt::BlockingQueuedConnection));
    QCOMPARE(destroyed.load(), 0);

    const quintptr workerId = currentThreadHandleOf(client);

    // The facade's exit path, exactly: `onOwnThread()` true means "stop it and let it die".
    client->stopOwnedThread();

    QCOMPARE(destroyed.load(), 1);
    QCOMPARE(destroyedOn.load(), workerId);

    // The client is gone; the fixture must know better than to delete it again. Deleting it here -
    // as the old facade did - is the double free CR-01 names.
    fixture.forgetClient();
    fixture.shutdown();
    QCOMPARE(destroyed.load(), 1);
}

void TstThreading::controlPlaneClientThatWasNeverMovedIsDeletedByItsOwner()
{
    auto* client = new ControlPlaneClient(nullptr);
    QVERIFY(!client->onOwnThread());

    // `stopOwnedThread()` on an object that was never moved destroys nothing, which is why the
    // owner still has to delete it (the facade's other branch).
    client->stopOwnedThread();

    delete client;   // exactly once, and a double delete here would crash the suite
}

void TstThreading::livenessTimerStartedFromAnotherThreadKeepsReporting()
{
    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    ThreadedFixture fixture;
    auto* timer = new LivenessTimer;
    timer->setIntervalMs(5);
    timer->setGraceMs(10000);
    timer->setControlPlane(fixture.startClient(&destroyed, &destroyedOn));
    timer->moveToThread(fixture.thread());
    fixture.own(timer);
    fixture.network()->body = sessionBody(QStringLiteral("ACTIVE"));

    // From the main thread, which is what `handleConnectionStarted()` does. Before the CR-02 fix
    // this start was refused by Qt, so the heartbeat reported once (the immediate tick) and was
    // then silent for the rest of the session.
    timer->start(QString::fromLatin1(kSessionId));

    QTRY_VERIFY_WITH_TIMEOUT(fixture.network()->callCount() >= 3, 2000);
    QVERIFY(timer->isRunning());

    const QStringList paths = fixture.network()->recordedPaths();
    QVERIFY(!paths.isEmpty());
    QVERIFY(paths.last().startsWith(QStringLiteral("/api/sessions/")));
    QVERIFY(paths.last().endsWith(QStringLiteral("/liveness")));
}

void TstThreading::livenessTimerStoppedFromAnotherThreadStops()
{
    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    ThreadedFixture fixture;
    auto* timer = new LivenessTimer;
    timer->setIntervalMs(5);
    timer->setControlPlane(fixture.startClient(&destroyed, &destroyedOn));
    timer->moveToThread(fixture.thread());
    fixture.own(timer);
    fixture.network()->body = sessionBody(QStringLiteral("ACTIVE"));
    // Stop it on its own thread before anything is moved home, so the shutdown is not racing a
    // queued call.
    fixture.prepareShutdown = [timer]() { timer->stop(); };

    timer->start(QString::fromLatin1(kSessionId));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.network()->callCount() >= 2, 2000);

    timer->stop();
    // The stop is queued, so the owning thread runs it shortly afterwards; then nothing more is
    // reported.
    QTRY_VERIFY_WITH_TIMEOUT(!timer->isRunning(), 2000);
    const int atStop = fixture.network()->callCount();
    QTest::qWait(100);
    QCOMPARE(fixture.network()->callCount(), atStop);
}

void TstThreading::horizonArmedFromAnotherThreadFires()
{
    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    ThreadedFixture fixture;
    auto* horizon = new AuthorizedThroughTimer;
    fixture.startClient(&destroyed, &destroyedOn);
    horizon->moveToThread(fixture.thread());
    fixture.own(horizon);

    QSignalSpy reached(horizon, &AuthorizedThroughTimer::horizonReached);

    // A horizon a moment away, armed from the main thread. Before the fix the timer's start was
    // refused, so `horizonReached()` - the billing-safety hard stop (D-33/D-34) - was unreachable
    // and a session could outlive the token it was authorised by.
    const QString horizonIso = QDateTime::currentDateTimeUtc().addMSecs(50).toString(Qt::ISODate);
    horizon->arm(horizonIso);

    QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 1, 5000);
    QVERIFY(!horizon->isArmed());

    // A horizon that is already past fires at once, through the same path. Also queued back to
    // this thread, hence the wait rather than an immediate compare.
    horizon->hardStop();
    QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 2, 2000);
}

void TstThreading::teardownDrivenFromTheNetworkThreadReachesDoneAndKeepsTheSignIn()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    auto* store = new TokenStore(nullptr);
    store->setDirectory(directory.path());
    if (!store->storeToken(TokenStore::accessTokenName(), QStringLiteral("an-access-token"))) {
        QSKIP("DPAPI refused a token on this machine; the store cannot be exercised here");
    }
    QVERIFY(store->hasToken(TokenStore::accessTokenName()));

    std::atomic<int> destroyed{0};
    std::atomic<quintptr> destroyedOn{0};

    ThreadedFixture fixture;
    ControlPlaneClient* client = fixture.startClient(&destroyed, &destroyedOn);
    // Every request gets a terminal session: `/end` succeeds, and the verify poll sees a session
    // that is over.
    fixture.network()->body = sessionBody(QStringLiteral("COMPLETED"));

    auto* controller = new TeardownController(nullptr);
    controller->setControlPlane(client);
    controller->setVerifyIntervalMs(5);
    controller->setTeardownGraceMs(2000);
    controller->moveToThread(fixture.thread());
    fixture.own(controller);

    QSignalSpy completed(controller, &TeardownController::teardownCompleted);
    QSignalSpy failed(controller, &TeardownController::teardownFailed);
    QSignalSpy stages(controller, &TeardownController::stageEntered);

    // From the main thread, as `handleReadyForDeletion()` does. Before the HR-01 fix the verify
    // timer was refused its start, teardown never reached `Clear`, and `teardownCompleted()` never
    // fired.
    controller->teardown(QString::fromLatin1(kSessionId), QStringLiteral("client-uuid"));

    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
    QCOMPARE(failed.count(), 0);

    // Teardown ran to its end from the network thread, walked the stages in order, and left the
    // customer's sign-in credential where it was (CUST-08).
    QVERIFY(store->hasToken(TokenStore::accessTokenName()));
    QCOMPARE(store->retrieveToken(TokenStore::accessTokenName()),
             QStringLiteral("an-access-token"));
    QVERIFY(controller->stage() == TeardownStage::Done);
    QCOMPARE(stageSequence(stages),
             QStringLiteral("Disable,Unpair,Verify,Clear,Done"));
}

QTEST_MAIN(TstThreading)

#include "tst_threading.moc"

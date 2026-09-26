#include "log_shipper.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// This fork is Windows-only (`AGENTS.md`: host and client platform is Windows 11 24H2+); the pid
// this file embeds in a spool file name, and uses to skip its own leftover on `adoptLeftovers()`,
// comes straight from `GetCurrentProcessId()` - the same primitive `telemetry.cpp` uses for its
// own executable-path resolution, and for the same reason: this class's worker thread is started
// from `SeatHubTelemetry::start()`, at the `app/main.cpp` call site, before `QCoreApplication`
// exists (Pitfall 15) - `QCoreApplication::applicationPid()` is not available there.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// WR-01/WR-02/CR-02: every qCWarning() call below runs on this class's own worker thread only
// (LogSpool's methods are only ever called from LogShipper's worker - the header's own comment -
// and pauseHandOff()'s timeout warning runs on whatever thread calls it, same as any other log
// line this codebase emits from the client thread). The worker thread is guarded against feeding
// its own qCWarning() calls back into LogShipper's queue by `s_onWorkerThread` (below), so none of
// these calls can recurse into this sink.
Q_LOGGING_CATEGORY(seathubLogShipper, "seathub.log_shipper")

namespace {

double currentEpochSeconds()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

qint64 currentProcessId()
{
    return static_cast<qint64>(GetCurrentProcessId());
}

QString spoolFileName(qint64 pid)
{
    return QStringLiteral("spool-%1.jsonl").arg(pid);
}

QJsonObject lineToJson(const ShippedLine& line)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("level"), static_cast<int>(line.level));
    obj.insert(QStringLiteral("body"), line.body);
    obj.insert(QStringLiteral("logged_at"), line.loggedAt);
    obj.insert(QStringLiteral("session_id"), line.sessionId);
    obj.insert(QStringLiteral("host_id"), line.hostId);
    obj.insert(QStringLiteral("trace_id"), line.traceId);
    return obj;
}

/// `std::nullopt` for a corrupt or truncated line (a crash mid-write to the previous version of
/// this file, before `QSaveFile` made the rewrite atomic) - skipped rather than treated as a
/// reason to give up on every line after it.
std::optional<ShippedLine> lineFromJson(const QJsonObject& obj)
{
    if (!obj.contains(QStringLiteral("body")) || !obj.contains(QStringLiteral("logged_at"))) {
        return std::nullopt;
    }
    ShippedLine line;
    line.level = static_cast<LogLevel>(
        obj.value(QStringLiteral("level")).toInt(static_cast<int>(LogLevel::Info)));
    line.body = obj.value(QStringLiteral("body")).toString();
    line.loggedAt = obj.value(QStringLiteral("logged_at")).toDouble();
    line.sessionId = obj.value(QStringLiteral("session_id")).toString();
    line.hostId = obj.value(QStringLiteral("host_id")).toString();
    line.traceId = obj.value(QStringLiteral("trace_id")).toString();
    return line;
}

QList<ShippedLine> readSpoolFile(const QString& path)
{
    QList<ShippedLine> lines;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return lines;
    }
    while (!file.atEnd()) {
        const QByteArray raw = file.readLine();
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        if (const std::optional<ShippedLine> line = lineFromJson(doc.object())) {
            lines.append(*line);
        }
    }
    return lines;
}

} // namespace

// ===================================================================================================
// LogSpool
// ===================================================================================================

LogSpool::LogSpool(const QString& directory)
    : m_directory(directory)
{
    QDir().mkpath(directory);
    m_path = QDir(directory).filePath(spoolFileName(currentProcessId()));
    m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
    // Our own pid should never collide with a lock another live process holds - this is a very
    // short, effectively non-blocking wait in practice.
    if (!m_lock->tryLock(1000)) {
        // WR-01: SeatHub has no single-instance guard (this class's own header comment) - this
        // lock is what lets adoptLeftovers() tell a leftover spool file from a live one's apart. A
        // failure here means this process's own spool file is unprotected for the rest of this
        // run; log it so a future incident leaves a trail instead of silently reading/writing it
        // unprotected.
        qCWarning(seathubLogShipper) << "could not lock" << (m_path + QStringLiteral(".lock"))
                                      << "- this process's own log spool is now unprotected";
    }
}

LogSpool::~LogSpool() = default;

void LogSpool::append(const ShippedLine& newLine)
{
    QList<ShippedLine> lines = readSpoolFile(m_path);
    lines.append(newLine);

    QList<QByteArray> encoded;
    encoded.reserve(lines.size());
    qint64 total = 0;
    for (const ShippedLine& line : lines) {
        QByteArray entry = QJsonDocument(lineToJson(line)).toJson(QJsonDocument::Compact);
        entry += '\n';
        total += entry.size();
        encoded.append(entry);
    }

    // Drop the oldest first (D-14) - but never the last one, even if it alone exceeds the cap:
    // the newest line is always kept.
    int dropFrom = 0;
    while (dropFrom < encoded.size() - 1 && total > kSpoolBytes) {
        total -= encoded.at(dropFrom).size();
        ++dropFrom;
    }

    QByteArray out;
    for (int i = dropFrom; i < encoded.size(); ++i) {
        out += encoded.at(i);
    }

    QSaveFile file(m_path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(out);
        if (!file.commit()) {
            // WR-02: the only on-disk persistence path this design relies on for "lines wait until
            // sign-in succeeds" (D-14) - log on a failed atomic rename (disk full, permission
            // denied, an antivirus lock) so a silently dropped spool write leaves a trail.
            qCWarning(seathubLogShipper) << "could not commit the log spool write to" << m_path;
        }
    }
    else {
        qCWarning(seathubLogShipper) << "could not open the log spool for writing:" << m_path;
    }
}

QList<ShippedLine> LogSpool::takeAll()
{
    const QList<ShippedLine> lines = readSpoolFile(m_path);
    QFile::remove(m_path);

    const double cutoff
        = currentEpochSeconds() - static_cast<double>(kSpoolMaxAgeDays) * 24.0 * 3600.0;
    QList<ShippedLine> fresh;
    fresh.reserve(lines.size());
    for (const ShippedLine& line : lines) {
        if (line.loggedAt >= cutoff) {
            fresh.append(line);
        }
    }
    return fresh;
}

QList<ShippedLine> LogSpool::adoptLeftovers(const QString& directory)
{
    QList<ShippedLine> result;
    QDir dir(directory);
    if (!dir.exists()) {
        return result;
    }

    const qint64 ownPid = currentProcessId();
    const QStringList files
        = dir.entryList(QStringList(QStringLiteral("spool-*.jsonl")), QDir::Files, QDir::Name);

    for (const QString& name : files) {
        QString pidPart = name;
        pidPart.remove(QStringLiteral("spool-"));
        pidPart.remove(QStringLiteral(".jsonl"));
        bool pidOk = false;
        const qint64 filePid = pidPart.toLongLong(&pidOk);
        if (pidOk && filePid == ownPid) {
            // Our own file - never "adopted" (it is not a leftover; it belongs to this run).
            continue;
        }

        const QString path = dir.filePath(name);
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(100)) {
            // A live process still holds this file's lock - not a leftover.
            continue;
        }
        result.append(readSpoolFile(path));
        QFile::remove(path);
        lock.unlock();
        QFile::remove(path + QStringLiteral(".lock"));
    }
    return result;
}

// ===================================================================================================
// LogShipper::Impl
// ===================================================================================================

namespace {
/// Set for the life of the worker thread only - the sink drops any line logged from ON that
/// thread, so a `qWarning()`/`qCWarning()` emitted BY the worker (there is exactly one, in
/// `scrub()`'s caller path, never inside the sink itself) can never feed back into its own queue.
/// `LogTee`'s own re-entry guard (`log_tee.cpp`) protects the tee's dispatch, not this sink's
/// queue - a message logged from the worker still reaches `dispatch()` fine (the tee's guard is
/// per calling-thread, and the worker is a different thread than whichever one is already mid
/// dispatch); it is this flag, not the tee's, that keeps the worker from feeding itself.
thread_local bool s_onWorkerThread = false;
} // namespace

/// Every mutable field `LogShipper`'s public methods touch, hidden behind the header's own
/// `unique_ptr<Impl>` so `log_shipper.h` never has to declare a `std::thread` or a mutex.
class LogShipper::Impl
{
public:
    struct Ids
    {
        QString sessionId;
        QString hostId;
        QString traceId;
    };

    ~Impl()
    {
        // WR-03: unregister this Impl's LogTee sink BEFORE the rest of this destructor (and this
        // object) finishes going away. `LogShipper::instance()`'s function-local static is
        // constructed at runtime, strictly AFTER `log_tee.cpp`'s own `s_sinks` (a namespace-scope
        // static that completes its trivial, zero-initialised construction during the program's
        // static-initialisation phase, before main() runs) - so it is destroyed strictly BEFORE
        // `s_sinks` is (C++ destroys statics in exact reverse order of construction completion,
        // across translation units). Without this call, any Qt/SDL log call between this
        // destructor running and `s_sinks`'s own destruction would dispatch into a dangling
        // lambda that reads `this->accepting` on freed memory - a genuine use-after-free.
        // `LogTee::removeSink()` also blocks until any dispatch already calling this sink on
        // another thread has finished (its own WR-07 guarantee), so this is safe to call
        // unconditionally, even mid-shutdown, and before `stop()` below.
        if (sinkHandle != 0) {
            LogTee::removeSink(sinkHandle);
            sinkHandle = 0;
        }
        stop();
    }

    void start(HandOff handOff)
    {
        stop(); // idempotent reset (tests only; production calls start() once).

        handOffFn = std::move(handOff);
        shippedBytes.store(0);
        capReached.store(false);
        canShip.store(false);
        handOffPaused.store(false);
        holdForTests.store(false);
        enqueueSequence.store(0);
        processedSequence.store(0);
        stopRequested.store(false);

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.clear();
        }

        const QString directory = spoolDirectoryOverride.isEmpty()
            ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                  .filePath(QStringLiteral("log-spool"))
            : spoolDirectoryOverride;
        QDir().mkpath(directory);

        // Adopt leftovers BEFORE this run's own spool file exists, so `adoptLeftovers()` never
        // sees (and skips) our own about-to-be-created file.
        QList<ShippedLine> adopted = LogSpool::adoptLeftovers(directory);

        spool = std::make_unique<LogSpool>(directory);
        for (const ShippedLine& line : adopted) {
            spool->append(line);
        }

        if (sinkHandle == 0) {
            // D-16/log_tee.h's own contract: registered exactly once, ever, for the life of the
            // process - never removed (this class mirrors `LogTee::install()`'s own "installed
            // for the life of the process" pattern rather than `removeSink()`'s).
            sinkHandle = LogTee::addSink([this](LogLevel level, int category, int priority,
                                              const QString& text) {
                sink(level, category, priority, text);
            });
        }

        accepting.store(true);
        worker = std::thread([this] { run(); });
        workerRunning.store(true);
    }

    void setCanShip(bool value)
    {
        canShip.store(value);
        // Wakes the worker immediately (rather than waiting for the next 200ms poll) so a
        // sign-in that flips this to `true` with nothing newly logged still drains the spool
        // promptly (Task 1's own tracer: three lines already spooled, then `setCanShip(true)`
        // with no fourth line).
        queueCv.notify_all();
    }

    void pauseHandOff()
    {
        handOffPaused.store(true);
        std::unique_lock<std::mutex> lock(pauseMutex);
        const int overrideMs = pauseTimeoutMsOverride.load();
        const std::chrono::milliseconds timeout = overrideMs >= 0
            ? std::chrono::milliseconds(overrideMs)
            : std::chrono::milliseconds(5000);
        const bool resumed
            = pauseCv.wait_for(lock, timeout, [this] { return !inHandOff.load(); });
        if (!resumed) {
            // CR-02: a bounded fallback, matching drainBeforeSignOut()'s own pattern - never hang
            // the calling (client) thread forever, whether the in-flight hand-off is merely slow
            // or genuinely wedged (a stuck HandOff callback). Proceeding "as if resumed" is safe:
            // the caller (adoptFirstDsn()'s re-init) only needs the WORKER to stop calling the OLD
            // handOffFn before it closes the SDK, and handOffPaused is already true regardless of
            // whether this wait itself was ever satisfied.
            qCWarning(seathubLogShipper)
                << "pauseHandOff() timed out after" << timeout.count()
                << "ms waiting for an in-flight hand-off to finish; proceeding as if resumed";
        }
    }

    void resumeHandOff()
    {
        handOffPaused.store(false);
        queueCv.notify_all();
    }

    void drainBeforeSignOut()
    {
        const quint64 target = enqueueSequence.load();
        std::unique_lock<std::mutex> lock(seqMutex);
        seqCv.wait_for(lock, std::chrono::seconds(5),
            [this, target] { return processedSequence.load() >= target; });
    }

    void stop()
    {
        accepting.store(false);
        if (!workerRunning.exchange(false)) {
            return;
        }
        stopRequested.store(true);
        queueCv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void publishIds(const QString& sessionId, const QString& hostId, const QString& traceId)
    {
        auto next = std::make_shared<const Ids>(Ids{ sessionId, hostId, traceId });
        std::atomic_store(&currentIds, next);
    }

    void setSpoolDirectoryForTests(const QString& directory) { spoolDirectoryOverride = directory; }

    void holdWorkerForTests(bool hold)
    {
        holdForTests.store(hold);
        queueCv.notify_all();
    }

    qint64 shippedBytesForTests() const { return shippedBytes.load(); }

    void setPauseTimeoutForTests(int milliseconds) { pauseTimeoutMsOverride.store(milliseconds); }

private:
    void sink(LogLevel level, int /*category*/, int /*priority*/, const QString& text)
    {
        // Contract (log_tee.h): never blocks, never logs, never touches LogTee's own sink list.
        if (!accepting.load(std::memory_order_acquire)) {
            return;
        }
        if (level == LogLevel::Debug) {
            // Debug stays in the local file only (D-14) - it never reaches the queue or spool.
            return;
        }
        if (s_onWorkerThread) {
            return;
        }

        ShippedLine line;
        line.level = level;
        line.body = text;
        line.loggedAt = currentEpochSeconds();
        const std::shared_ptr<const Ids> ids = std::atomic_load(&currentIds);
        if (ids) {
            line.sessionId = ids->sessionId;
            line.hostId = ids->hostId;
            line.traceId = ids->traceId;
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (queue.size() >= static_cast<size_t>(LogShipper::kQueueLines)) {
                queue.pop_front();
                // WR-04: the popped line was already counted by an earlier fetch_add() below, on
                // ITS OWN push - but it will now never reach run()'s processLine() (only the
                // lines still in the queue do). Compensate here, or drainBeforeSignOut()'s target
                // (captured against this same counter) can never again be satisfied by
                // processedSequence once even a single line has ever been dropped by this cap -
                // permanently desynchronizing the two counters for the rest of the process's life.
                enqueueSequence.fetch_sub(1);
            }
            queue.push_back(std::move(line));
            enqueueSequence.fetch_add(1);
        }
        queueCv.notify_one();
    }

    void run()
    {
        s_onWorkerThread = true;
        for (;;) {
            std::deque<ShippedLine> batch;
            bool shuttingDown = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return stopRequested.load() || (!holdForTests.load() && !queue.empty());
                });
                shuttingDown = stopRequested.load();
                if (!shuttingDown && holdForTests.load()) {
                    continue;
                }
                batch.swap(queue);
            }

            if (shuttingDown) {
                // SEATHUB § E.3 step 6: never attempt a hand-off during shutdown - only persist,
                // so nothing captured is lost, but nothing new ships this late either.
                for (const ShippedLine& raw : batch) {
                    ShippedLine line = raw;
                    line.body = LogShipper::scrub(line.body);
                    spool->append(line);
                }
                processedSequence.store(enqueueSequence.load());
                seqCv.notify_all();
                return;
            }

            // Drain whatever is already spooled BEFORE this cycle's own batch (SEATHUB § E.3
            // step 2: the spool always drains ahead of anything newly arriving), and - just as
            // important - on every wake, even an EMPTY batch: `setCanShip(true)` with nothing new
            // logged since is exactly how a sign-in drains lines that piled up before it (Task 1's
            // own tracer). `holdWorkerForTests()` never suppresses this - only the queue's own
            // dequeue is held for that seam.
            if (!handOffPaused.load() && canShip.load()) {
                drainSpool();
            }

            for (const ShippedLine& raw : batch) {
                processLine(raw);
                processedSequence.fetch_add(1);
                seqCv.notify_all();
            }
        }
    }

    /// Hands off everything currently in the spool, oldest first, stopping (and putting the
    /// remainder straight back, in order) the moment a hand-off fails or `canShip`/the pause flag
    /// flips mid-drain.
    void drainSpool()
    {
        QList<ShippedLine> spooled = spool->takeAll();
        for (int i = 0; i < spooled.size(); ++i) {
            if (handOffPaused.load() || !canShip.load()) {
                requeue(spooled, i);
                return;
            }
            if (!handOffOne(spooled.at(i))) {
                requeue(spooled, i);
                return;
            }
        }
    }

    void processLine(const ShippedLine& rawLine)
    {
        ShippedLine line = rawLine;
        line.body = LogShipper::scrub(line.body);

        if (handOffPaused.load() || !canShip.load()) {
            spool->append(line);
            return;
        }

        if (!handOffOne(line)) {
            spool->append(line);
        }
    }

    /// Puts `spooled[from..]` back into the spool, in order - used whenever draining stops
    /// partway through (a hand-off failed, or `canShip`/pause flipped mid-drain).
    void requeue(const QList<ShippedLine>& spooled, int from)
    {
        for (int j = from; j < spooled.size(); ++j) {
            spool->append(spooled.at(j));
        }
    }

    bool handOffOne(const ShippedLine& line)
    {
        if (capReached.load()) {
            // E.2: past the per-run cap, every line is silently dropped (not re-spooled - a
            // spool that could never drain again would only grow without bound for the rest of
            // this run).
            return true;
        }

        const qint64 bodyBytes = line.body.toUtf8().size();
        if (shippedBytes.load() + bodyBytes > LogShipper::kShippedBytesPerRun) {
            if (!capReached.exchange(true)) {
                ShippedLine capLine;
                capLine.level = LogLevel::Warning;
                capLine.body = QStringLiteral("log cap reached");
                capLine.loggedAt = currentEpochSeconds();
                capLine.sessionId = line.sessionId;
                capLine.hostId = line.hostId;
                capLine.traceId = line.traceId;
                callHandOff(capLine);
            }
            return true;
        }

        const bool ok = callHandOff(line);
        if (ok) {
            shippedBytes.fetch_add(bodyBytes);
        }
        return ok;
    }

    bool callHandOff(const ShippedLine& line)
    {
        inHandOff.store(true);
        const bool ok = handOffFn ? handOffFn(line) : false;
        {
            // CR-02: publish `inHandOff = false` and notify while holding the SAME mutex
            // `pauseHandOff()` locks before checking its own predicate/blocking - per the
            // condition-variable contract (a modification must be published under the mutex the
            // waiter uses to correctly serialize against its own lock-check-block sequence), a
            // notify that does not hold this mutex can race a waiter between its predicate check
            // and the actual wait, losing the notification. Holding `pauseMutex` here closes that
            // race outright, independent of the bounded-wait fallback above.
            std::lock_guard<std::mutex> lock(pauseMutex);
            inHandOff.store(false);
            pauseCv.notify_all();
        }
        return ok;
    }

    HandOff handOffFn;
    LogTee::SinkHandle sinkHandle = 0;
    std::atomic<int> pauseTimeoutMsOverride{ -1 };

    std::atomic<bool> accepting{ false };
    std::atomic<bool> canShip{ false };
    std::atomic<bool> handOffPaused{ false };
    std::atomic<bool> inHandOff{ false };
    std::atomic<bool> holdForTests{ false };
    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> workerRunning{ false };
    std::atomic<qint64> shippedBytes{ 0 };
    std::atomic<bool> capReached{ false };
    std::atomic<quint64> enqueueSequence{ 0 };
    std::atomic<quint64> processedSequence{ 0 };

    std::shared_ptr<const Ids> currentIds;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<ShippedLine> queue;

    std::mutex pauseMutex;
    std::condition_variable pauseCv;

    std::mutex seqMutex;
    std::condition_variable seqCv;

    std::thread worker;
    std::unique_ptr<LogSpool> spool;
    QString spoolDirectoryOverride;
};

// ===================================================================================================
// LogShipper
// ===================================================================================================

LogShipper& LogShipper::instance()
{
    static LogShipper shipper;
    return shipper;
}

LogShipper::LogShipper()
    : m_impl(std::make_unique<Impl>())
{
}

LogShipper::~LogShipper()
{
    // Belt-and-suspenders (see the header's own comment on `stop()`): the real shutdown path is
    // `SeatHubClient`'s destructor calling `stop()` explicitly, but a joinable `std::thread` left
    // attached to this singleton at static destruction would otherwise call `std::terminate()`.
    m_impl->stop();
}

void LogShipper::start(HandOff handOff)
{
    m_impl->start(std::move(handOff));
}

void LogShipper::setCanShip(bool canShip)
{
    m_impl->setCanShip(canShip);
}

void LogShipper::pauseHandOff()
{
    m_impl->pauseHandOff();
}

void LogShipper::resumeHandOff()
{
    m_impl->resumeHandOff();
}

void LogShipper::drainBeforeSignOut()
{
    m_impl->drainBeforeSignOut();
}

void LogShipper::stop()
{
    m_impl->stop();
}

void LogShipper::publishIds(const QString& sessionId, const QString& hostId, const QString& traceId)
{
    m_impl->publishIds(sessionId, hostId, traceId);
}

void LogShipper::setSpoolDirectoryForTests(const QString& directory)
{
    m_impl->setSpoolDirectoryForTests(directory);
}

void LogShipper::holdWorkerForTests(bool hold)
{
    m_impl->holdWorkerForTests(hold);
}

qint64 LogShipper::shippedBytesForTests() const
{
    return m_impl->shippedBytesForTests();
}

void LogShipper::setPauseTimeoutForTests(int milliseconds)
{
    m_impl->setPauseTimeoutForTests(milliseconds);
}

QString LogShipper::scrub(const QString& text)
{
    QString result = text;

    // Upstream's own two redactions (`main.cpp`:71-72, 97-99) run only on the disk-log path -
    // reapplied here so the shipper never depends on that ordering.
    static const QRegularExpression rikey(QStringLiteral("&rikey=\\w+"));
    static const QRegularExpression rikeyId(QStringLiteral("&rikeyid=[\\d-]+"));
    result.replace(rikey, QStringLiteral("&rikey=REDACTED"));
    result.replace(rikeyId, QStringLiteral("&rikeyid=REDACTED"));

    // This fork's pairing-handshake query values (`app/backend/nvpairingmanager.cpp`), which
    // `nvhttp.cpp:514` logs verbatim as part of the full request URL: `qInfo() << "Executing
    // request:" << url.toString();`. Each value is hex-encoded (a salt, a certificate, an
    // encrypted challenge or response, or the pairing secret) - never digits-only, so this cannot
    // collide with an IPv4 address (D-09 keeps those).
    static const QRegularExpression salt(QStringLiteral("&salt=[0-9a-fA-F]+"));
    static const QRegularExpression clientCert(QStringLiteral("&?clientcert=[0-9a-fA-F]+"));
    static const QRegularExpression clientChallenge(QStringLiteral("&clientchallenge=[0-9a-fA-F]+"));
    static const QRegularExpression serverChallengeResp(
        QStringLiteral("&serverchallengeresp=[0-9a-fA-F]+"));
    static const QRegularExpression clientPairingSecret(
        QStringLiteral("&clientpairingsecret=[0-9a-fA-F]+"));
    result.replace(salt, QStringLiteral("&salt=REDACTED"));
    result.replace(clientCert, QStringLiteral("&clientcert=REDACTED"));
    result.replace(clientChallenge, QStringLiteral("&clientchallenge=REDACTED"));
    result.replace(serverChallengeResp, QStringLiteral("&serverchallengeresp=REDACTED"));
    result.replace(clientPairingSecret, QStringLiteral("&clientpairingsecret=REDACTED"));

    // `Authorization: Bearer <token>` (D-09).
    static const QRegularExpression bearer(
        QStringLiteral("Authorization:\\s*Bearer\\s+\\S+"), QRegularExpression::CaseInsensitiveOption);
    result.replace(bearer, QStringLiteral("Authorization: Bearer REDACTED"));

    // A 4-digit Sunshine PIN written next to the word "pin" (D-09) - matches "PIN: 1234",
    // "pin=1234", "pin 1234", but not a bare 4-digit number elsewhere in the line (an IPv4 octet,
    // a port, a status code), which is exactly the point: only a PIN NAMED as one is redacted.
    static const QRegularExpression pin(
        QStringLiteral("\\bpin\\b\\s*[:=]?\\s*\\d{4}\\b"), QRegularExpression::CaseInsensitiveOption);
    result.replace(pin, QStringLiteral("PIN REDACTED"));

    // A PEM block (a pairing certificate) - DOTALL so it spans the line's embedded '\n's.
    static const QRegularExpression pem(
        QStringLiteral("-----BEGIN [A-Z ]+-----[\\s\\S]*?-----END [A-Z ]+-----"));
    result.replace(pem, QStringLiteral("-----BEGIN CERTIFICATE----- REDACTED -----END CERTIFICATE-----"));

    return result;
}

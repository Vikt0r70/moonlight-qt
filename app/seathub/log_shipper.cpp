#include "log_shipper.h"

#include "reporting_consent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QSharedPointer>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
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

QMutex redactionsMutex;
QSharedPointer<const QStringList> currentRedactions;

QString scrubRig(const QString& text)
{
    QSharedPointer<const QStringList> snapshot;
    {
        QMutexLocker lock(&redactionsMutex);
        snapshot = currentRedactions;
    }
    QString result = text;
    if (snapshot) for (const auto& literal : *snapshot) {
        if (!literal.isEmpty()) result.replace(literal, QStringLiteral("[rig]"));
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------
// T020/SC7/N1 (owner correction C3, `docs/spec/agents.md`:351, ADR-0071): a customer's IP address is
// not published or retained off the rig. Everything below runs ONLY on the two paths that leave the
// machine - the Sentry payload (`LogShipper::scrub()` in `processLine()`/`drainSpool()`) and the
// off-rig support spool (`LogSpool::append`) - never on the rig's own local file log, which keeps
// the address by that same ruling.
//
// Deliberately NOT scrubbed:
//   - this machine's own endpoints (loopback, the wildcard): a URL, a DSN and a bind fact stay
//     readable, and none of them identifies the customer;
//   - ports, versions, ids: `203.0.113.7:47984` keeps `:47984`, and `0.1.37` is not address-shaped
//     at all (three dotted parts, not four).
//
// Implemented by hand rather than with QHostAddress: this translation unit is compiled into test
// suites that link Qt Core only (`tests/tst_log_shipper.pro`, `tests/tst_log_tee.pro`), and
// QHostAddress lives in Qt Network.
// ---------------------------------------------------------------------------------------------------

bool isHexGroup(const QString& group, int maxLength)
{
    if (group.isEmpty() || group.size() > maxLength) {
        return false;
    }
    for (const QChar c : group) {
        const QChar lower = c.toLower();
        if (!((lower >= QLatin1Char('0') && lower <= QLatin1Char('9'))
              || (lower >= QLatin1Char('a') && lower <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

QStringList splitGroups(const QString& text)
{
    return text.isEmpty() ? QStringList() : text.split(QLatin1Char(':'));
}

bool isIpv4Literal(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) {
        return false;
    }
    for (const QString& part : parts) {
        if (part.isEmpty() || part.size() > 3) {
            return false;
        }
        for (const QChar c : part) {
            if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
                return false;
            }
        }
        if (part.toInt() > 255) {
            return false;
        }
    }
    return true;
}

bool isIpv6Literal(const QString& text)
{
    if (!text.contains(QLatin1Char(':'))) {
        return false;
    }

    QString work = text;
    int groups = 0;
    const int lastColon = work.lastIndexOf(QLatin1Char(':'));
    const QString tail = work.mid(lastColon + 1);
    if (tail.contains(QLatin1Char('.'))) {
        // RFC4291 embedded IPv4 tail ("::ffff:203.0.113.7") - two groups' worth of address.
        if (!isIpv4Literal(tail)) {
            return false;
        }
        groups += 2;
        work = work.left(lastColon);
    }

    const int compression = work.indexOf(QStringLiteral("::"));
    if (compression >= 0) {
        // At most one "::", and it must stand for at least one all-zero group.
        if (work.indexOf(QStringLiteral("::"), compression + 1) >= 0) {
            return false;
        }
        for (const QString& group : splitGroups(work.left(compression))) {
            if (!isHexGroup(group, 4)) {
                return false;
            }
            ++groups;
        }
        for (const QString& group : splitGroups(work.mid(compression + 2))) {
            if (!isHexGroup(group, 4)) {
                return false;
            }
            ++groups;
        }
        return groups < 8;
    }

    for (const QString& group : splitGroups(work)) {
        if (!isHexGroup(group, 4)) {
            return false;
        }
        ++groups;
    }
    // Without a "::" compression every one of the eight groups must be written out - which is also
    // what stops a clock (`12:30:45`) and a MAC-ish six-group run from ever reading as an address.
    return groups == 8;
}

/// True for an address literal that identifies the customer (and so must go), false for one that
/// identifies only this machine's own endpoint or is not an address at all.
bool isCustomerAddress(const QString& candidate)
{
    if (isIpv4Literal(candidate)) {
        const int firstOctet = candidate.section(QLatin1Char('.'), 0, 0).toInt();
        // The loopback block and the wildcard are local facts, and no customer is ever dialled at
        // either.
        return firstOctet != 127 && firstOctet != 0;
    }
    if (!isIpv6Literal(candidate)) {
        return false;
    }

    const QString lowered = candidate.toLower();
    const QString mappedPrefix = QStringLiteral("::ffff:");
    if (lowered.startsWith(mappedPrefix)) {
        // An IPv4-mapped IPv6 literal is the same customer address written differently.
        return isCustomerAddress(lowered.mid(mappedPrefix.size()));
    }
    return lowered != QStringLiteral("::") && lowered != QStringLiteral("::1");
}

/// Replaces every match the callback maps to a replacement, leaving the rest of the text exact.
QString replaceMatches(const QString& text, const QRegularExpression& pattern,
                       const std::function<QString(const QRegularExpressionMatch&)>& decide)
{
    QString out;
    out.reserve(text.size());
    int last = 0;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        out += text.mid(last, match.capturedStart() - last);
        out += decide(match);
        last = match.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

/// A bare IPv6 literal with a port appended and no brackets (`2001:db8::5:47989`) is not itself a
/// valid address once the port is on it, so the port is kept and the address part replaced.
QString scrubIpv6Candidate(const QString& candidate)
{
    if (isCustomerAddress(candidate)) {
        return QStringLiteral("[address]");
    }
    static const QRegularExpression portTail(QStringLiteral("^(.*):(\\d{1,5})$"));
    const QRegularExpressionMatch match = portTail.match(candidate);
    if (match.hasMatch() && isCustomerAddress(match.captured(1))) {
        return QStringLiteral("[address]:") + match.captured(2);
    }
    return candidate;
}

QString scrubAddresses(const QString& text)
{
    QString result = text;

    // 1. A bracketed IPv6 literal, port or no port: the whole bracket pair goes, the port stays.
    static const QRegularExpression bracketed(QStringLiteral("\\[([0-9A-Fa-f:.]+)\\]"));
    result = replaceMatches(result, bracketed, [](const QRegularExpressionMatch& match) {
        return isCustomerAddress(match.captured(1)) ? QStringLiteral("[address]")
                                                    : match.captured(0);
    });

    // 2. An IPv4-mapped IPv6 literal as one unit, before the dotted quad inside it is considered.
    static const QRegularExpression mapped(
        QStringLiteral("::ffff:\\d{1,3}(?:\\.\\d{1,3}){3}"));
    result = replaceMatches(result, mapped, [](const QRegularExpressionMatch& match) {
        return isCustomerAddress(match.captured(0)) ? QStringLiteral("[address]")
                                                    : match.captured(0);
    });

    // 3. A dotted quad, including the backslash-escaped form a regex-shaped log line carries
    //    (`203\.0\.113\.7`). Three parts (a version, `0.1.37`) never match.
    static const QRegularExpression ipv4(
        QStringLiteral("\\d{1,3}(?:\\\\?\\.\\d{1,3}){3}"));
    result = replaceMatches(result, ipv4, [](const QRegularExpressionMatch& match) {
        QString plain = match.captured(0);
        plain.remove(QLatin1Char('\\'));
        if (!isIpv4Literal(plain)) {
            return match.captured(0);
        }
        return isCustomerAddress(plain) ? QStringLiteral("[address]") : match.captured(0);
    });

    // 4. Any remaining bare IPv6 run - written out in full, compressed, or carrying a port.
    static const QRegularExpression ipv6Run(QStringLiteral("[0-9A-Fa-f:.]+"));
    result = replaceMatches(result, ipv6Run, [](const QRegularExpressionMatch& match) {
        const QString candidate = match.captured(0);
        if (candidate.count(QLatin1Char(':')) < 2) {
            return candidate;
        }
        return scrubIpv6Candidate(candidate);
    });

    return result;
}

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
    // Plan 09 (ADR-0072 item 7): the diagnostic lane's own fields. Written unconditionally, so a
    // reader that predates them still sees an ordinary line as ordinary (`attrs` empty, flag
    // false) and never has to guess from a missing key.
    obj.insert(QStringLiteral("attrs"), line.attrs);
    obj.insert(QStringLiteral("diagnostic"), line.diagnostic);
    obj.insert(QStringLiteral("account_id"), line.accountId);
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
    // Old spool files - written by the release before the diagnostic lane existed - carry neither
    // key: both default to their empty/false values, which is exactly what an ordinary line is.
    line.attrs = obj.value(QStringLiteral("attrs")).toObject();
    line.diagnostic = obj.value(QStringLiteral("diagnostic")).toBool(false);
    // A spool file from before T023 has no account: empty means "not account-scoped", and such a
    // line ships as it always did.
    line.accountId = obj.value(QStringLiteral("account_id")).toString();
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
            // T020/SC7: a spool file written by an OLDER release still carries the customer's
            // address. This spool is a copy held off the rig (a support copy waiting to be sent),
            // so it is sanitised here - at read, re-encoded on the next append, and scrubbed again
            // on the way out by `processLine()`. Every older copy is therefore cleaned before it is
            // ever re-sent, and a copy nobody ever re-sends is cleaned the first time it is read.
            ShippedLine sanitised = *line;
            sanitised.body = LogShipper::scrub(sanitised.body);
            sanitised.attrs = LogShipper::scrubAttributes(sanitised.attrs);
            lines.append(sanitised);
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
    m_pendingLines.store(readSpoolFile(m_path).size());
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
    // T020/SC7: the spool is a copy that outlives this machine's own file log and is written for
    // the express purpose of being sent off the rig, so BOTH the body and the diagnostic
    // attributes are sanitised here, once, for every line that lands in it - whether it arrived
    // from a live capture or from the shutdown flush. `readSpoolFile()` applies the same treatment
    // to lines an older release already wrote.
    ShippedLine line = newLine;
    line.body = LogShipper::scrub(line.body);
    line.attrs = LogShipper::scrubAttributes(line.attrs);
    lines.append(line);

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
        else { m_pendingLines.store(encoded.size() - dropFrom); }
    }
    else {
        qCWarning(seathubLogShipper) << "could not open the log spool for writing:" << m_path;
    }
}

QList<ShippedLine> LogSpool::takeAll()
{
    const QList<ShippedLine> lines = readSpoolFile(m_path);
    if (QFile::remove(m_path)) m_pendingLines.store(0);

    const double cutoff
        = currentEpochSeconds() - static_cast<double>(kSpoolMaxAgeDays) * 24.0 * 3600.0;
    QList<ShippedLine> fresh;
    fresh.reserve(lines.size());
    for (const ShippedLine& line : lines) {
        if (line.loggedAt >= cutoff) {
            fresh.append(line);
        }
        else {
            // D-13: a line dropped for age is a line this run will never deliver. Counted, not
            // logged - the launch record's `dropped_spool_age` is where a reader finds it.
            // Atomic: this runs on the worker thread while that record is read from the client
            // thread (LogSpool's own comment on `droppedAgeLines()`).
            m_droppedAgeLines.fetch_add(1);
        }
    }
    return fresh;
}

void LogSpool::appendAdopted(const QList<ShippedLine>& lines)
{
    // The same cutoff `takeAll()` applies at drain, computed the same way - applied here instead
    // because a line that is ALREADY dead on arrival should be counted before anything reads the
    // counter (Plan 09: `noteLaunch()` reads it right after `LogShipper::start()` returns), and
    // because dropping it now keeps `takeAll()` from counting the same line a second time.
    const double cutoff
        = currentEpochSeconds() - static_cast<double>(kSpoolMaxAgeDays) * 24.0 * 3600.0;
    for (const ShippedLine& line : lines) {
        if (line.loggedAt >= cutoff) {
            append(line);
        }
        else {
            m_droppedAgeLines.fetch_add(1);
        }
    }
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
        droppedQueueCount.store(0);
        capReachedCount.store(0);
        diagnosticEnqueued.store(0);
        adoptedSpoolLines.store(0);
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
        adoptedSpoolLines.store(adopted.size());

        spool = std::make_unique<LogSpool>(directory);
        // Plan 09 (ADR-0072 item 8): adopted lines go through the age rule HERE rather than at
        // the first drain, so `dropped_spool_age` already counts the dead-on-arrival ones by the
        // time `SeatHubTelemetry::start()` reaches `noteLaunch()` and builds the launch record.
        spool->appendAdopted(adopted);

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

    void publishAccount(const QString& accountId, const std::function<void()>& underLock)
    {
        // The worker holds `accountMutex` from its account check until the SDK has taken the
        // line, so waiting here means any line already past that check is emitted under the SDK
        // user it was checked against, and the next line is checked against this account.
        std::lock_guard<std::mutex> lock(accountMutex);
        if (underLock) {
            underLock();
        }
        std::atomic_store(&currentAccount, std::make_shared<const QString>(accountId));
    }

    qint64 droppedAccountTotal() const { return droppedAccountCount.load(); }

    void setSpoolDirectoryForTests(const QString& directory) { spoolDirectoryOverride = directory; }

    void holdWorkerForTests(bool hold)
    {
        holdForTests.store(hold);
        queueCv.notify_all();
    }

    qint64 shippedBytesForTests() const { return shippedBytes.load(); }

    // D-13 / plan 09: the launch record's counters. Each is an informational count read from
    // whichever thread asks (the launch record reads them at start, a test after a run).
    qint64 droppedQueueTotal() const { return droppedQueueCount.load(); }
    qint64 capReachedTotal() const { return capReachedCount.load(); }
    int adoptedLines() const { return adoptedSpoolLines.load(); }
    int droppedSpoolAgeTotal() const { return spool ? spool->droppedAgeLines() : 0; }
    int backlogSpoolLines() const { return spool ? spool->pendingLines() : 0; }

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
        // Capture-time redaction survives a terminal clear before the worker drains this line.
        line.body = scrubRig(text);
        line.loggedAt = currentEpochSeconds();
        line.accountId = currentAccountId();
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
                // D-13 (plan 09): counted for the launch record - a line the queue dropped is a
                // line this run will never deliver, and a reader has to be able to see that.
                droppedQueueCount.fetch_add(1);
            }
            queue.push_back(std::move(line));
            enqueueSequence.fetch_add(1);
        }
        queueCv.notify_one();
    }

public:
    /// ADR-0072 item 7 / plan 09: the diagnostic lane's entry point. The line joins the same
    /// queue as a tee line - same scrub, same `canShip`/pause/spool rules, same drop-oldest
    /// queue cap - and differs only in its bounds: it is counted against
    /// `kDiagnosticLinesPerRun` here and exempt from the byte caps in `handOffOne()`.
    void enqueueDiagnostic(const QString& body, LogLevel level, const QJsonObject& attrs)
    {
        if (!accepting.load(std::memory_order_acquire)) {
            return;
        }
        if (level == LogLevel::Debug) {
            // The same rule the tee's own sink applies: debug never leaves this machine.
            return;
        }
        if (s_onWorkerThread) {
            return;
        }

        // V30: the lane's per-run bound is a COUNT, checked before the line is built. The cap is
        // counted once - the trip, not every line it then drops - so `cap_reached` reads "a cap
        // was reached N times", the same shape as the byte cap's own trip.
        const int queued = diagnosticEnqueued.fetch_add(1) + 1;
        if (queued > LogShipper::kDiagnosticLinesPerRun) {
            if (queued == LogShipper::kDiagnosticLinesPerRun + 1) {
                capReachedCount.fetch_add(1);
            }
            return;
        }

        ShippedLine line;
        line.level = level;
        line.body = body;
        line.loggedAt = currentEpochSeconds();
        line.attrs = attrs;
        line.diagnostic = true;
        line.accountId = currentAccountId();
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
                enqueueSequence.fetch_sub(1);
                droppedQueueCount.fetch_add(1);
            }
            queue.push_back(std::move(line));
            enqueueSequence.fetch_add(1);
        }
        queueCv.notify_one();
    }

private:
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
            if (!handOffPaused.load() && canShip.load() && ReportingConsent::coveredUploadsAllowed()) {
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
            // Per line, so a sign-out never waits behind the whole spool. The gate is re-checked
            // under the lock: it may have flipped while this thread waited for it.
            std::lock_guard<std::mutex> accountLock(accountMutex);
            if (handOffPaused.load() || !canShip.load() || !ReportingConsent::coveredUploadsAllowed()) {
                requeue(spooled, i);
                return;
            }
            if (belongsToAnotherAccount(spooled.at(i))) {
                continue;
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
        line.attrs = LogShipper::scrubAttributes(line.attrs);

        // The account check and the whole SDK emission are one step with respect to
        // `publishAccount()`, so the SDK user cannot change between the two (T070 / A2).
        std::lock_guard<std::mutex> accountLock(accountMutex);
        if (handOffPaused.load() || !canShip.load() || !ReportingConsent::coveredUploadsAllowed()) {
            spool->append(line);
            return;
        }

        if (belongsToAnotherAccount(line)) {
            return;
        }

        if (!handOffOne(line)) {
            spool->append(line);
        }
    }

    /// T023: the account signed in right now ("" when none). Called on any thread.
    QString currentAccountId() const
    {
        const std::shared_ptr<const QString> account = std::atomic_load(&currentAccount);
        return account ? *account : QString();
    }

    /// T023: true, and counted, for a line logged under one account that is due to ship while a
    /// different one is signed in. Such a line is dropped, not re-spooled: it can never ship
    /// correctly, and the SDK would stamp it with the wrong user. A line with no account (logged
    /// before any sign-in, or spooled by an older build) ships as it always did, and the same
    /// account signing back in still ships what it held. Checked only once the line is otherwise
    /// free to ship, so a sign-out never drops a held line early.
    bool belongsToAnotherAccount(const ShippedLine& line)
    {
        if (line.accountId.isEmpty() || line.accountId == currentAccountId()) {
            return false;
        }
        droppedAccountCount.fetch_add(1);
        return true;
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
        // ADR-0072 item 7 / plan 09: a diagnostic line is exempt from the byte caps entirely. It
        // was already bounded at enqueue by `kDiagnosticLinesPerRun`, and the two byte bounds
        // below exist for the open-ended tee traffic - applied to the diagnostic lane they would
        // silence exactly the structured record of what failed, most of all the cap line itself.
        if (line.diagnostic) {
            return callHandOff(line);
        }

        if (capReached.load()) {
            // E.2: past the per-run cap, every line is silently dropped (not re-spooled - a
            // spool that could never drain again would only grow without bound for the rest of
            // this run).
            return true;
        }

        const qint64 bodyBytes = line.body.toUtf8().size();
        if (shippedBytes.load() + bodyBytes > LogShipper::kShippedBytesPerRun) {
            if (!capReached.exchange(true)) {
                // D-13: the launch record's `cap_reached` counts the trips, and this is the one
                // the byte cap can make in a run.
                capReachedCount.fetch_add(1);
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
    /// D-13 (plan 09): the three cumulative counters the launch record reports. `droppedQueue`
    /// counts lines the queue's own drop-oldest cap popped, `capReachedCount` counts every cap
    /// trip (the 8 MiB byte cap and the diagnostic lane's count cap alike), and
    /// `diagnosticEnqueued` is what `kDiagnosticLinesPerRun` is measured against.
    std::atomic<qint64> droppedQueueCount{ 0 };
    std::atomic<qint64> capReachedCount{ 0 };
    std::atomic<int> diagnosticEnqueued{ 0 };
    /// How many lines `start()` adopted from dead processes' spool files (D-13's
    /// `spool_lines_adopted`).
    std::atomic<int> adoptedSpoolLines{ 0 };
    std::atomic<quint64> enqueueSequence{ 0 };
    std::atomic<quint64> processedSequence{ 0 };

    std::shared_ptr<const Ids> currentIds;
    std::shared_ptr<const QString> currentAccount;
    std::atomic<qint64> droppedAccountCount{ 0 };
    /// Held by the worker from the account comparison through the end of the hand-off, and by
    /// `publishAccount()` around the SDK user change. Lock order: this, then the SDK's own locks.
    /// Never held across `pauseHandOff()` or `drainBeforeSignOut()`.
    std::mutex accountMutex;

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

void LogShipper::publishAccount(const QString& accountId, const std::function<void()>& underLock)
{
    m_impl->publishAccount(accountId, underLock);
}

qint64 LogShipper::droppedAccountCount() const
{
    return m_impl->droppedAccountTotal();
}

void LogShipper::enqueueDiagnostic(const QString& body, LogLevel level, const QJsonObject& attrs)
{
    m_impl->enqueueDiagnostic(body, level, attrs);
}

qint64 LogShipper::droppedQueueCount() const
{
    return m_impl->droppedQueueTotal();
}

int LogShipper::backlogSpoolLines() const
{
    return m_impl->backlogSpoolLines();
}

qint64 LogShipper::droppedSpoolAgeCount() const
{
    return m_impl->droppedSpoolAgeTotal();
}

qint64 LogShipper::capReachedCount() const
{
    return m_impl->capReachedTotal();
}

int LogShipper::adoptedSpoolLines() const
{
    return m_impl->adoptedLines();
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

void LogShipper::setRedactions(const QStringList& literals)
{
    auto snapshot = QSharedPointer<const QStringList>(new const QStringList(literals));
    QMutexLocker lock(&redactionsMutex);
    currentRedactions.swap(snapshot);
}
void LogShipper::clearRedactions()
{
    QMutexLocker lock(&redactionsMutex);
    currentRedactions.clear();
}

QString LogShipper::scrub(const QString& text)
{
    QString result = scrubRig(text);

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
    // IN-02: all five consistently `&?`-prefixed (matching `clientCert`'s own, already-safer
    // form) - `nvpairingmanager.cpp`'s request body happens to put every one of these after a
    // literal `&` today (verified against the real call site), so this was never observable, but
    // a future reordering of that request construction would otherwise silently defeat scrubbing
    // for whichever value became first, with no test catching it (a D-09 scrubbing rule whose
    // whole job is "this secret must never leave the machine").
    static const QRegularExpression salt(QStringLiteral("&?salt=[0-9a-fA-F]+"));
    static const QRegularExpression clientCert(QStringLiteral("&?clientcert=[0-9a-fA-F]+"));
    static const QRegularExpression clientChallenge(QStringLiteral("&?clientchallenge=[0-9a-fA-F]+"));
    static const QRegularExpression serverChallengeResp(
        QStringLiteral("&?serverchallengeresp=[0-9a-fA-F]+"));
    static const QRegularExpression clientPairingSecret(
        QStringLiteral("&?clientpairingsecret=[0-9a-fA-F]+"));
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

    // T020/SC7/N1: the customer's own addresses, last, so a pairing secret or a URL token cannot
    // hide an address behind an already-replaced run. Idempotent: `[address]` is not address-shaped.
    result = scrubAddresses(result);

    return result;
}

QJsonObject LogShipper::scrubAttributes(const QJsonObject& attrs)
{
    QJsonObject out;
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        const QJsonValue value = it.value();
        if (value.isString()) {
            out.insert(it.key(), scrub(value.toString()));
        }
        else if (value.isObject()) {
            // A nested value is still text on its way off the machine - recurse rather than drop it
            // (a dropped key would silently change the diagnostic's meaning).
            out.insert(it.key(), scrubAttributes(value.toObject()));
        }
        else if (value.isArray()) {
            QJsonArray array;
            for (const QJsonValue& element : value.toArray()) {
                array.append(element.isString() ? QJsonValue(scrub(element.toString()))
                                                : element);
            }
            out.insert(it.key(), array);
        }
        else {
            // Numbers and bools carry no text of their own: a port, an attempt count and a step
            // number survive exactly as the caller wrote them.
            out.insert(it.key(), value);
        }
    }
    return out;
}

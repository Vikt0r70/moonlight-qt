#pragma once

// LogShipper/LogSpool (06.3.1 D-14, D-09, SC-5, Plan 21): SeatHub's own log shipper.
//
// `LogTee` (`log_tee.h`) dispatches every Qt/SDL log line in this process to whichever sinks are
// registered. `LogShipper` is one such sink, registered once for the life of the process
// (`start()`, called from `SeatHubTelemetry::start()`), and it is the ONLY sink in this fork that
// ships lines off the machine. Per the tee's own contract (D-16, `log_tee.h`'s `addSink()`
// comment), this sink itself never blocks, never logs, and never calls `addSink()`/`removeSink()`
// from inside its own dispatch - `start()` registers the sink exactly once, ever, and every later
// `start()` call (tests only; production calls it once) reuses that same registration instead of
// adding a second one.
//
// The design (`06.3.1-RESEARCH-SEATHUB.md` § E.3):
//   1. The sink (any thread) filters info+, copies the text, stamps a snapshot of the current
//      `{session_id, host_id, trace_id}` (published by `publishIds()` - never read from another
//      module's statics on this thread, which would be a data race: `telemetry.cpp`'s own
//      identity is documented client-thread-only), and pushes to a bounded in-memory queue
//      (`kQueueLines`, dropping the oldest).
//   2. One `std::thread` worker (never a `QThread`/`QTimer` - it must start before
//      `QGuiApplication` exists, Pitfall 15) scrubs each line and either spools it or hands it to
//      the SDK, per `setCanShip()` and the pause flag, draining the spool first.
//   3. `LogSpool` is one JSON-Lines file per process, under a per-file `QLockFile` so a leftover
//      from a process that no longer runs can be told apart from one that still does (Pitfall 8:
//      SeatHub has no single-instance guard) - bounded by `kSpoolBytes`, oldest lines dropped
//      first, and a line older than `kSpoolMaxAgeDays` is dropped at drain rather than shipped
//      with a stale time.
//
// `app/seathub/telemetry.cpp` is the only caller that builds a real `HandOff` (it is also the
// only translation unit that includes `sentry.h` - this header and its `.cpp` know nothing about
// sentry-native). Nothing here reads or writes `%LOCALAPPDATA%\Seven Hills\SeatHub\log-spool\`
// directly in a test: `setSpoolDirectoryForTests()` redirects it first.

#include "log_tee.h"

#include <QLockFile>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

/// One line captured by the `LogTee` sink, snapshotted at log time - never at replay time
/// (Pitfall 5: a spooled line ships with ITS OWN original time, carried in `loggedAt`, not
/// whenever the worker finally gets to it).
struct ShippedLine
{
    LogLevel level = LogLevel::Info;
    QString body;
    /// Epoch seconds, captured when the line was logged (not when it ships).
    double loggedAt = 0.0;
    QString sessionId;
    QString hostId;
    QString traceId;
    /// ADR-0072 item 7 / plan 09: scalar attributes for the diagnostic lane (`play.step`,
    /// `client.telemetry`, ...). Empty for every ordinary line the `LogTee` sink captures, so an
    /// empty object and "not a diagnostic line" mean the same thing to a reader - `diagnostic`
    /// below is the authoritative flag, persisted beside it. Scalars only: string, number or
    /// bool, never a nested object or list (`logShipperHandOff()` turns each one into a
    /// `sentry_value_new_attribute`).
    QJsonObject attrs;
    /// True for a line this process built itself as a structured diagnostic (ADR-0072), never one
    /// the tee captured. Persisted, so a diagnostic line that waits in the spool keeps both its
    /// attributes and its exemption from the 8 MiB body cap across a crash and a restart.
    bool diagnostic = false;
};

/// `true` when the SDK took the line (an envelope now owns it, one way or another); `false` for
/// `SENTRY_LOG_RETURN_FAILED` or `SENTRY_LOG_RETURN_DISABLED`, meaning the line stays in - or
/// goes back into - the spool. `telemetry.cpp` is the only real implementation; tests use a stub.
using HandOff = std::function<bool(const ShippedLine&)>;

/// One process's own on-disk overflow for lines a `LogShipper` cannot hand off yet - not signed
/// in, no DSN, `logs: false`, or a paused hand-off (a re-init in progress). `LogShipper`'s worker
/// is the only caller of every method here; nothing in this class is thread-safe on its own.
class LogSpool
{
public:
    /// D-14, `timing.md`/ADR-0063: the on-disk cap. Appending past it drops the oldest lines
    /// first - never the newest, and never all of them (the newest line is always kept even if it
    /// alone exceeds the cap).
    static constexpr qint64 kSpoolBytes = 2 * 1024 * 1024;
    /// D-14: a line this old is never shipped - `takeAll()` drops it rather than sending it with a
    /// stale-looking time (SEATHUB § E.2).
    static constexpr int kSpoolMaxAgeDays = 7;

    /// `directory` must already exist or be creatable (`QDir::mkpath`) - `LogShipper::start()`
    /// creates it before constructing this. Locks `spool-<pid>.jsonl.lock` for the life of this
    /// object (Pitfall 8): a second `LogSpool` for the SAME pid within one process (tests
    /// restarting `LogShipper`) locks and unlocks the same file cleanly, one at a time; a leftover
    /// from a DIFFERENT, no-longer-running process is what `adoptLeftovers()` reclaims.
    explicit LogSpool(const QString& directory);
    ~LogSpool();

    /// Appends `line` to this process's own spool file (`spool-<pid>.jsonl`), pruning the oldest
    /// lines first to stay within `kSpoolBytes`.
    void append(const ShippedLine& line);

    /// Reads every line in this process's own spool file, deletes the file, and returns the lines
    /// oldest-first with anything older than `kSpoolMaxAgeDays` already dropped. A caller that
    /// cannot hand off every returned line must `append()` the ones it could not, in order, to put
    /// them back (this class has no separate "put it back" call - `append()` is idempotent for
    /// that purpose).
    QList<ShippedLine> takeAll();

    /// Appends an adopted batch (what `adoptLeftovers()` returned) with the age rule already
    /// applied: a line older than `kSpoolMaxAgeDays` is dropped HERE and counted, never queued
    /// for the first drain to drop again. Plan 09 (ADR-0072 item 8): `LogShipper::start()` runs
    /// this BEFORE `SeatHubTelemetry::start()` reaches `noteLaunch()`, so the launch record's
    /// `dropped_spool_age` already knows how many leftovers startup threw away while
    /// `spool_lines_adopted` still reports what was adopted. Same 7-day cutoff `takeAll()` has
    /// always applied - no new clock policy, and a line the filter already passed is unaffected.
    void appendAdopted(const QList<ShippedLine>& lines);

    /// Plan 09 (D-13): how many lines `takeAll()` has dropped for being older than
    /// `kSpoolMaxAgeDays`, plus what `appendAdopted()` dropped at startup. Atomic: the worker
    /// increments it on the spool thread while the launch record reads it from the client thread
    /// - a plain `int` there would be a data race. Cumulative over this object's life; the
    /// launch record's `dropped_spool_age` reads it through `LogShipper::droppedSpoolAgeCount()`.
    int droppedAgeLines() const { return m_droppedAgeLines.load(); }
    int pendingLines() const { return m_pendingLines.load(); }

    /// Scans `directory` for `spool-*.jsonl` files this process did not create, adopts (reads,
    /// then deletes) any whose owning process is no longer running - proven by a failed-to-lock
    /// check against Qt's own PID-liveness detection, not a time heuristic - and leaves any file
    /// whose process is still alive untouched. Called once, from `LogShipper::start()`, before its
    /// own spool file exists.
    static QList<ShippedLine> adoptLeftovers(const QString& directory);

private:
    QString m_directory;
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    std::atomic<int> m_droppedAgeLines{ 0 };
    std::atomic<int> m_pendingLines{ 0 };
};

/// The one process-lifetime sink `SeatHubTelemetry::start()` registers (D-14). A singleton -
/// `LogTee`'s sink list is process-global, and this is this fork's one subscriber that ships
/// lines anywhere.
class LogShipper
{
public:
    /// `timing.md`/ADR-0063: the in-memory queue's line cap - the newest `kQueueLines` survive a
    /// burst; older ones already reached the queue and are dropped to make room.
    static constexpr int kQueueLines = 1000;
    static constexpr qint64 kSpoolBytes = LogSpool::kSpoolBytes;
    static constexpr int kSpoolMaxAgeDays = LogSpool::kSpoolMaxAgeDays;
    /// D-14/E.2: once this many bytes of log BODIES have shipped in this process's run, one
    /// `"log cap reached"` line ships and nothing else does, for the rest of the run. The disk log
    /// (`main.cpp`'s own 10 MB cap) is a separate limit and is unaffected.
    static constexpr qint64 kShippedBytesPerRun = 8 * 1024 * 1024;
    /// ADR-0072 V30 / plan 09: the diagnostic lane's own per-run bound. Ordinary lines are bounded
    /// by `kShippedBytesPerRun` (bytes of body); diagnostics are bounded by COUNT instead - they
    /// are the handful of structured lines per Play that carry the whole record of what failed,
    /// and a byte cap would silence exactly the summary line that matters most. Past this count
    /// the lane stops silently, and `capReachedCount()` records that it did.
    static constexpr int kDiagnosticLinesPerRun = 1000;

    static LogShipper& instance();

    /// Registers the `LogTee` sink (once, ever - a later call reuses the existing registration)
    /// and starts the worker thread. `handOff` is called on the worker thread only, never on the
    /// thread that logged. Safe to call again after `stop()` (tests only; production calls this
    /// once, from `SeatHubTelemetry::start()`) - resets every per-run counter (shipped bytes, the
    /// cap-reached flag, `canShip`, the queue) and opens a fresh `LogSpool`.
    void start(HandOff handOff);

    /// Whether the worker may hand a line to the SDK at all: signed in, a DSN in use, and the
    /// handout's `logs` flag - `telemetry.cpp` is the only caller and computes all three itself
    /// (this class knows nothing about sign-in, DSNs or handouts). `false` before `start()` is
    /// ever called, and cheap/safe to call even then (`tst_facade_wiring` calls the identity
    /// setters that lead here without ever calling `start()`).
    void setCanShip(bool canShip);

    /// Spools every line from here on, even when `canShip()` is true - `telemetry.cpp`'s one
    /// re-init (the first DSN's adoption) wraps `sentry_close()`/`sentry_init()` in
    /// `pauseHandOff()`/`resumeHandOff()` so no line is ever hand-off'd to an SDK instance that is
    /// mid-close or mid-init (SEATHUB § E.3 step 5). Synchronous: does not return until any
    /// hand-off already in flight on the worker thread has finished, so the pause is real by the
    /// time the caller proceeds to close the SDK - OR until a bounded wait (production: 5000ms,
    /// CR-02) elapses, whichever comes first. The bounded wait is a fallback for a hand-off that
    /// never finishes (a wedged `HandOff` callback); it is logged when it fires. Calling code
    /// proceeds either way - never blocks the calling (client) thread forever.
    void pauseHandOff();
    /// Reverses `pauseHandOff()`. A no-op, harmlessly, if the worker was never started.
    void resumeHandOff();

    /// Blocks (with a bounded timeout) until the worker has made its spool-or-hand-off decision
    /// for every line captured before this call returns - so a line logged in the last instant of
    /// account A's session is never still sitting in the in-memory queue by the time the caller
    /// clears A's identity (Pitfall 9). A no-op if the worker was never started, or if nothing was
    /// ever captured.
    void drainBeforeSignOut();

    /// Stops accepting new lines, flushes whatever is still queued straight to the spool (never
    /// attempting a hand-off during shutdown - SEATHUB § E.3 step 6: nothing captured is lost, but
    /// nothing new ships either), and joins the worker thread. Idempotent, and safe to call on a
    /// `LogShipper` that was never started. Must be called before process exit if `start()` was
    /// ever called - a joinable `std::thread` still attached at this singleton's own static
    /// destruction would call `std::terminate()` (guarded here too: the destructor calls this).
    void stop();

    /// D-09/T-06.3.1-60: removes `&rikey=`/`&rikeyid=` values (upstream's own redaction runs only
    /// on the disk-log path, `main.cpp`:97-99 - this reapplies the same two patterns), an
    /// `Authorization: Bearer` token, this fork's pairing-handshake query values (`salt=`,
    /// `clientcert=`, `clientchallenge=`, `serverchallengeresp=`, `clientpairingsecret=` -
    /// `app/backend/nvpairingmanager.cpp`'s own request bodies, which `nvhttp.cpp` logs verbatim
    /// with `qInfo() << "Executing request:" << url.toString();`), a 4-digit Sunshine PIN written
    /// next to the word "pin", and a PEM certificate block. The customer's public IP is left
    /// alone (D-09). Static and pure - no lock, no I/O - so `before_send_log` can call it a second
    /// time in `telemetry.cpp` with no extra cost.
    static QString scrub(const QString& text);
    static void setRedactions(const QStringList& literals);
    static void clearRedactions();

    /// Publishes the identity every future line snapshots. `telemetry.cpp`'s `setSession()` /
    /// `clearSession()` / `setTrace()` / `clearTrace()` call this (Plan 21 deviation - the
    /// alternative was the sink reading `telemetry.cpp`'s own `s_current*` statics directly, which
    /// are documented client-thread-only and would be a data race read from the sink's own,
    /// arbitrary, thread). Safe to call before `start()` - a snapshot published early is simply
    /// what the first captured line already carries.
    void publishIds(const QString& sessionId, const QString& hostId, const QString& traceId);

    /// ADR-0072 item 7 / plan 09: the diagnostic lane's entry point - one structured line with
    /// scalar attributes, enqueued exactly like an ordinary line (same queue, same scrub, same
    /// `canShip`/pause/spool rules) but exempt from the 8 MiB body cap and counted against
    /// `kDiagnosticLinesPerRun` instead. A Debug level is ignored, as `sink()` ignores it. Safe
    /// before `start()` - nothing is captured then, as for any other line.
    void enqueueDiagnostic(const QString& body, LogLevel level, const QJsonObject& attrs);

    /// D-13 / plan 09: the three cumulative counters the launch record reports - a line dropped by
    /// the queue's own drop-oldest cap, a line dropped for being older than
    /// `kSpoolMaxAgeDays`, and how many times a per-run cap (the 8 MiB byte cap, or the
    /// diagnostic lane's count cap) was reached in this run. Each is a count, not a flag: a run
    /// that hits a cap once and a run that hits it a thousand times are told apart by it.
    qint64 droppedQueueCount() const;
    int backlogSpoolLines() const;
    qint64 droppedSpoolAgeCount() const;
    qint64 capReachedCount() const;
    /// D-13 / plan 09: how many lines `start()` adopted from spool files left by processes that
    /// are no longer running - the launch record's `spool_lines_adopted`.
    int adoptedSpoolLines() const;

    /// Test-only: every later `start()` uses `directory` instead of the real
    /// `%LOCALAPPDATA%\Seven Hills\SeatHub\log-spool\`. Call before `start()`.
    void setSpoolDirectoryForTests(const QString& directory);
    /// Test-only: blocks the worker from dequeuing anything (the queue itself, and its
    /// `kQueueLines` drop-oldest cap, are unaffected - only the worker's own consumption pauses) -
    /// `pauseHandOff()` cannot stand in for this: it still drains the queue into the spool, which
    /// would defeat a test asserting on the queue's own cap.
    void holdWorkerForTests(bool hold);
    /// Test-only: bytes actually handed to the stub `HandOff` so far this run (Task 1's 8 MiB cap
    /// test measures what the stub received, since the queue itself can drop lines first).
    qint64 shippedBytesForTests() const;
    /// Test-only: overrides `pauseHandOff()`'s bounded wait (production: 5000ms, the CR-02 fix)
    /// so a test proving the timeout fallback does not have to wait the full 5s. A negative value
    /// (the default) means "use the production timeout".
    void setPauseTimeoutForTests(int milliseconds);

private:
    LogShipper();
    ~LogShipper();
    LogShipper(const LogShipper&) = delete;
    LogShipper& operator=(const LogShipper&) = delete;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

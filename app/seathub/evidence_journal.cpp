#include "evidence_journal.h"

#include "attempt_vocab.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <limits>

// The journal's own code, and nothing else: no network, no GUI, no SDL, no Windows API. Every value
// that reaches a capsule is a member of a closed set, an ADR-0072 vocabulary token, an integer, a
// boolean or a timestamp (ADR-0105 section 5).

namespace {

constexpr qint64 kMaxSafeInteger = 9007199254740991LL;
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024; // a hand-edited giant is unreadable, not parsed
constexpr int kFormatVersion = 1;
constexpr qint64 kSkewBoundMs = 5000; // docs/spec/agents.md:28

QElapsedTimer startProcessTimer()
{
    QElapsedTimer timer;
    timer.start();
    return timer;
}

// Started when the program is loaded, once per process.
const QElapsedTimer kProcessTimer = startProcessTimer();

const QString kCapsulePrefix = QStringLiteral("evj-");
const QString kCapsuleSuffix = QStringLiteral(".json");
const QString kPendingFile = QStringLiteral("counters.pending");

// ---------------------------------------------------------------------------------------------
// Closed sets (ADR-0105 section 3; docs/spec/openapi.yaml EvidenceEntry)
// ---------------------------------------------------------------------------------------------

const QStringList& outcomeSet()
{
    static const QStringList v{QStringLiteral("success"), QStringLiteral("failure")};
    return v;
}
const QStringList& retryReasonSet()
{
    static const QStringList v{QStringLiteral("conflict"), QStringLiteral("failure")};
    return v;
}
const QStringList& portOutcomeSet()
{
    static const QStringList v{QStringLiteral("failed"), QStringLiteral("clean"),
                               QStringLiteral("inconclusive")};
    return v;
}
const QStringList& outboxEventSet()
{
    static const QStringList v{QStringLiteral("put"),       QStringLiteral("drain"),
                               QStringLiteral("delivered"), QStringLiteral("kept"),
                               QStringLiteral("dropped"),   QStringLiteral("unreadable")};
    return v;
}
const QStringList& outboxAnswerSet()
{
    static const QStringList v{QStringLiteral("2xx"),  QStringLiteral("400"),
                               QStringLiteral("401"),  QStringLiteral("403"),
                               QStringLiteral("404"),  QStringLiteral("422"),
                               QStringLiteral("5xx"),  QStringLiteral("transport"),
                               QStringLiteral("none")};
    return v;
}
const QStringList& mediumSet()
{
    static const QStringList v{QStringLiteral("ethernet"), QStringLiteral("wifi"),
                               QStringLiteral("cellular"), QStringLiteral("bluetooth"),
                               QStringLiteral("unknown")};
    return v;
}
const QStringList& reachabilitySet()
{
    static const QStringList v{QStringLiteral("online"), QStringLiteral("site"),
                               QStringLiteral("local"), QStringLiteral("disconnected"),
                               QStringLiteral("unknown")};
    return v;
}
const QStringList& clockBucketSet()
{
    static const QStringList v{QStringLiteral("within_5s"), QStringLiteral("client_ahead"),
                               QStringLiteral("client_behind"), QStringLiteral("unknown")};
    return v;
}

// ---------------------------------------------------------------------------------------------
// Field rules
// ---------------------------------------------------------------------------------------------

enum class FieldType { Int, Bool, Enum, Token };

struct FieldRule {
    QString name;
    FieldType type = FieldType::Int;
    qint64 lo = 0;
    qint64 hi = 0;
    QStringList values;
    bool optional = false;
};

FieldRule intField(const char* name, qint64 lo, qint64 hi, bool optional = false)
{
    FieldRule rule;
    rule.name = QLatin1String(name);
    rule.type = FieldType::Int;
    rule.lo = lo;
    rule.hi = hi;
    rule.optional = optional;
    return rule;
}
FieldRule boolField(const char* name)
{
    FieldRule rule;
    rule.name = QLatin1String(name);
    rule.type = FieldType::Bool;
    return rule;
}
FieldRule enumField(const char* name, const QStringList& values, bool optional = false)
{
    FieldRule rule;
    rule.name = QLatin1String(name);
    rule.type = FieldType::Enum;
    rule.values = values;
    rule.optional = optional;
    return rule;
}
FieldRule tokenField(const char* name)
{
    FieldRule rule;
    rule.name = QLatin1String(name);
    rule.type = FieldType::Token;
    return rule;
}

const QHash<QString, QList<FieldRule>>& kindRules()
{
    static const QHash<QString, QList<FieldRule>> rules = [] {
        QHash<QString, QList<FieldRule>> r;
        r.insert(QStringLiteral("pairing_attempt"),
                 {intField("attempt", 1, 2), enumField("outcome", outcomeSet()),
                  tokenField("engine_step"), tokenField("engine_class"), boolField("retried"),
                  enumField("retry_reason", retryReasonSet(), true), boolField("pin_shape_ok"),
                  intField("handshake_ms", 0, kMaxSafeInteger)});
        r.insert(QStringLiteral("port_test"),
                 {enumField("outcome", portOutcomeSet()),
                  intField("failed_mask", 1, 4294967294LL, true)});
        const QList<FieldRule> outbox{enumField("event", outboxEventSet()),
                                      enumField("answer", outboxAnswerSet()),
                                      intField("count", 1, kMaxSafeInteger)};
        r.insert(QStringLiteral("end_outbox"), outbox);
        r.insert(QStringLiteral("quality_outbox"), outbox);
        r.insert(QStringLiteral("stall_window"),
                 {intField("windows", 1, kMaxSafeInteger),
                  intField("duration_ms", 0, kMaxSafeInteger)});
        r.insert(QStringLiteral("network_change"),
                 {enumField("medium", mediumSet()), enumField("reachability", reachabilitySet()),
                  boolField("captive_portal"), boolField("metered"),
                  intField("transitions", 1, kMaxSafeInteger)});
        r.insert(QStringLiteral("engine_failure"),
                 {tokenField("step"), tokenField("engine_class"),
                  intField("engine_code", std::numeric_limits<qint32>::min(),
                           std::numeric_limits<qint32>::max())});
        r.insert(QStringLiteral("clock_skew"), {enumField("bucket", clockBucketSet())});
        return r;
    }();
    return rules;
}

bool isIntegral(const QJsonValue& value, qint64 lo, qint64 hi)
{
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    if (!(d > -1e18 && d < 1e18)) {
        return false; // also rejects NaN
    }
    if (d !=static_cast<double>(static_cast<qint64>(d))) {
        return false;
    }
    const qint64 n = static_cast<qint64>(d);
    return n >= lo && n <= hi;
}

// ^[a-z][a-z0-9_]{0,31}$
bool isToken(const QString& s)
{
    if (s.isEmpty() || s.size() > 32) {
        return false;
    }
    for (int i = 0; i < s.size(); ++i) {
        const ushort c = s.at(i).unicode();
        const bool lower = c >= 'a' && c <= 'z';
        const bool digitOrUnderscore = (c >= '0' && c <= '9') || c == '_';
        if (i == 0 ? !lower : !(lower || digitOrUnderscore)) {
            return false;
        }
    }
    return true;
}

// An opaque id: [A-Za-z0-9_-]{1,64}. A UUID passes; an address, a host name, a path, an e-mail
// address or a Wi-Fi name with punctuation does not.
bool isOpaqueId(const QString& s)
{
    if (s.isEmpty() || s.size() > 64) {
        return false;
    }
    for (const QChar qc : s) {
        const ushort c = qc.unicode();
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isCanonicalUuid(const QString& s)
{
    const QUuid u = QUuid::fromString(s);
    return !u.isNull() && u.toString(QUuid::WithoutBraces) == s;
}

bool isRfc3339Utc(const QString& s)
{
    if (s.size() < 20 || !s.endsWith(QLatin1Char('Z'))) {
        return false;
    }
    return QDateTime::fromString(s, Qt::ISODateWithMs).isValid();
}

bool isAttemptStep(const QString& step)
{
    return attemptStepTokens().contains(step);
}

bool fieldOk(const FieldRule& rule, const QJsonValue& value)
{
    switch (rule.type) {
    case FieldType::Int:
        return isIntegral(value, rule.lo, rule.hi);
    case FieldType::Bool:
        return value.isBool();
    case FieldType::Enum:
        return value.isString() && rule.values.contains(value.toString());
    case FieldType::Token:
        return value.isString() && isToken(value.toString());
    }
    return false;
}

/// `envelope` true: `seq`, `at` and `elapsed_ms` are required (a stored entry); false: they must
/// be absent (a builder's result). `strictVocab` additionally holds the ADR-0072 closed sets for
/// the step and class tokens (builders and `record`); the disk reader checks the wire pattern only
/// so a future vocabulary change never makes a stored capsule unreadable.
bool validateEntry(const QJsonObject& entry, bool envelope, bool strictVocab)
{
    const QString kind = entry.value(QStringLiteral("kind")).toString();
    const auto it = kindRules().constFind(kind);
    if (it == kindRules().constEnd()) {
        return false;
    }
    const QList<FieldRule>& rules = it.value();

    QSet<QString> allowed{QStringLiteral("kind")};
    if (envelope) {
        allowed.insert(QStringLiteral("seq"));
        allowed.insert(QStringLiteral("at"));
        allowed.insert(QStringLiteral("elapsed_ms"));
        if (!isIntegral(entry.value(QStringLiteral("seq")), 0, kMaxSafeInteger)
            || !isIntegral(entry.value(QStringLiteral("elapsed_ms")), 0, kMaxSafeInteger)
            || !entry.value(QStringLiteral("at")).isString()
            || !isRfc3339Utc(entry.value(QStringLiteral("at")).toString())) {
            return false;
        }
    }
    for (const FieldRule& rule : rules) {
        allowed.insert(rule.name);
        if (!entry.contains(rule.name)) {
            if (!rule.optional) {
                return false;
            }
            continue;
        }
        if (!fieldOk(rule, entry.value(rule.name))) {
            return false;
        }
    }
    for (auto k = entry.constBegin(); k != entry.constEnd(); ++k) {
        if (!allowed.contains(k.key())) {
            return false;
        }
    }

    // Cross-field rules.
    if (kind == QLatin1String("pairing_attempt")) {
        if (entry.contains(QStringLiteral("retry_reason"))
            && !entry.value(QStringLiteral("retried")).toBool()) {
            return false;
        }
        if (strictVocab) {
            const QString step = entry.value(QStringLiteral("engine_step")).toString();
            const QString cls = entry.value(QStringLiteral("engine_class")).toString();
            if (!isAttemptStep(step)) {
                return false;
            }
            if (entry.value(QStringLiteral("outcome")).toString() == QLatin1String("success")) {
                if (cls != QLatin1String("ok")) {
                    return false;
                }
            } else if (cls.isEmpty() || !isClosedFailureClass(step, cls)) {
                return false;
            }
        }
    } else if (kind == QLatin1String("port_test")) {
        const bool failed = entry.value(QStringLiteral("outcome")).toString()
            == QLatin1String("failed");
        if (failed != entry.contains(QStringLiteral("failed_mask"))) {
            return false;
        }
    } else if (kind == QLatin1String("engine_failure") && strictVocab) {
        const QString step = entry.value(QStringLiteral("step")).toString();
        const QString cls = entry.value(QStringLiteral("engine_class")).toString();
        if (!isAttemptStep(step) || cls.isEmpty() || !isClosedFailureClass(step, cls)) {
            return false;
        }
    }
    return true;
}

bool isFailureEntry(const QJsonObject& entry)
{
    const QString kind = entry.value(QStringLiteral("kind")).toString();
    const QString outcome = entry.value(QStringLiteral("outcome")).toString();
    return (kind == QLatin1String("pairing_attempt") && outcome == QLatin1String("failure"))
        || (kind == QLatin1String("port_test") && outcome == QLatin1String("failed"))
        || kind == QLatin1String("engine_failure") || kind == QLatin1String("stall_window");
}

/// A capsule's summary: its first failure entry and the first entry of every other kind (its
/// closed classes), in seq order. Seqs are kept, so a server that already stored an entry still
/// recognises it.
QJsonArray summarise(const QJsonArray& entries)
{
    QSet<int> chosen;
    for (int i = 0; i < entries.size(); ++i) {
        if (isFailureEntry(entries.at(i).toObject())) {
            chosen.insert(i);
            break;
        }
    }
    QSet<QString> kinds;
    for (const int i : chosen) {
        kinds.insert(entries.at(i).toObject().value(QStringLiteral("kind")).toString());
    }
    for (int i = 0; i < entries.size(); ++i) {
        const QString kind = entries.at(i).toObject().value(QStringLiteral("kind")).toString();
        if (!kinds.contains(kind)) {
            kinds.insert(kind);
            chosen.insert(i);
        }
    }
    QJsonArray out;
    for (int i = 0; i < entries.size(); ++i) {
        if (chosen.contains(i)) {
            out.append(entries.at(i));
        }
    }
    return out;
}

QString answerClass(int status)
{
    if (status >= 200 && status < 300) {
        return QStringLiteral("2xx");
    }
    if (status >= 500 && status < 600) {
        return QStringLiteral("5xx");
    }
    switch (status) {
    case 400:
    case 401:
    case 403:
    case 404:
    case 422:
        return QString::number(status);
    default:
        break;
    }
    return status < 0 || status == 0 ? QStringLiteral("transport") : QStringLiteral("none");
}

// Every put, drain, delivery and drop writes a line. The line carries the closed `event` and
// `answer` values, the session id and a count - never a path, an address or free text.
void logLine(const char* event, const QString& answer, const QString& sessionId, qint64 count)
{
    qInfo().noquote() << QStringLiteral("evidence.journal event=%1 answer=%2 session=%3 count=%4")
                             .arg(QLatin1String(event), answer,
                                  sessionId.isEmpty() ? QStringLiteral("-") : sessionId,
                                  QString::number(count));
}

std::atomic<int> g_openSerial{0};
std::atomic<qint64> g_lastOpenMs{0};

QJsonObject countersToJson(const EvidenceJournal::Counters& c)
{
    QJsonObject o;
    o.insert(QStringLiteral("reduced_capsules"), c.reducedCapsules);
    o.insert(QStringLiteral("dropped_capsules"), c.droppedCapsules);
    o.insert(QStringLiteral("unreadable_files"), c.unreadableFiles);
    o.insert(QStringLiteral("dropped_entries"), c.droppedEntries);
    return o;
}

bool countersFromJson(const QJsonValue& value, EvidenceJournal::Counters* out)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject o = value.toObject();
    if (o.size() != 4) {
        return false;
    }
    const QString names[4] = {QStringLiteral("reduced_capsules"), QStringLiteral("dropped_capsules"),
                              QStringLiteral("unreadable_files"), QStringLiteral("dropped_entries")};
    qint64 v[4];
    for (int i = 0; i < 4; ++i) {
        if (!isIntegral(o.value(names[i]), 0, kMaxSafeInteger)) {
            return false;
        }
        v[i] = static_cast<qint64>(o.value(names[i]).toDouble());
    }
    out->reducedCapsules = v[0];
    out->droppedCapsules = v[1];
    out->unreadableFiles = v[2];
    out->droppedEntries = v[3];
    return true;
}

EvidenceJournal::Counters plus(const EvidenceJournal::Counters& a,
                               const EvidenceJournal::Counters& b)
{
    EvidenceJournal::Counters c;
    c.reducedCapsules = std::min(kMaxSafeInteger, a.reducedCapsules + b.reducedCapsules);
    c.droppedCapsules = std::min(kMaxSafeInteger, a.droppedCapsules + b.droppedCapsules);
    c.unreadableFiles = std::min(kMaxSafeInteger, a.unreadableFiles + b.unreadableFiles);
    c.droppedEntries = std::min(kMaxSafeInteger, a.droppedEntries + b.droppedEntries);
    return c;
}

bool isZero(const EvidenceJournal::Counters& c)
{
    return c.reducedCapsules == 0 && c.droppedCapsules == 0 && c.unreadableFiles == 0
        && c.droppedEntries == 0;
}

QJsonObject finishBuilt(const QJsonObject& entry)
{
    return validateEntry(entry, false, true) ? entry : QJsonObject();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Capsule
// ---------------------------------------------------------------------------------------------

struct EvidenceJournal::Capsule {
    QString sessionId;
    QString accountId;
    QString runId;
    bool reduced = false;
    Counters counters;
    QJsonArray entries;

    QJsonObject body() const
    {
        QJsonObject o;
        o.insert(QStringLiteral("run_id"), runId);
        o.insert(QStringLiteral("reduced"), reduced);
        o.insert(QStringLiteral("counters"), countersToJson(counters));
        o.insert(QStringLiteral("entries"), entries);
        return o;
    }

    QJsonObject onDisk() const
    {
        QJsonObject o = body();
        o.insert(QStringLiteral("v"), kFormatVersion);
        o.insert(QStringLiteral("session_id"), sessionId);
        o.insert(QStringLiteral("account_id"), accountId);
        return o;
    }
};

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

EvidenceJournal::EvidenceJournal(QString directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : std::move(directory))
{
}

void EvidenceJournal::setDirectory(const QString& directory)
{
    QMutexLocker locker(&m_mutex);
    m_directory = directory.isEmpty() ? defaultDirectory() : directory;
}

QString EvidenceJournal::directory() const
{
    QMutexLocker locker(&m_mutex);
    return m_directory;
}

QString EvidenceJournal::defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("evidence-journal"));
}

void EvidenceJournal::setDirectoryCapBytes(qint64 bytes)
{
    if (bytes < 1) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    m_directoryCapBytes = bytes;
}

void EvidenceJournal::setEntryCap(int entries)
{
    QMutexLocker locker(&m_mutex);
    m_entryCap = std::max(1, std::min(entries, kDefaultEntryCap));
}

qint64 EvidenceJournal::processElapsedMs()
{
    return kProcessTimer.elapsed();
}

bool EvidenceJournal::open(const QString& sessionId, const QString& accountId)
{
    if (!isOpaqueId(sessionId) || !isOpaqueId(accountId)) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    m_open = false;
    m_sessionId = sessionId;
    m_accountId = accountId;
    m_runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // File names sort by open time; the serial breaks a tie within one millisecond and the clock
    // never goes backwards for the names of one process.
    qint64 ms = QDateTime::currentMSecsSinceEpoch();
    ms = std::max(ms, g_lastOpenMs.load());
    g_lastOpenMs.store(ms);
    const int serial = g_openSerial.fetch_add(1) % 10000;
    m_fileName = QStringLiteral("%1%2-%3-%4%5")
                     .arg(kCapsulePrefix, QString::number(ms).rightJustified(13, QLatin1Char('0')),
                          QString::number(serial).rightJustified(4, QLatin1Char('0')), m_runId,
                          kCapsuleSuffix);
    m_nextSeq = 0;
    m_open = true;
    enforceCapLocked();
    return true;
}

void EvidenceJournal::close()
{
    QMutexLocker locker(&m_mutex);
    m_open = false;
}

bool EvidenceJournal::isOpen() const
{
    QMutexLocker locker(&m_mutex);
    return m_open;
}

// ---------------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------------

QJsonObject EvidenceJournal::pairingAttempt(int attempt, bool success, const QString& engineStep,
                                            const QString& engineClass, bool retried,
                                            const QString& retryReason, bool pinShapeOk,
                                            qint64 handshakeMs)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("pairing_attempt"));
    o.insert(QStringLiteral("attempt"), attempt);
    o.insert(QStringLiteral("outcome"),
             success ? QStringLiteral("success") : QStringLiteral("failure"));
    o.insert(QStringLiteral("engine_step"), engineStep);
    o.insert(QStringLiteral("engine_class"),
             success && engineClass.isEmpty() ? QStringLiteral("ok") : engineClass);
    o.insert(QStringLiteral("retried"), retried);
    if (!retryReason.isEmpty()) {
        o.insert(QStringLiteral("retry_reason"), retryReason);
    }
    o.insert(QStringLiteral("pin_shape_ok"), pinShapeOk);
    o.insert(QStringLiteral("handshake_ms"), handshakeMs);
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::portTest(const PortTestResult& result)
{
    QJsonObject o = portTestJson(result);
    if (o.isEmpty()) {
        return QJsonObject();
    }
    o.insert(QStringLiteral("kind"), QStringLiteral("port_test"));
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::outbox(const QString& kind, const QString& event,
                                    const QString& answer, int count)
{
    if (kind != QLatin1String("end_outbox") && kind != QLatin1String("quality_outbox")) {
        return QJsonObject();
    }
    QJsonObject o;
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("event"), event);
    o.insert(QStringLiteral("answer"), answer);
    o.insert(QStringLiteral("count"), count);
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::stallWindow(int windows, qint64 durationMs)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("stall_window"));
    o.insert(QStringLiteral("windows"), windows);
    o.insert(QStringLiteral("duration_ms"), durationMs);
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::networkChange(const QString& medium, const QString& reachability,
                                           bool captivePortal, bool metered, int transitions)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("network_change"));
    o.insert(QStringLiteral("medium"), medium);
    o.insert(QStringLiteral("reachability"), reachability);
    o.insert(QStringLiteral("captive_portal"), captivePortal);
    o.insert(QStringLiteral("metered"), metered);
    o.insert(QStringLiteral("transitions"), transitions);
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::engineFailure(const QString& step, const QString& engineClass,
                                           int engineCode)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("engine_failure"));
    o.insert(QStringLiteral("step"), step);
    o.insert(QStringLiteral("engine_class"), engineClass);
    o.insert(QStringLiteral("engine_code"), engineCode);
    return finishBuilt(o);
}

QJsonObject EvidenceJournal::clockSkew(const QDateTime& serverUtc, const QDateTime& localUtc)
{
    QString bucket = QStringLiteral("unknown");
    if (serverUtc.isValid() && localUtc.isValid()) {
        const qint64 skewMs = serverUtc.msecsTo(localUtc); // positive: the client is ahead
        if (skewMs > kSkewBoundMs) {
            bucket = QStringLiteral("client_ahead");
        } else if (skewMs < -kSkewBoundMs) {
            bucket = QStringLiteral("client_behind");
        } else {
            bucket = QStringLiteral("within_5s");
        }
    }
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("clock_skew"));
    o.insert(QStringLiteral("bucket"), bucket);
    return finishBuilt(o);
}

// ---------------------------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------------------------

QString EvidenceJournal::capsulePath(const QString& fileName) const
{
    return QDir(m_directory).filePath(fileName);
}

QStringList EvidenceJournal::capsuleFileNames() const
{
    return QDir(m_directory)
        .entryList(QStringList{kCapsulePrefix + QLatin1Char('*') + kCapsuleSuffix}, QDir::Files,
                   QDir::Name);
}

bool EvidenceJournal::readCapsule(const QString& fileName, Capsule* out) const
{
    QFile file(capsulePath(fileName));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileBytes) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.size() != 7 || !isIntegral(o.value(QStringLiteral("v")), kFormatVersion, kFormatVersion)
        || !o.value(QStringLiteral("session_id")).isString()
        || !isOpaqueId(o.value(QStringLiteral("session_id")).toString())
        || !o.value(QStringLiteral("account_id")).isString()
        || !isOpaqueId(o.value(QStringLiteral("account_id")).toString())
        || !o.value(QStringLiteral("run_id")).isString()
        || !isCanonicalUuid(o.value(QStringLiteral("run_id")).toString())
        || !o.value(QStringLiteral("reduced")).isBool()
        || !o.value(QStringLiteral("entries")).isArray()) {
        return false;
    }
    Capsule capsule;
    capsule.sessionId = o.value(QStringLiteral("session_id")).toString();
    capsule.accountId = o.value(QStringLiteral("account_id")).toString();
    capsule.runId = o.value(QStringLiteral("run_id")).toString();
    capsule.reduced = o.value(QStringLiteral("reduced")).toBool();
    if (!countersFromJson(o.value(QStringLiteral("counters")), &capsule.counters)) {
        return false;
    }
    capsule.entries = o.value(QStringLiteral("entries")).toArray();
    if (capsule.entries.isEmpty() || capsule.entries.size() > kDefaultEntryCap) {
        return false;
    }
    QSet<qint64> seqs;
    for (const QJsonValue& value : capsule.entries) {
        if (!value.isObject() || !validateEntry(value.toObject(), true, false)) {
            return false;
        }
        const qint64 seq = static_cast<qint64>(value.toObject().value(QStringLiteral("seq")).toDouble());
        if (seqs.contains(seq)) {
            return false;
        }
        seqs.insert(seq);
    }
    *out = capsule;
    return true;
}

bool EvidenceJournal::writeCapsule(const QString& fileName, const Capsule& capsule) const
{
    if (!QDir().mkpath(m_directory)) {
        return false;
    }
    // QSaveFile: a crash mid-write leaves the previous copy (or nothing), never a truncated file.
    QSaveFile file(capsulePath(fileName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(capsule.onDisk()).toJson(QJsonDocument::Compact));
    return file.commit();
}

EvidenceJournal::Counters EvidenceJournal::readPendingCounters() const
{
    Counters counters;
    QFile file(QDir(m_directory).filePath(kPendingFile));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        return Counters();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || !countersFromJson(doc.object(), &counters)) {
        return Counters();
    }
    return counters;
}

void EvidenceJournal::addPendingCounters(const Counters& delta) const
{
    const Counters current = readPendingCounters();
    const Counters next = isZero(delta) ? current : plus(current, delta);
    if (!QDir().mkpath(m_directory)) {
        return;
    }
    const QString path = QDir(m_directory).filePath(kPendingFile);
    if (isZero(next)) {
        QFile::remove(path);
        return;
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(countersToJson(next)).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

EvidenceJournal::Counters EvidenceJournal::pendingCounters() const
{
    QMutexLocker locker(&m_mutex);
    return readPendingCounters();
}

void EvidenceJournal::removeUnreadable(const QString& fileName) const
{
    QFile::remove(capsulePath(fileName));
    Counters delta;
    delta.unreadableFiles = 1;
    addPendingCounters(delta);
    logLine("unreadable", QStringLiteral("none"), QString(), 1);
}

void EvidenceJournal::enforceCapLocked()
{
    // Bounded: every pass reduces, drops or removes one file.
    for (int guard = 0; guard < 100000; ++guard) {
        const QStringList names = capsuleFileNames();
        qint64 total = 0;
        for (const QString& name : names) {
            total += QFileInfo(capsulePath(name)).size();
        }
        if (total <= m_directoryCapBytes) {
            return;
        }

        // The open Play's capsule is never reduced or dropped by the cap.
        bool progressed = false;
        for (const QString& name : names) {
            if (m_open && name == m_fileName) {
                continue;
            }
            Capsule capsule;
            if (!readCapsule(name, &capsule)) {
                removeUnreadable(name);
                progressed = true;
                break;
            }
            if (capsule.reduced) {
                continue;
            }
            capsule.entries = summarise(capsule.entries);
            capsule.reduced = true;
            if (!writeCapsule(name, capsule)) {
                return; // the disk refuses; do not spin
            }
            Counters delta;
            delta.reducedCapsules = 1;
            addPendingCounters(delta);
            logLine("reduced", QStringLiteral("none"), capsule.sessionId, capsule.entries.size());
            progressed = true;
            break;
        }
        if (progressed) {
            continue;
        }

        // Every closed capsule is already a summary and the folder is still over the cap: drop
        // the oldest summary. Its own counters are not lost; they move to the pending counts.
        for (const QString& name : names) {
            if (m_open && name == m_fileName) {
                continue;
            }
            Capsule capsule;
            Counters delta;
            QString sessionId;
            if (readCapsule(name, &capsule)) {
                delta = capsule.counters;
                sessionId = capsule.sessionId;
            }
            QFile::remove(capsulePath(name));
            delta.droppedCapsules += 1;
            addPendingCounters(delta);
            logLine("dropped", QStringLiteral("none"), sessionId, 1);
            progressed = true;
            break;
        }
        if (!progressed) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------------------------

void EvidenceJournal::record(const QJsonObject& entry)
{
    QMutexLocker locker(&m_mutex);
    if (!m_open) {
        return; // nothing is recorded while no Play is open
    }

    Counters droppedEntry;
    droppedEntry.droppedEntries = 1;
    if (entry.isEmpty() || !validateEntry(entry, false, true) || m_nextSeq >= m_entryCap) {
        addPendingCounters(droppedEntry);
        logLine("dropped", QStringLiteral("none"), m_sessionId, 1);
        return;
    }

    Capsule capsule;
    if (!QFileInfo::exists(capsulePath(m_fileName)) || !readCapsule(m_fileName, &capsule)) {
        if (QFileInfo::exists(capsulePath(m_fileName))) {
            removeUnreadable(m_fileName);
        }
        capsule = Capsule();
        capsule.sessionId = m_sessionId;
        capsule.accountId = m_accountId;
        capsule.runId = m_runId;
    }

    QJsonObject stamped = entry;
    stamped.insert(QStringLiteral("seq"), m_nextSeq);
    stamped.insert(QStringLiteral("at"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    stamped.insert(QStringLiteral("elapsed_ms"), processElapsedMs());
    capsule.entries.append(stamped);

    if (!writeCapsule(m_fileName, capsule)) {
        addPendingCounters(droppedEntry);
        logLine("dropped", QStringLiteral("none"), m_sessionId, 1);
        return;
    }
    ++m_nextSeq;
    logLine("put", QStringLiteral("none"), m_sessionId, 1);
    enforceCapLocked();
}

// ---------------------------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------------------------

QList<EvidenceJournal::PendingCapsule> EvidenceJournal::deliverable(const QString& accountId) const
{
    QMutexLocker locker(&m_mutex);

    QList<QPair<QString, Capsule>> mine;
    for (const QString& name : capsuleFileNames()) {
        Capsule capsule;
        if (!readCapsule(name, &capsule)) {
            removeUnreadable(name);
            continue;
        }
        if (capsule.accountId == accountId) {
            mine.append(qMakePair(name, capsule));
        }
    }

    // The loss counts ride the next delivered capsule: the first one handed out. They are written
    // into its file first, so a replay of it carries the same numbers.
    const Counters pending = readPendingCounters();
    if (!isZero(pending) && !mine.isEmpty()) {
        Capsule folded = mine.first().second;
        folded.counters = plus(folded.counters, pending);
        if (writeCapsule(mine.first().first, folded)) {
            mine.first().second = folded;
            QFile::remove(QDir(m_directory).filePath(kPendingFile));
        }
    }

    QList<PendingCapsule> out;
    for (const auto& item : mine) {
        const Capsule& capsule = item.second;
        PendingCapsule pendingCapsule;
        pendingCapsule.sessionId = capsule.sessionId;
        pendingCapsule.runId = capsule.runId;
        pendingCapsule.body = QJsonDocument(capsule.body()).toJson(QJsonDocument::Compact);
        out.append(pendingCapsule);

        qint64 maxSeq = -1;
        for (const QJsonValue& value : capsule.entries) {
            maxSeq = std::max(
                maxSeq, static_cast<qint64>(value.toObject().value(QStringLiteral("seq")).toDouble()));
        }
        m_inflightMaxSeq.insert(capsule.runId,
                                std::max(maxSeq, m_inflightMaxSeq.value(capsule.runId, -1)));
        logLine("drain", QStringLiteral("none"), capsule.sessionId, capsule.entries.size());
    }
    return out;
}

void EvidenceJournal::applyAnswer(const QString& sessionId, const QString& runId,
                                  int httpStatusOrMinusOneForTransport)
{
    if (!isCanonicalUuid(runId)) {
        return;
    }
    QMutexLocker locker(&m_mutex);

    QString fileName;
    for (const QString& name : capsuleFileNames()) {
        if (name.endsWith(runId + kCapsuleSuffix)) {
            fileName = name;
            break;
        }
    }
    if (fileName.isEmpty()) {
        return; // already removed: the answer to a replay
    }
    Capsule capsule;
    if (!readCapsule(fileName, &capsule)) {
        removeUnreadable(fileName);
        return;
    }
    if (capsule.sessionId != sessionId || capsule.runId != runId) {
        return;
    }

    const int status = httpStatusOrMinusOneForTransport;
    const QString answer = answerClass(status);
    const bool ok = status >= 200 && status < 300;

    if (ok || status == 404) {
        // The server has it (2xx) or has no such session, so a replay can never succeed (404).
        // Entries recorded after this capsule was handed out stay for the next send.
        QJsonArray remaining;
        if (ok) {
            const qint64 sentMax = m_inflightMaxSeq.value(runId, std::numeric_limits<qint64>::max());
            for (const QJsonValue& value : capsule.entries) {
                if (static_cast<qint64>(value.toObject().value(QStringLiteral("seq")).toDouble())
                    > sentMax) {
                    remaining.append(value);
                }
            }
        }
        if (remaining.isEmpty()) {
            QFile::remove(capsulePath(fileName));
            m_inflightMaxSeq.remove(runId);
        } else {
            capsule.entries = remaining;
            capsule.counters = Counters(); // delivered with the part that was sent
            if (!writeCapsule(fileName, capsule)) {
                return;
            }
        }
        logLine(ok ? "delivered" : "dropped", answer, sessionId, capsule.entries.size());
        return;
    }
    if (status == 400 || status == 422) {
        // A capsule the server will never accept: drop it and count it, so a client bug cannot
        // fill the disk. Its own counts were never delivered; they move to the pending counts.
        QFile::remove(capsulePath(fileName));
        m_inflightMaxSeq.remove(runId);
        Counters delta = capsule.counters;
        delta.droppedCapsules += 1;
        addPendingCounters(delta);
        logLine("dropped", answer, sessionId, 1);
        return;
    }
    // 401, 403, 5xx, a transport failure or any other answer: keep it; a report must not die.
    logLine("kept", answer, sessionId, capsule.entries.size());
}

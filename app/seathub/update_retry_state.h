#pragma once

#include <QDateTime>
#include <QStringList>
#include <QRandomGenerator>
#include <QVariantMap>
#include <functional>

// ADR-0077 item 7: pre-journal quiescence is a STATE rule, never a duration. The inputs below are facts the
// observer read at one moment; none of them is an age, and `admitInstallerLaunch` takes no clock. The
// separate journal-start rule (V35) is not an input: journal age never authorizes overlap.
struct InstallerObservation
{
    // The installer process this client launched, identified by PID and creation time.
    //   None    - no launch was recorded (first run, or state written before the observer existed)
    //   Alive   - that exact process is still running
    //   Gone    - it exited, or the PID now belongs to a different process (creation time differs)
    //   Unknown - it could not be opened or identified (for example another account's elevated process)
    enum class Owner { None, Alive, Gone, Unknown };
    // Any process on this machine whose image is a SeatHub setup or the SeatHub maintenance tool, from
    // any account, including a setup started by hand.
    enum class Scan { Clear, Present, Unknown };
    // A reboot ends every process, so it resolves an owner that could not be identified.
    enum class Boot { Same, Changed, Unknown };
    Owner owner = Owner::None;
    Scan scan = Scan::Unknown;
    Boot boot = Boot::Unknown;
};

enum class Admission { Quiescent, Blocked };

// Blocked, not abandoned: an ambiguous state is never converted into permission by waiting.
Admission admitInstallerLaunch(const InstallerObservation& observation);
const char* ownerName(InstallerObservation::Owner owner);
const char* scanName(InstallerObservation::Scan scan);
const char* bootName(InstallerObservation::Boot boot);

// The traces ADR-0077 item 8 assigns to this observer. One record per launch, replaced by the next launch,
// so it is bounded by construction and needs no history cap. Enumerated and numeric values only.
struct InstallerTrace
{
    QDateTime requestedAt, returnedAt;   // brackets the elevation prompt, SmartScreen and the person
    qint64 shellError = 0;               // GetLastError of the launch (0 on success)
    qint64 pid = 0;                      // the launched installer, 0 when the launch gave none
    QString pidCreated;                  // its creation FILETIME, decimal
    qint64 launcherPid = 0;              // the process that launched it; a different observer means it died
    qint64 launcherSession = -1, installerSession = -1;
    qint64 bootUptimeMs = 0;             // GetTickCount64 at launch; a smaller value later means a reboot
    int observations = 0;
    QString lastOwner, lastScan, lastBoot;
    bool ownerAccessDenied = false, observerChanged = false;
    bool journalStartPresent = false, stagePresent = false, progressChanged = false;
};

class UpdateRetryState
{
public:
    // V34 / ADR-0070 item 5: design values, not vendor defaults.
    static constexpr int kMaxAttempts = 4; // V34 design value: total automatic attempts per target.
    static constexpr qint64 kBase = 3600; // V34 design value: one hour, seconds.
    static constexpr qint64 kCap = 86400; // V34 design value: 24 hours, seconds.
    int v = 1;
    QString target, lastClass;
    int attemptsFailed = 0, declines = 0;
    QDateTime firstFailedAt, lastFailedAt, nextAllowedAt;
    bool exhaustedReported = false, launched = false;
    // Keep manual Try again free across a process exit before its result arrives.
    bool launchedManual = false;
    QStringList reportedIds;
    QVariantMap lastOffer;
    InstallerTrace trace;

    static QString defaultPath();
    static UpdateRetryState load(const QString& path);
    bool save(const QString& path) const;
    void rememberReport(const QString& id);
    void setClock(std::function<QDateTime()> clock) { m_clock = std::move(clock); }
    void setRandomGenerator(QRandomGenerator* random) { m_random = random; }
    QDateTime now() const { return m_clock(); }
    qint64 nextDelayFor(int k) const;
    void noteFailure(const QString& failureClass, bool permanent = false, bool manual = false,
                     bool previousLaunch = false);
    void noteDecline();
    void noteSuccess();
    bool automaticAttemptAllowed(const QDateTime& now, bool streaming) const;
    bool exhausted() const;
    bool reset(const QString& nextTarget, const QString& installed);
private:
    std::function<QDateTime()> m_clock = [] { return QDateTime::currentDateTimeUtc(); };
    QRandomGenerator* m_random = QRandomGenerator::global();
    bool m_declinedThisLaunch = false, m_firstFailureThisLaunch = false;
};

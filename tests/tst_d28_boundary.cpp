/*****************************************************************************
 * SeatHub fork - the D-28 fork boundary as an executable check (STREAM-06).
 *
 * The boundary is the phase's most safety-critical structural invariant: the streaming engine is
 * upstream's, unmodified, apart from two named exceptions with their own ADRs -
 * `app/streaming/video/overlaymanager.*` (ADR-0045, the HUD bitmap injection point) and
 * `app/streaming/session.cpp` (one window-title literal, ADR-0046, and the conntest server plus its
 * port-test relay, ADR-0104).
 *
 * Until this suite, that invariant was measured by hand in each verification pass and by a CI job
 * that has never run and is not a required check (security F-2: the branch is unprotected and the
 * job's `grep -Ev 'overlaymanager|session\.cpp'` is unanchored, so `overlaymanager_evil.cpp` would
 * pass it). A boundary that only a hand-run command measures is not a boundary. This suite is the
 * same measurement, executable, with whole-path comparison instead of a substring regex.
 *
 * It measures the WORKING TREE - committed, staged and unstaged tracked changes, plus untracked
 * files Git does not ignore - against the pinned upstream commit object itself. No tag or
 * remote-tracking ref is involved, so there is nothing to prepare before a run, and an edit that
 * is not yet committed is measured too (a task Verify runs before the patch is committed).
 *
 * It asserts four things:
 *
 *   1. The comparison base is the pinned upstream commit `f786e94c...` (v6.1.0, the value
 *      `seathub-ops/pins.yaml` records), so a re-fetch cannot silently move what "the boundary" is
 *      measured against. The commit object must be present; a shallow clone lacks it and the suite
 *      fails by name rather than skipping.
 *   2. Under `app/streaming/` the fork changes exactly the three documented files and nothing
 *      else, and nothing under `app/backend/` changes at all.
 *   3. The `session.cpp` exception is a title literal and nothing more: every added line carries
 *      the ADR-0046 title lines (2) or one of the four `SeatHub: ADR-0104` lines (the conntest
 *      definition, its include and two relay calls), and exactly two upstream lines are removed
 *      (the title literal and the conntest definition). The CI gate allowlists the whole *file*;
 *      this allowlists the *scope* ADR-0046 and ADR-0104 actually grant.
 *   4. `FORK-CHANGES.md` enumerates every file under the fork that upstream also has - the other
 *      half of STREAM-06, previously verified by hand.
 *
 * The last slots are the ones that keep the rest honest: each builds a throwaway repository whose
 * working tree edits a decoder (unstaged, staged, untracked or committed), runs the same
 * classifier over it, and requires it to report the violation. A check that cannot fail is not a
 * check.
 *
 * Build recipe:
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
 *   cd tests && qmake tst_d28_boundary.pro && jom && tst_d28_boundary.exe -o tst_d28_boundary-out.txt,txt
 *****************************************************************************/

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopedPointer>
#include <QSet>
#include <QTemporaryDir>

#include <functional>

#ifndef FORK_ROOT
#error "FORK_ROOT must be defined by the .pro file (the fork's repository root)"
#endif

namespace {

/// The pinned upstream commit - `v6.1.0`, the value `seathub-ops/pins.yaml` records as
/// `moonlight_qt.upstream_commit` and ADR-0041 restates.
const char* kPinnedUpstreamCommit = "f786e94c7b2f943e24e65d7d74deb539b827fc84";

/// The two exceptions, as whole repository-relative paths. ADR-0045 grants the overlay compositor;
/// ADR-0046 grants one window-title string literal inside `session.cpp`.
const char* kSessionCpp = "app/streaming/session.cpp";
const char* kOverlayManagerCpp = "app/streaming/video/overlaymanager.cpp";
const char* kOverlayManagerH = "app/streaming/video/overlaymanager.h";

/// What a missing base reports. There is no skip and no fallback to a tag, branch or HEAD.
const char* kMissingBaseMessage =
    "the pinned upstream commit f786e94c7b2f943e24e65d7d74deb539b827fc84 is not in this "
    "repository: the boundary is measured against that commit object, so the checkout needs "
    "full history (a shallow clone does not carry it)";

struct GitRun
{
    int exitCode = -1;
    QString out;
    QString err;
};

GitRun git(const QString& root, const QStringList& args, int timeoutMs = 60000)
{
    GitRun run;
    QProcess process;
    process.setWorkingDirectory(root);
    process.start(QStringLiteral("git"), args);
    if (!process.waitForFinished(timeoutMs)) {
        run.err = QStringLiteral("git did not finish within %1 ms").arg(timeoutMs);
        process.kill();
        process.waitForFinished(5000);
        return run;
    }
    run.exitCode = process.exitCode();
    run.out = QString::fromLocal8Bit(process.readAllStandardOutput());
    run.err = QString::fromLocal8Bit(process.readAllStandardError());
    return run;
}

/// A single grader over one repository. Everything it is asked is answerable from `git`, so the
/// synthetic probes in the last slots exercise exactly the code that grades the real fork.
class BoundaryCheck
{
public:
    BoundaryCheck(QString root, QString baseCommit)
        : m_root(std::move(root)), m_base(std::move(baseCommit))
    {
    }

    /// The commit object the base names, or an empty string when this repository does not hold
    /// it. `^{commit}` is required: a bare full hex passes `rev-parse --verify` even when the
    /// object is absent.
    QString resolveBase() const
    {
        const GitRun run = git(m_root, { QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                          QStringLiteral("--quiet"),
                                          m_base + QStringLiteral("^{commit}") });
        return run.exitCode == 0 ? run.out.trimmed() : QString();
    }

    /// True only when the base is a commit object this repository holds and it is exactly the
    /// 40-hex asked for.
    bool baseCommitPresent() const { return resolveBase() == m_base; }

    /// True once any git query against the base has failed. A failed query returns nothing, which
    /// would otherwise read as "nothing changed", so the slots assert this stays false.
    bool gitFailed() const { return m_gitFailed; }

    /// Tracked changes against the base: committed, staged and unstaged together (the working
    /// tree against the base commit, with no `..HEAD`).
    QStringList changedTrackedPaths(const QStringList& pathspecs = QStringList()) const
    {
        QStringList args { QStringLiteral("diff"), QStringLiteral("--name-only"), m_base };
        if (!pathspecs.isEmpty()) {
            args.append(QStringLiteral("--"));
            args.append(pathspecs);
        }
        const GitRun run = git(m_root, args);
        if (run.exitCode != 0) {
            m_gitFailed = true;
        }
        return run.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    /// Files Git does not track and does not ignore. An ignored file can never be staged into a
    /// landed patch, so it is not measured.
    QStringList untrackedPaths(const QStringList& pathspecs) const
    {
        QStringList args { QStringLiteral("ls-files"), QStringLiteral("--others"),
                           QStringLiteral("--exclude-standard") };
        if (!pathspecs.isEmpty()) {
            args.append(QStringLiteral("--"));
            args.append(pathspecs);
        }
        const GitRun run = git(m_root, args);
        if (run.exitCode != 0) {
            m_gitFailed = true;
        }
        return run.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    /// Every path under the pathspecs that differs from the base in the working tree: tracked
    /// changes plus untracked new files.
    QStringList changedPaths(const QStringList& pathspecs) const
    {
        QStringList all = changedTrackedPaths(pathspecs);
        all.append(untrackedPaths(pathspecs));
        return all;
    }

    QString diffText(const QStringList& pathspecs) const
    {
        QStringList args { QStringLiteral("diff"), m_base };
        args.append(QStringLiteral("--"));
        args.append(pathspecs);
        const GitRun run = git(m_root, args);
        if (run.exitCode != 0) {
            m_gitFailed = true;
        }
        return run.out;
    }

    /// True only for a whole-path match. ADR-0045's compositor and ADR-0046's `session.cpp`, and
    /// nothing that merely spells a similar name.
    static bool isDocumentedException(const QString& path)
    {
        return path == QLatin1String(kSessionCpp) || path == QLatin1String(kOverlayManagerCpp)
            || path == QLatin1String(kOverlayManagerH);
    }

    /// Every changed path that the D-28 boundary forbids: anything under `app/streaming/` that is
    /// not one of the two documented exceptions, and anything under `app/backend/` at all.
    static QStringList violations(const QStringList& changed)
    {
        QStringList bad;
        for (const QString& path : changed) {
            const QString normalized = QDir::fromNativeSeparators(path);
            if (normalized.startsWith(QLatin1String("app/streaming/"))) {
                if (!isDocumentedException(normalized)) {
                    bad.append(normalized);
                }
            }
            else if (normalized.startsWith(QLatin1String("app/backend/"))) {
                bad.append(normalized);
            }
        }
        bad.sort();
        return bad;
    }

    /// The lines the diff adds, without the `+++` header.
    static QStringList addedLines(const QString& diff)
    {
        QStringList lines;
        for (const QString& line : diff.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("+++"))) {
                continue;
            }
            if (line.startsWith(QLatin1Char('+'))) {
                lines.append(line.mid(1));
            }
        }
        return lines;
    }

    /// The lines the diff removes, without the `---` header.
    static QStringList removedLines(const QString& diff)
    {
        QStringList lines;
        for (const QString& line : diff.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("---"))) {
                continue;
            }
            if (line.startsWith(QLatin1Char('-'))) {
                lines.append(line.mid(1));
            }
        }
        return lines;
    }

private:
    QString m_root;
    QString m_base;
    mutable bool m_gitFailed = false;
};

/// Grades one `session.cpp` diff against the two grants that touch it (empty means it is within
/// both): ADR-0046's window title (a marker comment and the literal, 2 added lines, 1 removed) and
/// ADR-0104's conntest change (the `CONN_TEST_SERVER` definition, one include and two
/// `PortTestRelay::publish` calls, 4 added lines marked `SeatHub: ADR-0104`, 1 removed). Counts are
/// equalities, and every added line must belong to exactly one of the six named lines, so an
/// unmarked line and a seventh marked line both fail.
QString sessionCppExceptionProblem(const QStringList& added, const QStringList& removed)
{
    int title = 0, define = 0, include = 0, publish = 0;
    for (const QString& line : added) {
        const bool adr0104 = line.contains(QLatin1String("SeatHub: ADR-0104"));
        const bool adr0046 = line.contains(QLatin1String("ADR-0046"));
        if (adr0104 && !adr0046) {
            if (line.contains(QLatin1String("#define CONN_TEST_SERVER"))
                && line.contains(QLatin1String("\"conntest.rentapc.online\""))) {
                ++define;
            } else if (line.contains(QLatin1String("#include"))
                       && line.contains(QLatin1String("seathub/port_test_relay.h"))) {
                ++include;
            } else if (line.contains(QLatin1String("seathub::PortTestRelay::publish("))) {
                ++publish;
            } else {
                return QStringLiteral("an ADR-0104 line is not the conntest definition, its "
                                      "include or a relay call: %1").arg(line.simplified());
            }
        } else if (adr0046 && !adr0104) {
            ++title;
        } else {
            return QStringLiteral("an added line is neither an ADR-0046 title line nor a "
                                  "`SeatHub: ADR-0104` line: %1").arg(line.simplified());
        }
    }
    if (added.size() != 6 || title != 2 || define != 1 || include != 1 || publish != 2) {
        return QStringLiteral("session.cpp adds %1 lines (title %2, define %3, include %4, publish "
                              "%5); the grants cover exactly 6 (2, 1, 1, 2)")
            .arg(added.size()).arg(title).arg(define).arg(include).arg(publish);
    }

    int oldTitle = 0, oldConntest = 0;
    for (const QString& line : removed) {
        if (line.contains(QLatin1String("#define CONN_TEST_SERVER"))
            && line.contains(QLatin1String("qt.conntest.moonlight-stream.org"))) {
            ++oldConntest;
        } else if (line.contains(QLatin1String("Moonlight"))) {
            ++oldTitle;
        } else {
            return QStringLiteral("a removed line is neither upstream's title literal nor its "
                                  "conntest definition: %1").arg(line.simplified());
        }
    }
    if (removed.size() != 2 || oldTitle != 1 || oldConntest != 1) {
        return QStringLiteral("session.cpp removes %1 lines; the grants cover exactly 2").arg(removed.size());
    }
    return QString();
}

QString readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

bool writeFile(const QString& path, const QByteArray& data, QIODevice::OpenMode mode)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(mode)) {
        return false;
    }
    file.write(data);
    return true;
}

bool commitAll(const QString& root, const QString& message)
{
    const QStringList identity { QStringLiteral("-c"),
                                 QStringLiteral("user.email=test@example.invalid"),
                                 QStringLiteral("-c"), QStringLiteral("user.name=Boundary Test"),
                                 QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false") };
    return git(root, { QStringLiteral("add"), QStringLiteral("-A") }).exitCode == 0
        && git(root,
               QStringList(identity)
                   + QStringList { QStringLiteral("commit"), QStringLiteral("-m"), message })
                   .exitCode
            == 0;
}

const char* kDecoder = "app/streaming/video/d3d11va.cpp";

/// What the grader reports for one throwaway repository, after `edit` has changed its working
/// tree. The repository's own first commit is the base: it holds the "upstream" decoder.
struct Probe
{
    bool built = false;
    bool basePresent = false;
    bool gitFailed = false;
    QStringList engine;
    QStringList backend;
};

Probe probeAfter(const std::function<bool(const QString& root)>& edit)
{
    Probe probe;
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return probe;
    }
    const QString root = scratch.path();
    if (git(root, { QStringLiteral("init") }).exitCode != 0
        || !writeFile(QDir(root).filePath(QString::fromLatin1(kDecoder)), "// upstream\n",
                      QIODevice::WriteOnly)
        || !commitAll(root, QStringLiteral("upstream"))) {
        return probe;
    }
    const GitRun head = git(root, { QStringLiteral("rev-parse"), QStringLiteral("HEAD") });
    const QString base = head.out.trimmed();
    if (head.exitCode != 0 || base.size() != 40 || !edit(root)) {
        return probe;
    }

    const BoundaryCheck check(root, base);
    probe.built = true;
    probe.basePresent = check.baseCommitPresent();
    probe.engine = check.changedPaths({ QStringLiteral("app/streaming/") });
    probe.backend = check.changedPaths({ QStringLiteral("app/backend/") });
    probe.gitFailed = check.gitFailed();
    return probe;
}

bool appendToDecoder(const QString& root)
{
    return writeFile(QDir(root).filePath(QString::fromLatin1(kDecoder)),
                     "// a SeatHub edit the D-28 boundary forbids\n", QIODevice::Append);
}

} // namespace

class TstD28Boundary : public QObject
{
    Q_OBJECT

private:
    static BoundaryCheck forkCheck()
    {
        return BoundaryCheck(QStringLiteral(FORK_ROOT), QString::fromLatin1(kPinnedUpstreamCommit));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo(QStringLiteral(FORK_ROOT)).isDir(),
                 "FORK_ROOT must point at the fork's repository root");
        QVERIFY2(QFileInfo(QStringLiteral(FORK_ROOT "/.git")).exists()
                     || QFileInfo(QStringLiteral(FORK_ROOT "/.git")).isFile(),
                 "FORK_ROOT must be a git work tree");
    }

    // --- the base is pinned, so the measurement cannot move under us ---------------------------

    void theComparisonBaseIsThePinnedUpstreamCommit()
    {
        const BoundaryCheck check = forkCheck();
        const QString resolved = check.resolveBase();
        QVERIFY2(!resolved.isEmpty(), kMissingBaseMessage);
        QVERIFY2(resolved == QString::fromLatin1(kPinnedUpstreamCommit),
                 qPrintable(QStringLiteral("the base resolved to %1, not the pinned upstream "
                                           "commit %2")
                                .arg(resolved, QString::fromLatin1(kPinnedUpstreamCommit))));
    }

    // --- the engine boundary -------------------------------------------------------------------

    void theEngineDirectoryChangesOnlyTheDocumentedExceptions()
    {
        const BoundaryCheck check = forkCheck();
        QVERIFY2(check.baseCommitPresent(), kMissingBaseMessage);

        const QStringList engine = check.changedPaths({ QStringLiteral("app/streaming/") });
        QVERIFY2(!check.gitFailed(), "a git query against the pinned base failed");
        QVERIFY2(BoundaryCheck::violations(engine).isEmpty(),
                 qPrintable(QStringLiteral("changes under app/streaming/ that D-28 forbids: %1")
                                .arg(BoundaryCheck::violations(engine).join(QLatin1Char(' ')))));

        // Exactly the two exceptions ADR-0045 and ADR-0046 name - no more, and no fewer: a stale
        // exception list is how a future amendment quietly widens the boundary without an ADR.
        QStringList expected { QString::fromLatin1(kSessionCpp),
                               QString::fromLatin1(kOverlayManagerCpp),
                               QString::fromLatin1(kOverlayManagerH) };
        expected.sort();
        QStringList actual = engine;
        actual.sort();
        QCOMPARE(actual, expected);
    }

    void noBackendFileChangesAtAll()
    {
        const BoundaryCheck check = forkCheck();
        QVERIFY2(check.baseCommitPresent(), kMissingBaseMessage);

        const QStringList backend = check.changedPaths({ QStringLiteral("app/backend/") });
        QVERIFY2(!check.gitFailed(), "a git query against the pinned base failed");
        QVERIFY2(backend.isEmpty(),
                 qPrintable(QStringLiteral("app/backend/ is not part of the fork boundary's "
                                           "exceptions, but these changed: %1")
                                .arg(backend.join(QLatin1Char(' ')))));
    }

    // --- the exception is a literal, not a licence to edit the file ----------------------------

    void theSessionCppExceptionIsTheTitleLiteralAndNothingElse()
    {
        const BoundaryCheck check = forkCheck();
        QVERIFY2(check.baseCommitPresent(), kMissingBaseMessage);

        const QString diff =
            check.diffText({ QString::fromLatin1(kSessionCpp) });
        QVERIFY2(!check.gitFailed(), "a git query against the pinned base failed");
        QVERIFY2(!diff.trimmed().isEmpty(),
                 "session.cpp is expected to carry the ADR-0046 title and the ADR-0104 change");

        const QStringList added = BoundaryCheck::addedLines(diff);
        const QStringList removed = BoundaryCheck::removedLines(diff);

        // ADR-0046 grants the window title (marker comment + literal) and ADR-0104 the conntest
        // change (definition, include, two relay calls): six added lines, two removed, nothing else.
        const QString problem = sessionCppExceptionProblem(added, removed);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        // The same predicate must refuse a diff that is the authorised one plus one more line.
        const QString unmarked = QStringLiteral("    int stray = 0;");
        const QString extraMarked = QStringLiteral("    stray(); // SeatHub: ADR-0104");
        QVERIFY2(!sessionCppExceptionProblem(added + QStringList { unmarked }, removed).isEmpty(),
                 "an unmarked extra added line must be refused");
        QVERIFY2(!sessionCppExceptionProblem(added + QStringList { extraMarked }, removed).isEmpty(),
                 "an extra `SeatHub: ADR-0104` line must be refused");
    }

    // --- the other half of STREAM-06: every modified upstream file is enumerated ---------------

    void forkChangesEnumeratesEveryUpstreamFileTheForkModifies()
    {
        const BoundaryCheck check = forkCheck();
        QVERIFY2(check.baseCommitPresent(), kMissingBaseMessage);

        const QString manifestPath = QStringLiteral(FORK_ROOT "/FORK-CHANGES.md");
        const QString manifest = readAll(manifestPath);
        QVERIFY2(!manifest.isEmpty(), "FORK-CHANGES.md must exist and be readable");

        const GitRun tree = git(QStringLiteral(FORK_ROOT),
                                { QStringLiteral("ls-tree"), QStringLiteral("-r"),
                                  QStringLiteral("--name-only"),
                                  QString::fromLatin1(kPinnedUpstreamCommit) });
        QCOMPARE(tree.exitCode, 0);
        const QStringList upstreamFiles =
            tree.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QSet<QString> upstream =
            QSet<QString>(upstreamFiles.constBegin(), upstreamFiles.constEnd());

        QStringList missing;
        // Tracked changes only: a file that is untracked is new to the fork and needs no row, and
        // so is any file the pinned commit does not have. Only modifications to upstream files do.
        for (const QString& path : check.changedTrackedPaths()) {
            if (!upstream.contains(path)) {
                continue;
            }
            if (!manifest.contains(path)) {
                missing.append(path);
            }
        }
        QVERIFY2(!check.gitFailed(), "a git query against the pinned base failed");

        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("modified upstream files FORK-CHANGES.md does not "
                                           "enumerate: %1")
                                .arg(missing.join(QLatin1Char(' ')))));
    }

    // --- the check has teeth -------------------------------------------------------------------

    void theCheckWouldCatchAnEngineEdit()
    {
        // The verifier found the CI diff gate unanchored and never run (F-2). This is the same
        // question asked of this suite instead: does the classifier actually refuse an engine
        // edit? The repository below is built for the purpose and thrown away, and its own first
        // commit is the base, so nothing about it needs the real pinned commit.
        const Probe probe = probeAfter([](const QString& root) {
            return appendToDecoder(root) && commitAll(root, QStringLiteral("edit"));
        });
        QVERIFY(probe.built);
        QVERIFY(probe.basePresent);
        QVERIFY(!probe.gitFailed);
        QCOMPARE(probe.engine, QStringList { QString::fromLatin1(kDecoder) });

        const QStringList bad = BoundaryCheck::violations(probe.engine);
        QCOMPARE(bad, QStringList { QString::fromLatin1(kDecoder) });

        // And the equality assertion the real fork passes - "under app/streaming/ exactly the two
        // documented exceptions" - does not hold here. That is the "fails before" half of this
        // suite: the same measurement, on a repository that breaks the boundary.
        QStringList expected { QString::fromLatin1(kSessionCpp),
                               QString::fromLatin1(kOverlayManagerCpp),
                               QString::fromLatin1(kOverlayManagerH) };
        expected.sort();
        QStringList actual = probe.engine;
        actual.sort();
        QVERIFY2(actual != expected,
                 "the probe repository's changes must not look like the documented exception set");

        // And the near-misses the CI gate's unanchored pattern would let through. A whole-path
        // allowlist is what makes the difference.
        QVERIFY(BoundaryCheck::violations({ QStringLiteral("app/streaming/video/overlaymanager_evil.cpp") })
                    .size()
                == 1);
        QVERIFY(BoundaryCheck::violations({ QStringLiteral("app/streaming/audio/not_session.cpp") })
                    .size()
                == 1);
        QVERIFY(BoundaryCheck::violations({ QStringLiteral("app/backend/nvhttp.cpp") }).size() == 1);
        QVERIFY(BoundaryCheck::violations({ QString::fromLatin1(kSessionCpp),
                                            QString::fromLatin1(kOverlayManagerCpp),
                                            QString::fromLatin1(kOverlayManagerH) })
                    .isEmpty());
    }

    void theCheckWouldCatchAnUnstagedEngineEdit()
    {
        // The edit exists only in the working tree: not staged, not committed. A task Verify runs
        // in exactly this state, before the patch is committed.
        const Probe probe = probeAfter([](const QString& root) { return appendToDecoder(root); });
        QVERIFY(probe.built);
        QVERIFY(probe.basePresent);
        QVERIFY(!probe.gitFailed);
        QCOMPARE(BoundaryCheck::violations(probe.engine),
                 QStringList { QString::fromLatin1(kDecoder) });
    }

    void theCheckWouldCatchAStagedEngineEdit()
    {
        const Probe probe = probeAfter([](const QString& root) {
            return appendToDecoder(root)
                && git(root, { QStringLiteral("add"), QStringLiteral("-A") }).exitCode == 0;
        });
        QVERIFY(probe.built);
        QVERIFY(probe.basePresent);
        QVERIFY(!probe.gitFailed);
        QCOMPARE(BoundaryCheck::violations(probe.engine),
                 QStringList { QString::fromLatin1(kDecoder) });
    }

    void theCheckWouldCatchAnUntrackedNewEngineFile()
    {
        const QString newEngineFile = QStringLiteral("app/streaming/video/newdecoder.cpp");
        const Probe probe = probeAfter([&newEngineFile](const QString& root) {
            return writeFile(QDir(root).filePath(newEngineFile), "// brand new\n",
                             QIODevice::WriteOnly);
        });
        QVERIFY(probe.built);
        QVERIFY(probe.basePresent);
        QVERIFY(!probe.gitFailed);
        QCOMPARE(BoundaryCheck::violations(probe.engine), QStringList { newEngineFile });
    }

    void theCheckWouldCatchAnUntrackedNewBackendFile()
    {
        const QString newBackendFile = QStringLiteral("app/backend/newhttp.cpp");
        const Probe probe = probeAfter([&newBackendFile](const QString& root) {
            return writeFile(QDir(root).filePath(newBackendFile), "// brand new\n",
                             QIODevice::WriteOnly);
        });
        QVERIFY(probe.built);
        QVERIFY(!probe.gitFailed);
        QCOMPARE(probe.backend, QStringList { newBackendFile });
        QCOMPARE(BoundaryCheck::violations(probe.backend), QStringList { newBackendFile });
    }

    void aFileGitIgnoresIsNotMeasuredButAnUnchangedTreeIsClean()
    {
        // Two controls so the probes above cannot be passing by accident: an untouched tree
        // reports nothing, and an ignored file (which Git can never stage into a landed patch) is
        // not counted.
        const Probe clean = probeAfter([](const QString&) { return true; });
        QVERIFY(clean.built);
        QVERIFY(!clean.gitFailed);
        QVERIFY(clean.engine.isEmpty());
        QVERIFY(clean.backend.isEmpty());

        const Probe ignored = probeAfter([](const QString& root) {
            return writeFile(QDir(root).filePath(QStringLiteral(".gitignore")),
                             "ignored_engine.cpp\n", QIODevice::WriteOnly)
                && writeFile(QDir(root).filePath(QStringLiteral("app/streaming/video/ignored_engine.cpp")),
                             "// never staged\n", QIODevice::WriteOnly);
        });
        QVERIFY(ignored.built);
        QVERIFY(!ignored.gitFailed);
        QVERIFY(ignored.engine.isEmpty());
    }

    void anAbsentPinnedBaseFailsClosed()
    {
        // The scratch repository does not hold the pinned upstream commit, which is what a
        // shallow clone looks like. The base check must say so, and a diff against the missing
        // base must be flagged as failed rather than read as "nothing changed".
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString root = scratch.path();
        QCOMPARE(git(root, { QStringLiteral("init") }).exitCode, 0);
        QVERIFY(writeFile(QDir(root).filePath(QString::fromLatin1(kDecoder)), "// upstream\n",
                          QIODevice::WriteOnly));
        QVERIFY(commitAll(root, QStringLiteral("upstream")));

        const BoundaryCheck check(root, QString::fromLatin1(kPinnedUpstreamCommit));
        QVERIFY(!check.baseCommitPresent());
        QVERIFY(check.resolveBase().isEmpty());
        QVERIFY(check.changedPaths({ QStringLiteral("app/backend/") }).isEmpty());
        QVERIFY2(check.gitFailed(),
                 "a diff against an absent base must be reported as a failure, not as no change");
    }
};

QTEST_MAIN(TstD28Boundary)

#include "tst_d28_boundary.moc"

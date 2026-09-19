/*****************************************************************************
 * SeatHub fork - the D-28 fork boundary as an executable check (STREAM-06).
 *
 * The boundary is the phase's most safety-critical structural invariant: the streaming engine is
 * upstream's, unmodified, apart from two named exceptions with their own ADRs -
 * `app/streaming/video/overlaymanager.*` (ADR-0045, the HUD bitmap injection point) and one
 * window-title string literal in `app/streaming/session.cpp` (ADR-0046).
 *
 * Until this suite, that invariant was measured by hand in each verification pass and by a CI job
 * that has never run and is not a required check (security F-2: the branch is unprotected and the
 * job's `grep -Ev 'overlaymanager|session\.cpp'` is unanchored, so `overlaymanager_evil.cpp` would
 * pass it). A boundary that only a hand-run command measures is not a boundary. This suite is the
 * same measurement, executable, with whole-path comparison instead of a substring regex.
 *
 * It asserts four things:
 *
 *   1. The comparison base is the pinned upstream commit `f786e94c...` (v6.1.0, the value
 *      `seathub-ops/pins.yaml` records), so a re-fetch cannot silently move what "the boundary" is
 *      measured against.
 *   2. Under `app/streaming/` the fork changes exactly the three documented files and nothing
 *      else, and nothing under `app/backend/` changes at all.
 *   3. The `session.cpp` exception is a title literal and nothing more: every added line carries
 *      the `SeatHub`/ADR-0046 marker, the single removed line is the upstream literal, and the
 *      diff is no larger than that. The CI gate allowlists the whole *file*; this allowlists the
 *      *scope* ADR-0046 actually grants.
 *   4. `FORK-CHANGES.md` enumerates every file under the fork that upstream also has - the other
 *      half of STREAM-06, previously verified by hand.
 *
 * The last slot is the one that keeps the rest honest: it builds a throwaway repository whose
 * branch edits a decoder, runs the same classifier over it, and requires it to report the
 * violation. A check that cannot fail is not a check.
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

#ifndef FORK_ROOT
#error "FORK_ROOT must be defined by the .pro file (the fork's repository root)"
#endif

namespace {

/// The pinned upstream commit - `v6.1.0`, the value `seathub-ops/pins.yaml` records as
/// `moonlight_qt.upstream_commit` and ADR-0041 restates.
const char* kPinnedUpstreamCommit = "f786e94c7b2f943e24e65d7d74deb539b827fc84";
const char* kUpstreamRef = "origin/v6.1.0";

/// The two exceptions, as whole repository-relative paths. ADR-0045 grants the overlay compositor;
/// ADR-0046 grants one window-title string literal inside `session.cpp`.
const char* kSessionCpp = "app/streaming/session.cpp";
const char* kOverlayManagerCpp = "app/streaming/video/overlaymanager.cpp";
const char* kOverlayManagerH = "app/streaming/video/overlaymanager.h";

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
/// synthetic probe in the last slot exercises exactly the code that grades the real fork.
class BoundaryCheck
{
public:
    explicit BoundaryCheck(QString root) : m_root(std::move(root)) {}

    bool hasBaseRef() const
    {
        const GitRun run = git(m_root, { QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                          QString::fromLatin1(kUpstreamRef) });
        return run.exitCode == 0 && !run.out.trimmed().isEmpty();
    }

    QString resolve(const QString& ref) const
    {
        return git(m_root, { QStringLiteral("rev-parse"), QStringLiteral("--verify"), ref })
            .out.trimmed();
    }

    QStringList changedPaths(const QStringList& pathspecs = QStringList()) const
    {
        QStringList args { QStringLiteral("diff"), QStringLiteral("--name-only"),
                           QStringLiteral("%1..HEAD").arg(QString::fromLatin1(kUpstreamRef)) };
        if (!pathspecs.isEmpty()) {
            args.append(QStringLiteral("--"));
            args.append(pathspecs);
        }
        const GitRun run = git(m_root, args);
        return run.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    QString diffText(const QStringList& pathspecs) const
    {
        QStringList args { QStringLiteral("diff"), QStringLiteral("%1..HEAD")
                               .arg(QString::fromLatin1(kUpstreamRef)) };
        args.append(QStringLiteral("--"));
        args.append(pathspecs);
        return git(m_root, args).out;
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
};

QString readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

class TstD28Boundary : public QObject
{
    Q_OBJECT

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
        BoundaryCheck check(QStringLiteral(FORK_ROOT));
        QVERIFY2(check.hasBaseRef(),
                 "origin/v6.1.0 must exist: the whole boundary is measured against it");
        const QString resolved = check.resolve(QString::fromLatin1(kUpstreamRef));
        QVERIFY2(resolved.startsWith(QString::fromLatin1(kPinnedUpstreamCommit)),
                 qPrintable(QStringLiteral("origin/v6.1.0 resolved to %1, not the pinned "
                                           "upstream commit %2")
                                .arg(resolved, QString::fromLatin1(kPinnedUpstreamCommit))));
    }

    // --- the engine boundary -------------------------------------------------------------------

    void theEngineDirectoryChangesOnlyTheDocumentedExceptions()
    {
        BoundaryCheck check(QStringLiteral(FORK_ROOT));

        const QStringList engine = check.changedPaths({ QStringLiteral("app/streaming/") });
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
        BoundaryCheck check(QStringLiteral(FORK_ROOT));
        const QStringList backend = check.changedPaths({ QStringLiteral("app/backend/") });
        QVERIFY2(backend.isEmpty(),
                 qPrintable(QStringLiteral("app/backend/ is not part of the fork boundary's "
                                           "exceptions, but these changed: %1")
                                .arg(backend.join(QLatin1Char(' ')))));
    }

    // --- the exception is a literal, not a licence to edit the file ----------------------------

    void theSessionCppExceptionIsTheTitleLiteralAndNothingElse()
    {
        BoundaryCheck check(QStringLiteral(FORK_ROOT));
        const QString diff =
            check.diffText({ QString::fromLatin1(kSessionCpp) });
        QVERIFY2(!diff.trimmed().isEmpty(), "session.cpp is expected to carry the ADR-0046 title");

        const QStringList added = BoundaryCheck::addedLines(diff);
        const QStringList removed = BoundaryCheck::removedLines(diff);

        // ADR-0046 grants one string literal. Two changed lines are the marker comment and the
        // literal; anything beyond that is code the ADR does not cover.
        QVERIFY2(added.size() <= 3,
                 qPrintable(QStringLiteral("session.cpp's diff adds %1 lines; the ADR-0046 "
                                           "exception covers the window title only")
                                .arg(added.size())));

        for (const QString& line : added) {
            QVERIFY2(line.contains(QLatin1String("SeatHub"))
                         || line.contains(QLatin1String("ADR-0046")),
                     qPrintable(QStringLiteral("an added line in session.cpp is not the title "
                                               "literal or its ADR marker: %1")
                                    .arg(line.simplified())));
        }

        QCOMPARE(removed.size(), 1);
        QVERIFY2(removed.first().contains(QLatin1String("Moonlight")),
                 qPrintable(QStringLiteral("the single removed line is expected to be upstream's "
                                           "title literal, not: %1")
                                .arg(removed.first().simplified())));
    }

    // --- the other half of STREAM-06: every modified upstream file is enumerated ---------------

    void forkChangesEnumeratesEveryUpstreamFileTheForkModifies()
    {
        BoundaryCheck check(QStringLiteral(FORK_ROOT));
        const QString manifestPath = QStringLiteral(FORK_ROOT "/FORK-CHANGES.md");
        const QString manifest = readAll(manifestPath);
        QVERIFY2(!manifest.isEmpty(), "FORK-CHANGES.md must exist and be readable");

        const GitRun tree = git(QStringLiteral(FORK_ROOT),
                                { QStringLiteral("ls-tree"), QStringLiteral("-r"),
                                  QStringLiteral("--name-only"), QString::fromLatin1(kUpstreamRef) });
        QCOMPARE(tree.exitCode, 0);
        const QStringList upstreamFiles =
            tree.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QSet<QString> upstream =
            QSet<QString>(upstreamFiles.constBegin(), upstreamFiles.constEnd());

        QStringList missing;
        for (const QString& path : check.changedPaths()) {
            // A file that does not exist upstream is new to the fork and needs no row: only
            // modifications to upstream files do.
            if (!upstream.contains(path)) {
                continue;
            }
            if (!manifest.contains(path)) {
                missing.append(path);
            }
        }

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
        // edit? The repository below is built for the purpose and thrown away.
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString root = scratch.path();

        QCOMPARE(git(root, { QStringLiteral("init") }).exitCode, 0);
        QDir(root).mkpath(QStringLiteral("app/streaming/video"));
        QFile decoder(QDir(root).filePath(QStringLiteral("app/streaming/video/d3d11va.cpp")));
        QVERIFY(decoder.open(QIODevice::WriteOnly));
        decoder.write("// upstream\n");
        decoder.close();

        const QStringList identity { QStringLiteral("-c"),
                                     QStringLiteral("user.email=test@example.invalid"),
                                     QStringLiteral("-c"), QStringLiteral("user.name=Boundary Test"),
                                     QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false") };
        QCOMPARE(git(root, QStringList { QStringLiteral("add"), QStringLiteral("-A") }).exitCode, 0);
        QCOMPARE(git(root, QStringList(identity) + QStringList { QStringLiteral("commit"),
                                                                 QStringLiteral("-m"),
                                                                 QStringLiteral("upstream") })
                     .exitCode,
                 0);
        QCOMPARE(git(root, { QStringLiteral("tag"), QString::fromLatin1(kUpstreamRef) }).exitCode, 0);

        QVERIFY(decoder.open(QIODevice::Append));
        decoder.write("// a SeatHub edit the D-28 boundary forbids\n");
        decoder.close();
        QCOMPARE(git(root, QStringList { QStringLiteral("add"), QStringLiteral("-A") }).exitCode, 0);
        QCOMPARE(git(root, QStringList(identity) + QStringList { QStringLiteral("commit"),
                                                                 QStringLiteral("-m"),
                                                                 QStringLiteral("edit") })
                     .exitCode,
                 0);

        BoundaryCheck probe(root);
        QVERIFY(probe.hasBaseRef());
        const QStringList engine = probe.changedPaths({ QStringLiteral("app/streaming/") });
        QCOMPARE(engine, QStringList { QStringLiteral("app/streaming/video/d3d11va.cpp") });

        const QStringList bad = BoundaryCheck::violations(engine);
        QCOMPARE(bad, QStringList { QStringLiteral("app/streaming/video/d3d11va.cpp") });

        // And the equality assertion the real fork passes - "under app/streaming/ exactly the two
        // documented exceptions" - does not hold here. That is the "fails before" half of this
        // suite: the same measurement, on a repository that breaks the boundary.
        QStringList expected { QString::fromLatin1(kSessionCpp),
                               QString::fromLatin1(kOverlayManagerCpp),
                               QString::fromLatin1(kOverlayManagerH) };
        expected.sort();
        QStringList actual = engine;
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
};

QTEST_MAIN(TstD28Boundary)

#include "tst_d28_boundary.moc"

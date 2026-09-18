/*****************************************************************************
 * SeatHub fork — unit tests for the error model (D-51, ADR-0008).
 *
 * These live in their own top-level `tests/` project because upstream moonlight-qt
 * ships no test tree at all; the fork adds one. They link the bridge's own
 * translation unit (`app/seathub/error_map.cpp`) directly rather than the whole
 * application, so a mapping regression fails here in seconds instead of requiring a
 * full streaming build.
 *
 * Behaviour covered (Plan 03-02 Task 2):
 *   1. connection-refused stage  -> SeatHub copy + reference, kind Engine
 *   2. decoder stage            -> SeatHub copy + reference, kind Engine
 *   3. displayLaunchError       -> generic copy; the engine's own words never reach
 *                                  the customer-facing field, only `diagnostic`
 *   4. unmapped stage           -> the D-51 generic fallback
 *   5. ErrorScreen.qml renders the reason and the reference code in mono type
 *   6. `diagnostic` never crosses into the view model handed to QML
 *****************************************************************************/

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QUrl>

#include "seathub/error_map.h"

namespace {

// The mono family the design tokens ask for. `docs/spec/copy.md` §5 microcopy rules
// put reference codes (`SH-4F7KQ2`) in mono and never translate them.
const char* kMonoFamily = "Geist Mono";

QString guiDir()
{
    // Walk up from the test binary's directory to find the fork's app/gui, so the test works
    // whether qmake built it in-source (tests/) or into the fork's shadow build tree.
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 8; ++depth) {
        const QString candidate = dir.filePath(QStringLiteral("app/gui"));
        if (QFile::exists(candidate + QStringLiteral("/ErrorScreen.qml"))) {
            return QDir::cleanPath(candidate);
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

// Walks the instantiated item tree for anything that has both `text` and `font`,
// which is how QQuickText and the Controls text-bearing items expose themselves.
// Read through the meta-object so the test needs no private Qt headers.
QList<QObject*> textItems(QObject* root)
{
    QList<QObject*> items;
    const auto children = root->findChildren<QObject*>();
    for (QObject* child : children) {
        const QMetaObject* metaObject = child->metaObject();
        if (metaObject->indexOfProperty("text") >= 0 && metaObject->indexOfProperty("font") >= 0) {
            items.append(child);
        }
    }
    return items;
}

} // namespace

class TstErrorMap : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void stageFailure_connectionRefused();
    void stageFailure_decoderUnavailable();
    void launchError_neverExposesEngineText();
    void stageFailure_unmappedFallsBackToGeneric();
    void diagnostic_neverReachesTheViewModel();
    void errorScreen_rendersReasonAndMonoReference();
};

void TstErrorMap::initTestCase()
{
    // The error model is a plain Qt Core translation unit; assert that up front so a
    // later accidental dependency on the streaming engine fails loudly here.
    QVERIFY(!guiDir().isEmpty());
}

void TstErrorMap::stageFailure_connectionRefused()
{
    const SeatHubFailure failure = mapStageFailure(QStringLiteral("connection_refused"), 0, QString());

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Can't reach the rig right now."));
    QCOMPARE(failure.reference, QStringLiteral("SH-CONREF"));
    QCOMPARE(failure.statusCode, 0);
    QVERIFY2(!failure.error.contains(QStringLiteral("SH-"), Qt::CaseInsensitive),
             "the reference is rendered separately; embedding it in the reason double-prints it");
}

void TstErrorMap::stageFailure_decoderUnavailable()
{
    const SeatHubFailure failure = mapStageFailure(QStringLiteral("decoder_unavailable"), 0, QString());

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error,
             QStringLiteral("This rig can't decode the stream. Try a different quality setting."));
    QCOMPARE(failure.reference, QStringLiteral("SH-DECUNAV"));
}

void TstErrorMap::launchError_neverExposesEngineText()
{
    const QString engineWords = QStringLiteral("Some raw Moonlight text");
    const SeatHubFailure failure = mapLaunchError(engineWords);

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Something went wrong on our side."));
    QCOMPARE(failure.reference, QStringLiteral("SH-GENERR"));

    // The whole point of the interception (T-03-05): nothing of the engine's reaches
    // the customer-facing field, but the diagnostic is preserved for support
    // (Pitfall 7 — dropping streaming diagnostics entirely is its own defect).
    QVERIFY(!failure.error.contains(engineWords));
    QVERIFY(!failure.error.contains(QStringLiteral("Moonlight"), Qt::CaseInsensitive));
    QCOMPARE(failure.diagnostic, engineWords);
}

void TstErrorMap::stageFailure_unmappedFallsBackToGeneric()
{
    // The engine's stage names come from moonlight-common-c's own table, so any stage
    // the mapping does not recognise must degrade to the D-51 fallback rather than
    // leaking the engine's stage name to the customer.
    const SeatHubFailure failure =
        mapStageFailure(QStringLiteral("STAGE_SOMETHING_WE_HAVE_NEVER_SEEN"), 1234, QStringLiteral("47989,47984"));

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Something went wrong on our side."));
    QCOMPARE(failure.reference, QStringLiteral("SH-GENERR"));
    QVERIFY(!failure.error.contains(QStringLiteral("STAGE_SOMETHING")));
    QVERIFY2(!failure.error.contains(QStringLiteral("47989")),
             "failing ports are engine detail and are never shown to a customer");
    // copy.md §Support & errors illustrates the sentence with the *sample* code SH-9K2XQ1.
    // Printing the sample hands the customer a code support cannot resolve, which ADR-0008
    // calls out as worse than showing no reference at all.
    QVERIFY2(!failure.error.contains(QStringLiteral("SH-9K2XQ1")),
             "copy.md's example reference must never be shown as if it were this incident's");
}

void TstErrorMap::diagnostic_neverReachesTheViewModel()
{
    // D-51 / the QML -> C++ trust boundary: what QML receives must not carry the
    // engine's raw words, so the diagnostic field is deliberately absent from the map.
    SeatHubFailure failure = mapLaunchError(QStringLiteral("Moonlight: decoder init failed"));
    failure.statusCode = 500;
    failure.failure = QStringLiteral("NO_HOST_AVAILABLE");

    const QVariantMap viewModel = failure.toVariantMap();
    QVERIFY(!viewModel.contains(QStringLiteral("diagnostic")));
    QCOMPARE(viewModel.value(QStringLiteral("kind")).toString(), QStringLiteral("engine"));
    QCOMPARE(viewModel.value(QStringLiteral("error")).toString(),
             QStringLiteral("Something went wrong on our side."));
    QCOMPARE(viewModel.value(QStringLiteral("reference")).toString(), QStringLiteral("SH-GENERR"));
    QCOMPARE(viewModel.value(QStringLiteral("statusCode")).toInt(), 500);
    QCOMPARE(viewModel.value(QStringLiteral("failure")).toString(), QStringLiteral("NO_HOST_AVAILABLE"));
}

void TstErrorMap::errorScreen_rendersReasonAndMonoReference()
{
    const QString qmlPath = guiDir() + QStringLiteral("/ErrorScreen.qml");
    QVERIFY2(QFile::exists(qmlPath), qPrintable(QStringLiteral("missing ") + qmlPath));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    QScopedPointer<QObject> root(component.create());
    if (!root) {
        QFAIL(qPrintable(component.errorString()));
    }

    // The facade is a QVariantMap-shaped stand-in here; the real one is SeatHubClient,
    // but ErrorScreen only ever reads `failure.error` and `reference` (D-35).
    const QString reason = QStringLiteral("Can't reach the rig right now.");
    const QString reference = QStringLiteral("SH-CONREF");
    QVariantMap failureMap;
    failureMap.insert(QStringLiteral("kind"), QStringLiteral("engine"));
    failureMap.insert(QStringLiteral("error"), reason);
    failureMap.insert(QStringLiteral("reference"), reference);
    QVariantMap clientStub;
    clientStub.insert(QStringLiteral("failure"), failureMap);
    clientStub.insert(QStringLiteral("reference"), reference);
    QVERIFY(root->setProperty("client", clientStub));

    QObject* reasonItem = nullptr;
    QObject* referenceItem = nullptr;
    const auto items = textItems(root.data());
    for (QObject* item : items) {
        const QString value = item->property("text").toString();
        if (value == reason && !reasonItem) {
            reasonItem = item;
        }
        else if (value == reference && !referenceItem) {
            referenceItem = item;
        }
    }

    QVERIFY2(reasonItem, "ErrorScreen must render the failure reason inline (ADR-0008, screens.md error state)");
    QVERIFY2(referenceItem, "ErrorScreen must render the ADR-0008 reference code");

    const QFont referenceFont = referenceItem->property("font").value<QFont>();
    QCOMPARE(referenceFont.family(), QString::fromLatin1(kMonoFamily));

    const QFont reasonFont = reasonItem->property("font").value<QFont>();
    QVERIFY2(reasonFont.family() != QString::fromLatin1(kMonoFamily),
             "the reason is prose and must not be set in the mono face");

    // Every failure carries a retry (ADR-0008 requires reason + retry + reference).
    bool sawRetryAction = false;
    for (QObject* item : items) {
        if (item->property("text").toString().contains(QStringLiteral("Try again"))) {
            sawRetryAction = true;
            break;
        }
    }
    QVERIFY2(sawRetryAction, "the error screen must offer a retry action");
}

QTEST_MAIN(TstErrorMap)

#include "tst_error_map.moc"

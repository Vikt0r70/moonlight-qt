/*****************************************************************************
 * SeatHub fork — unit tests for the error model (D-51, ADR-0008).
 *
 * These live in their own top-level `tests/` project because upstream moonlight-qt
 * ships no test tree at all; the fork adds one. They link the bridge's own
 * translation unit (`app/seathub/error_map.cpp`) directly rather than the whole
 * application, so a mapping regression fails here in seconds instead of requiring a
 * full streaming build.
 *
 * Behaviour covered (Plan 03-02 Task 2, revised for audit F9):
 *   1. connection-refused stage    -> the copy.md generic sentence, NO reference
 *   2. decoder stage               -> the same, and no invented decoder copy either
 *   3. displayLaunchError          -> generic copy; the engine's own words never reach
 *                                     the customer-facing field, only `diagnostic`
 *   4. unmapped stage              -> the D-51 generic fallback
 *   5. the client never invents an ADR-0008 reference code
 *   6. ErrorScreen.qml renders the reason and the reference code in mono type
 *   7. `diagnostic` never crosses into the view model handed to QML
 *
 * The audit (F9) found five invented strings and two invented codes here - including
 * `SH-DECUNAV`, seven characters where ADR-0008 allows `SH-` + six. copy.md is the only place
 * user-facing copy may come from, and ADR-0008 is explicit that a reference the client makes up
 * "resolves to nothing", which is worse than showing none. So a stage failure now carries the
 * generic deck sentence and an empty reference, and these tests assert exactly that.
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

    void stageFailure_neverInventsCopyOrReference();
    void stageFailure_decoderStageIsNoException();
    void launchError_neverExposesEngineText();
    void stageFailure_unmappedFallsBackToGeneric();
    void diagnostic_neverReachesTheViewModel();
    void apiFailure_passesTheServerReferenceThrough();
    void ownSessionRefusal_showsTheServersSentenceVerbatim();
    void errorScreen_rendersReasonAndMonoReference();
    void mapEndBlame();
    void searchableReference_isTheFormTheConsoleSearchResolves();
    void everyFailureKindCarriesOnlyASearchableReference();
    void errorScreen_neverDrawsAReferenceThatIsNotOne();

    // T026 / ADR-0096: the presentation key is derived from observed evidence only.
    void endPresentation_isConnectionLostOnlyForAStartedStreamWithANativeFault_data();
    void endPresentation_isConnectionLostOnlyForAStartedStreamWithANativeFault();
    void endPresentation_isCancelledOnlyWhenNothingStartedAndNothingFailed();
    void endPresentation_keepsAnActualFailedStartUnknown();
    void nativeMinus102MatchesServerUnknown();

    // T062 / ADR-0104: the firewall sentence needs a conclusive failed port test.
    void engineStageMessage_firewallSentenceOnlyForAConclusiveFailure();
    void mapPortTestFailure_honoursItsArgument();
};

void TstErrorMap::initTestCase()
{
    // The error model is a plain Qt Core translation unit; assert that up front so a
    // later accidental dependency on the streaming engine fails loudly here.
    QVERIFY(!guiDir().isEmpty());
}

void TstErrorMap::stageFailure_neverInventsCopyOrReference()
{
    const SeatHubFailure failure = mapStageFailure(QStringLiteral("connection_refused"), 0, QString());

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Something went wrong on our side."));
    QVERIFY2(failure.reference.isEmpty(),
             "ADR-0008: a reference the client invents resolves to nothing, which is worse than none");
    QCOMPARE(failure.statusCode, 0);
    QVERIFY2(!failure.error.contains(QStringLiteral("SH-"), Qt::CaseInsensitive),
             "the reference is rendered separately; embedding it in the reason double-prints it");
    QVERIFY2(failure.diagnostic.contains(QStringLiteral("connection_refused")),
             "the stage detail is kept for support even though it never reaches the customer");
}

void TstErrorMap::stageFailure_decoderStageIsNoException()
{
    // A decoder failure used to get its own invented sentence and a seven-character code. It is
    // a stage failure like any other: generic copy, no reference, engine detail only in the
    // diagnostic. Anything more needs a copy.md entry and a control-plane reference first.
    const SeatHubFailure failure = mapStageFailure(QStringLiteral("decoder_unavailable"), 0, QString());

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Something went wrong on our side."));
    QVERIFY(failure.reference.isEmpty());
    QVERIFY2(!failure.error.contains(QStringLiteral("decoder"), Qt::CaseInsensitive),
             "no engine vocabulary reaches the customer-facing sentence");
    QVERIFY(failure.diagnostic.contains(QStringLiteral("decoder_unavailable")));
}

void TstErrorMap::launchError_neverExposesEngineText()
{
    const QString engineWords = QStringLiteral("Some raw Moonlight text");
    const SeatHubFailure failure = mapLaunchError(engineWords);

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Engine));
    QCOMPARE(failure.error, QStringLiteral("Something went wrong on our side."));
    QVERIFY(failure.reference.isEmpty());

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
    QVERIFY(failure.reference.isEmpty());
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
    QVERIFY(viewModel.value(QStringLiteral("reference")).toString().isEmpty());
    QCOMPARE(viewModel.value(QStringLiteral("statusCode")).toInt(), 500);
    QCOMPARE(viewModel.value(QStringLiteral("failure")).toString(), QStringLiteral("NO_HOST_AVAILABLE"));
}

void TstErrorMap::apiFailure_passesTheServerReferenceThrough()
{
    // The only reference codes that exist are the control plane's (ADR-0008). When the
    // control plane supplies one it is carried through untouched - ErrorScreen renders it in
    // mono - and when it supplies none the field stays empty rather than being filled in here.
    const SeatHubFailure failure = SeatHubFailure::api(
        409, QStringLiteral("An account already exists for this phone number."),
        QStringLiteral("SH-4F7KQ2"), QStringLiteral("PHONE_ALREADY_REGISTERED"));

    QCOMPARE(static_cast<int>(failure.kind), static_cast<int>(FailureKind::Api));
    QCOMPARE(failure.statusCode, 409);
    QCOMPARE(failure.reference, QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(failure.failure, QStringLiteral("PHONE_ALREADY_REGISTERED"));
    QCOMPARE(failure.error, QStringLiteral("An account already exists for this phone number."));

    const SeatHubFailure noCode =
        SeatHubFailure::api(500, QStringLiteral("Something went wrong on our side."), QString());
    QVERIFY2(noCode.reference.isEmpty(),
             "the client must not manufacture a reference the control plane never issued");
}

void TstErrorMap::ownSessionRefusal_showsTheServersSentenceVerbatim()
{
    // LK01: when the server still holds the customer's previous session, the refusal Play meets is
    // `USER_HAS_NONTERMINAL_SESSION`, and the customer reads the SERVER's sentence for it - the
    // existing mapping carries it through untouched, with no client-side string standing in for it
    // and no reference the control plane did not issue.
    const QString sentence = QStringLiteral("The server's own sentence about the earlier session.");
    const SeatHubFailure failure = SeatHubFailure::api(
        409, sentence, QStringLiteral("SH-4F7KQ2"), QStringLiteral("USER_HAS_NONTERMINAL_SESSION"));

    QCOMPARE(failure.error, sentence);
    QCOMPARE(failure.failure, QStringLiteral("USER_HAS_NONTERMINAL_SESSION"));
    QCOMPARE(failure.reference, QStringLiteral("SH-4F7KQ2"));

    const QVariantMap viewModel = failure.toVariantMap();
    QCOMPARE(viewModel.value(QStringLiteral("error")).toString(), sentence);
    QCOMPARE(viewModel.value(QStringLiteral("reference")).toString(), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(viewModel.value(QStringLiteral("failure")).toString(),
             QStringLiteral("USER_HAS_NONTERMINAL_SESSION"));
    QVERIFY2(viewModel.value(QStringLiteral("error")).toString() != SeatHubFailure::generic().error,
             "a refusal the server worded is never replaced by the generic sentence");
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
    // but ErrorScreen only ever reads `failure.error` and `reference` (D-35). The code is
    // copy.md's own example, which is shape-valid under ADR-0008 (`SH-` + six characters,
    // no I/L/O/U) - test data, never shipped copy.
    const QString reason = QStringLiteral("Something went wrong on our side.");
    const QString reference = QStringLiteral("SH-4F7KQ2");
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

void TstErrorMap::mapEndBlame()
{
    // 06.1's ADR item 4 / J-22, copy.md §Session end reasons' CONNECT_FAILED row: each of the
    // three blames gives its own sentence, with the server's reference set; an empty blame keeps
    // today's plain sentence and shows no reference; a reference never appears without a server
    // value to have carried it.
    const SeatHubFailure rig = ::mapEndBlame(QStringLiteral("rig"), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(rig.error,
             QStringLiteral("The rig had a problem, so the stream didn't start. You were not charged."));
    QCOMPARE(rig.reference, QStringLiteral("SH-4F7KQ2"));

    const SeatHubFailure connection =
        ::mapEndBlame(QStringLiteral("connection"), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(connection.error,
             QStringLiteral("Your connection couldn't reach the rig, so the stream didn't start. "
                            "You were not charged."));
    QCOMPARE(connection.reference, QStringLiteral("SH-4F7KQ2"));

    const SeatHubFailure us = ::mapEndBlame(QStringLiteral("us"), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(us.error,
             QStringLiteral("Something went wrong on our side, so the stream didn't start. You "
                            "were not charged."));
    QCOMPARE(us.reference, QStringLiteral("SH-4F7KQ2"));

    // An empty blame (an older server, or a non-failing end): the plain sentence, no reference -
    // even when a reference somehow arrived alongside it, ADR-0008 leaves nothing for it to name.
    const SeatHubFailure noBlame = ::mapEndBlame(QString(), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(noBlame.error, QStringLiteral("The stream didn't start. You were not charged."));
    QVERIFY2(noBlame.reference.isEmpty(),
             "an empty blame must show no reference, whatever the argument carried");

    const SeatHubFailure neither = ::mapEndBlame(QString(), QString());
    QCOMPARE(neither.error, QStringLiteral("The stream didn't start. You were not charged."));
    QVERIFY(neither.reference.isEmpty());
}

// SC13 "searchable plain-language errors" (screens.md §23 Error, the State coverage matrix's Error row,
// ADR-0008 items 3 and 4). The operator finds a customer's failure by typing the code the customer
// read out, so the code on screen must be exactly the form the console search resolves: `SH-` and six
// characters of 0-9 A-Z without I, L, O and U, upper-case, no whitespace around it. The client cleans
// up a code it was given; it never builds one, and anything that is not a reference is never drawn
// as one (CUST-10: no enum or code ever appears bare).
void TstErrorMap::searchableReference_isTheFormTheConsoleSearchResolves()
{
    // Already in form: unchanged. The deck's two sample codes, and the alphabet's edges.
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("SH-4F7KQ2")), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("SH-9K2XQ1")), QStringLiteral("SH-9K2XQ1"));
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("SH-000000")), QStringLiteral("SH-000000"));
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("SH-ZZZZZZ")), QStringLiteral("SH-ZZZZZZ"));

    // Padding and case are corrected, so what is read out is what is searched for.
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("  SH-4F7KQ2\n")), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(SeatHubFailure::searchableReference(QStringLiteral("sh-4f7kq2")), QStringLiteral("SH-4F7KQ2"));

    // Not a reference, so not shown as one: a truncated or over-long code, a letter the alphabet
    // leaves out (I, L, O, U), a code inside a sentence, an enum, a different prefix, nothing at all.
    const QStringList notReferences = {
        QStringLiteral("SH-4F7KQ"),   QStringLiteral("SH-4F7KQ22"), QStringLiteral("SH-4F7KQI"),
        QStringLiteral("SH-4F7KQL"),  QStringLiteral("SH-4F7KQO"),  QStringLiteral("SH-4F7KQU"),
        QStringLiteral("SH-4F 7KQ2"), QStringLiteral("SH4F7KQ2"),   QStringLiteral("XX-4F7KQ2"),
        QStringLiteral("Reference SH-4F7KQ2"), QStringLiteral("NO_HOST_AVAILABLE"), QString(),
        QStringLiteral("   "),
    };
    for (const QString& candidate : notReferences) {
        QVERIFY2(SeatHubFailure::searchableReference(candidate).isEmpty(), qPrintable(candidate));
    }
}

void TstErrorMap::everyFailureKindCarriesOnlyASearchableReference()
{
    // The control plane's own refusal: a padded, lower-case code is put in form; a bare enum in the
    // reference slot is dropped, and the server's sentence is still shown as it came.
    const SeatHubFailure padded = SeatHubFailure::api(
        402, QStringLiteral("You need at least 15 minutes of credit to start a session."),
        QStringLiteral(" sh-4f7kq2 "));
    QCOMPARE(padded.reference, QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(padded.error, QStringLiteral("You need at least 15 minutes of credit to start a session."));
    QCOMPARE(padded.toVariantMap().value(QStringLiteral("reference")).toString(),
             QStringLiteral("SH-4F7KQ2"));

    const SeatHubFailure bareEnum = SeatHubFailure::api(
        409, QStringLiteral("You already have a session. Wait for it to finish."),
        QStringLiteral("SESSION_ACTIVE"), QStringLiteral("SESSION_ACTIVE"));
    QVERIFY2(bareEnum.reference.isEmpty(), "an enum is never drawn as the support reference");
    QCOMPARE(bareEnum.error, QStringLiteral("You already have a session. Wait for it to finish."));
    QCOMPARE(bareEnum.failure, QStringLiteral("SESSION_ACTIVE"));

    // The engine-side constructor (the update window's failure) holds to the same rule.
    QCOMPARE(SeatHubFailure::engine(QStringLiteral("The installer couldn't be started."),
                                    QStringLiteral("sh-9k2xq1")).reference,
             QStringLiteral("SH-9K2XQ1"));
    QVERIFY(SeatHubFailure::engine(QStringLiteral("The installer couldn't be started."),
                                   QStringLiteral("SUPPORT-REF")).reference.isEmpty());

    // The terminal-session blame path: the server's `end_reference` is put in form too, and a value
    // that is not a reference leaves the blame sentence with no code rather than a wrong one.
    QCOMPARE(::mapEndBlame(QStringLiteral("rig"), QStringLiteral(" sh-4f7kq2")).reference,
             QStringLiteral("SH-4F7KQ2"));
    QVERIFY(::mapEndBlame(QStringLiteral("us"), QStringLiteral("whatever")).reference.isEmpty());
    QVERIFY(::mapEndBlame(QStringLiteral("connection"), QStringLiteral("whatever")).error.contains(
        QStringLiteral("You were not charged.")));

    // Local failures have no code to clean up: the generic sentence stays, with none, and the deck's
    // sample code is never taken for this incident's.
    const SeatHubFailure generic = SeatHubFailure::generic();
    QCOMPARE(generic.error, QStringLiteral("Something went wrong on our side."));
    QVERIFY(generic.reference.isEmpty());
    QVERIFY(SeatHubFailure::network(SeatHubFailure::offlineSentence()).reference.isEmpty());
    QVERIFY(mapStageFailure(QStringLiteral("RTSP handshake"), -1, QString()).reference.isEmpty());
}

void TstErrorMap::errorScreen_neverDrawsAReferenceThatIsNotOne()
{
    // End to end through the view model ErrorScreen reads (D-35): a failure the control plane sent
    // with a code in the wrong slot reaches the screen with the sentence and with no reference row.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    const SeatHubFailure failure = SeatHubFailure::api(
        500, QStringLiteral("Something went wrong on our side."), QStringLiteral("INTERNAL_ERROR"));
    QVariantMap clientStub;
    clientStub.insert(QStringLiteral("failure"), failure.toVariantMap());
    clientStub.insert(QStringLiteral("reference"), failure.reference);

    QQmlComponent component(&engine, QUrl::fromLocalFile(guiDir() + QStringLiteral("/ErrorScreen.qml")));
    QScopedPointer<QObject> screen(component.createWithInitialProperties(
        { { QStringLiteral("client"), clientStub } }));
    QVERIFY2(screen, qPrintable(component.errorString()));

    bool sawSentence = false;
    for (QObject* item : textItems(screen.data())) {
        const QString value = item->property("text").toString();
        QVERIFY2(value != QStringLiteral("INTERNAL_ERROR"), "an enum is never drawn as a reference");
        sawSentence = sawSentence || value == QStringLiteral("Something went wrong on our side.");
    }
    QVERIFY(sawSentence);
    QObject* referenceRow = screen->findChild<QObject*>(QStringLiteral("referenceRow"));
    QVERIFY(referenceRow);
    QVERIFY2(!referenceRow->property("visible").toBool(),
             "no reference row at all rather than a label with nothing after it");
}

void TstErrorMap::endPresentation_isConnectionLostOnlyForAStartedStreamWithANativeFault_data()
{
    QTest::addColumn<int>("code");
    QTest::addColumn<QString>("expected");
    QTest::newRow("no video traffic (-100)") << -100 << "connection_lost";
    QTest::newRow("no video frame (-101)") << -101 << "connection_lost";
    QTest::newRow("unexpected early termination (-102)") << -102 << "unknown";
    QTest::newRow("graceful (0)") << 0 << "unknown";
    QTest::newRow("protected content (-103)") << -103 << "unknown";
    QTest::newRow("frame conversion (-104)") << -104 << "unknown";
    QTest::newRow("some other code") << 5 << "unknown";
}

void TstErrorMap::endPresentation_isConnectionLostOnlyForAStartedStreamWithANativeFault()
{
    QFETCH(int, code);
    QFETCH(QString, expected);
    EndEvidence evidence;
    evidence.streamStarted = true;
    evidence.engineFailed = false;
    evidence.nativeFault = true;
    evidence.nativeFaultCode = code;
    QCOMPARE(deriveEndPresentation(evidence), expected);

    // The same code without the stream having started is not a lost connection.
    evidence.streamStarted = false;
    evidence.engineFailed = true;
    QCOMPARE(deriveEndPresentation(evidence), QStringLiteral("unknown"));

    // A started stream with no native fault at all (a generic quit) stays unknown.
    EndEvidence quiet;
    quiet.streamStarted = true;
    QCOMPARE(deriveEndPresentation(quiet), QStringLiteral("unknown"));
}

void TstErrorMap::endPresentation_isCancelledOnlyWhenNothingStartedAndNothingFailed()
{
    EndEvidence evidence;
    evidence.streamStarted = false;
    evidence.engineFailed = false;
    QCOMPARE(deriveEndPresentation(evidence), QStringLiteral("cancelled"));
}

void TstErrorMap::endPresentation_keepsAnActualFailedStartUnknown()
{
    EndEvidence evidence;
    evidence.streamStarted = false;
    evidence.engineFailed = true;
    QCOMPARE(deriveEndPresentation(evidence), QStringLiteral("unknown"));
}

// T075 / F5: the shared counterexample of the server's
// `test_absent_ambiguous_or_generic_evidence_is_unknown_never_manual` (seathub-control
// tests/test_end_outcomes.py): a completed CUSTOMER_ENDED session, stage streaming, engine_error
// -102 and no independent supported loss evidence is `unknown`. ADR-0096 and timeline.py limit
// native loss to {-100, -101}; -102 alone must never become `connection_lost` in the client either.
void TstErrorMap::nativeMinus102MatchesServerUnknown()
{
    EndEvidence evidence;
    evidence.streamStarted = true;
    evidence.engineFailed = false;
    evidence.nativeFault = true;
    evidence.nativeFaultCode = -102;
    QCOMPARE(deriveEndPresentation(evidence), QStringLiteral("unknown"));

    // The approved native set is untouched: -100 and -101 still lose the connection.
    for (const int supported : { -100, -101 }) {
        evidence.nativeFaultCode = supported;
        QCOMPARE(deriveEndPresentation(evidence), QStringLiteral("connection_lost"));
    }
}

void TstErrorMap::engineStageMessage_firewallSentenceOnlyForAConclusiveFailure()
{
    const QString bare = QStringLiteral("Starting RTSP handshake failed: Error 10060");
    const QString withSentence =
        bare + QStringLiteral("\n\nCheck your firewall and port forwarding rules for port(s): "
                              "TCP 48010");

    PortTestResult failed;
    failed.outcome = PortTestResult::Outcome::Failed;
    failed.failedMask = 0x10;
    QCOMPARE(engineStageMessage(QStringLiteral("RTSP handshake"), 10060,
                                QStringLiteral("TCP 48010"), failed),
             withSentence);

    // A clean, an inconclusive and a not-run test must not blame the customer's firewall.
    PortTestResult clean;
    clean.outcome = PortTestResult::Outcome::Clean;
    PortTestResult inconclusive;
    inconclusive.outcome = PortTestResult::Outcome::Inconclusive;
    const PortTestResult notRun;
    for (const PortTestResult& result : { clean, inconclusive, notRun }) {
        QCOMPARE(engineStageMessage(QStringLiteral("RTSP handshake"), 10060,
                                    QStringLiteral("TCP 48010"), result),
                 bare);
    }

    // A failed test with no failing ports names no ports, so there is nothing to append.
    QCOMPARE(engineStageMessage(QStringLiteral("RTSP handshake"), 10060, QString(), failed), bare);
}

void TstErrorMap::mapPortTestFailure_honoursItsArgument()
{
    // The mapped failure stays the generic copy: this task adds no new string. The classified
    // result travels only in the diagnostic, which never reaches the view model.
    const SeatHubFailure failed = mapPortTestFailure(0x10);
    const SeatHubFailure clean = mapPortTestFailure(0);
    const SeatHubFailure inconclusive = mapPortTestFailure(static_cast<int>(0xFFFFFFFFu));
    QCOMPARE(failed.error, SeatHubFailure::generic().error);
    QCOMPARE(clean.error, SeatHubFailure::generic().error);
    QCOMPARE(inconclusive.error, SeatHubFailure::generic().error);
    QVERIFY(failed.diagnostic.contains(QStringLiteral("failed")));
    QVERIFY(clean.diagnostic.contains(QStringLiteral("clean")));
    QVERIFY(inconclusive.diagnostic.contains(QStringLiteral("inconclusive")));
    QVERIFY(failed.diagnostic != clean.diagnostic);
    QVERIFY(clean.diagnostic != inconclusive.diagnostic);
}

QTEST_MAIN(TstErrorMap)

#include "tst_error_map.moc"

/*****************************************************************************
 * SeatHub fork - the attempt vocabulary (ADR-0072 item 1, Plan 09 Task 1).
 *
 * The 11 `attempt_step` tokens and their closed `failure_class` sets are frozen in
 * `docs/decisions/ADR-0072-play-diagnostics-the-attempt-vocabulary-and-stream-quality-samples.md`
 * and mirrored by `docs/spec/client.md` "Attempt steps". `scripts/check_attempt_vocabulary.py`
 * proves the list is identical in the spec, the contract, the server and this fork's header;
 * this suite proves the mappers behave: every answer is a token from a closed set, and no answer
 * can ever be derived from diagnostic text (the redaction rule, ADR-0072 item 2).
 *
 * The classification the pairing handshake applies with these mappers is asserted end to end in
 * `tst_pairing::aWrongPinFailureCarriesStepAndClassButNeverTheDiagnostic`.
 *
 * Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
 *   cd tests && qmake tst_attempt_vocab.pro && jom && tst_attempt_vocab.exe -o tst_attempt_vocab-out.txt,txt
 *****************************************************************************/

#include <QtTest>

#include <QRegularExpression>
#include <QStringList>

#include "seathub/attempt_vocab.h"

namespace {

// `docs/spec/client.md` "Attempt steps", row order - the same list the vocabulary script holds
// as its expectation. Duplicated here so the suite fails on a drift in either direction.
const char* const kExpectedSteps[] = {
    "allocate",
    "rig_wait",
    "pair_authorize",
    "pair_server_info",
    "pair_handshake",
    "engine_prepare",
    "engine_connect",
    "first_frame",
    "stream",
    "reconnect",
    "teardown",
};

const QRegularExpression kTokenGrammar(QStringLiteral("^[a-z][a-z0-9_]{0,31}$"));

// The union of every closed class set the pairing rows can produce (`pair_server_info` and
// `pair_handshake`). Anything outside it would be a class no Sentry query can group on.
const QSet<QString> kPairingClasses = {
    QStringLiteral("no_address"),
    QStringLiteral("net_refused"),
    QStringLiteral("net_closed"),
    QStringLiteral("net_host_not_found"),
    QStringLiteral("net_timeout"),
    QStringLiteral("net_tls"),
    QStringLiteral("net_proxy"),
    QStringLiteral("net_other"),
    QStringLiteral("http_4xx"),
    QStringLiteral("http_5xx"),
    QStringLiteral("pin_rejected"),
    QStringLiteral("in_progress"),
    QStringLiteral("failed"),
    QStringLiteral("crypto_init"),
    QStringLiteral("other"),
};

const QSet<QString> kEngineStageClasses = {
    QStringLiteral("platform_init"), QStringLiteral("name_resolution"),
    QStringLiteral("audio_init"),     QStringLiteral("rtsp_handshake"),
    QStringLiteral("control_init"),   QStringLiteral("video_init"),
    QStringLiteral("input_init"),     QStringLiteral("control_start"),
    QStringLiteral("video_start"),    QStringLiteral("audio_start"),
    QStringLiteral("input_start"),    QStringLiteral("launch_error"),
};

} // namespace

class TstAttemptVocab : public QObject
{
    Q_OBJECT

private slots:
    void stepTokensAreTheElevenInOrder()
    {
        const QStringList tokens = attemptStepTokens();

        QStringList expected;
        for (const char* const step : kExpectedSteps) {
            expected.append(QString::fromLatin1(step));
        }
        QCOMPARE(tokens, expected);
        QCOMPARE(tokens.size(), 11);

        for (const QString& token : tokens) {
            QVERIFY2(kTokenGrammar.match(token).hasMatch(),
                     qPrintable(QStringLiteral("token is not well formed: %1").arg(token)));
        }

        // The client coerces anything it does not know to `other` (ADR-0072 item 1).
        QCOMPARE(coerceStep(QStringLiteral("nonsense")), QStringLiteral("other"));
        QCOMPARE(coerceStep(QString()), QStringLiteral("other"));
        QCOMPARE(coerceStep(QStringLiteral("Pair_Handshake")), QStringLiteral("other"));
    }

    void coerceStepKeepsTheTokensItKnows()
    {
        for (const char* const step : kExpectedSteps) {
            const QString token = QString::fromLatin1(step);
            QCOMPARE(coerceStep(token), token);
        }
    }

    void pairStateMapsToClosedClasses()
    {
        // `NvPairingManager::PairState`'s own order (`app/backend/nvpairingmanager.h`):
        // PAIRED 0, PIN_WRONG 1, FAILED 2, ALREADY_IN_PROGRESS 3.
        QCOMPARE(classForPairState(1), QStringLiteral("pin_rejected"));
        QCOMPARE(classForPairState(3), QStringLiteral("in_progress"));
        QCOMPARE(classForPairState(2), QStringLiteral("failed"));

        // Anything that is not one of the three failures is `other` - never a sentence.
        QCOMPARE(classForPairState(0), QStringLiteral("other"));
        QCOMPARE(classForPairState(42), QStringLiteral("other"));
        QVERIFY(kPairingClasses.contains(classForPairState(1)));
        QVERIFY(kPairingClasses.contains(classForPairState(3)));
        QVERIFY(kPairingClasses.contains(classForPairState(2)));
    }

    void networkErrorMapsToTheClosedNetSet()
    {
        QCOMPARE(classForNetworkError(1), QStringLiteral("net_refused"));
        QCOMPARE(classForNetworkError(2), QStringLiteral("net_closed"));
        QCOMPARE(classForNetworkError(3), QStringLiteral("net_host_not_found"));
        QCOMPARE(classForNetworkError(4), QStringLiteral("net_timeout"));
        QCOMPARE(classForNetworkError(6), QStringLiteral("net_tls"));
        for (int proxy = 101; proxy <= 105; ++proxy) {
            QCOMPARE(classForNetworkError(proxy), QStringLiteral("net_proxy"));
        }

        // The documented default arm: every other integer, including 0 (NoError) and the
        // unlisted values such as 5, lands on `net_other`.
        for (int other : { 0, 5, 7, 99, 106, 199, 999 }) {
            QCOMPARE(classForNetworkError(other), QStringLiteral("net_other"));
        }
    }

    void httpStatusMapsToBuckets()
    {
        QCOMPARE(classForHttpStatus(400), QStringLiteral("http_4xx"));
        QCOMPARE(classForHttpStatus(404), QStringLiteral("http_4xx"));
        QCOMPARE(classForHttpStatus(499), QStringLiteral("http_4xx"));
        QCOMPARE(classForHttpStatus(500), QStringLiteral("http_5xx"));
        QCOMPARE(classForHttpStatus(503), QStringLiteral("http_5xx"));
        QCOMPARE(classForHttpStatus(599), QStringLiteral("http_5xx"));

        // Outside both buckets the family's default applies: `pair_server_info`'s closed set
        // carries no `other`, so a redirect, a 2xx or a nonsense status is `net_other`.
        for (int other : { 0, 200, 302, 600, -1 }) {
            QCOMPARE(classForHttpStatus(other), QStringLiteral("net_other"));
        }
    }

    void engineStageMapsToTheElevenSlugsPlusLaunchError()
    {
        const char* const expected[] = { "platform_init", "name_resolution", "audio_init",
                                         "rtsp_handshake", "control_init",  "video_init",
                                         "input_init",     "control_start", "video_start",
                                         "audio_start",    "input_start" };
        for (int index = 1; index <= 11; ++index) {
            QCOMPARE(classForEngineStage(index), QString::fromLatin1(expected[index - 1]));
            QVERIFY(kEngineStageClasses.contains(classForEngineStage(index)));
        }

        // Stage `none` (0), an index past the last stage and a negative one all mean "no stage
        // had started", which is exactly what `launch_error` names.
        for (int other : { 0, 12, -1, 99 }) {
            QCOMPARE(classForEngineStage(other), QStringLiteral("launch_error"));
        }
    }

    void everyAnswerIsATokenAndStaysInThePairingClasses()
    {
        // The redaction rule, asserted structurally: no mapper can return anything that is not
        // a lowercase token, and the pairing mappers stay inside their closed sets. A mapper
        // that ever answered with a diagnostic sentence would fail here first.
        const QList<int> netCodes = { 0,   1,   2,   3,   4,   5,   6,   99,
                                      101, 102, 103, 104, 105, 999 };
        const QList<int> httpCodes = { 0, 200, 302, 400, 404, 499, 500, 503, 599, 600 };
        const QList<int> pairStates = { 0, 1, 2, 3, 4 };

        for (int code : netCodes) {
            const QString cls = classForNetworkError(code);
            QVERIFY2(kTokenGrammar.match(cls).hasMatch(), qPrintable(cls));
            QVERIFY(kPairingClasses.contains(cls));
        }
        for (int status : httpCodes) {
            const QString cls = classForHttpStatus(status);
            QVERIFY2(kTokenGrammar.match(cls).hasMatch(), qPrintable(cls));
            QVERIFY(kPairingClasses.contains(cls));
        }
        for (int state : pairStates) {
            const QString cls = classForPairState(state);
            QVERIFY2(kTokenGrammar.match(cls).hasMatch(), qPrintable(cls));
            QVERIFY(kPairingClasses.contains(cls));
        }
        for (int stage = -1; stage <= 13; ++stage) {
            const QString cls = classForEngineStage(stage);
            QVERIFY2(kTokenGrammar.match(cls).hasMatch(), qPrintable(cls));
            QVERIFY(kEngineStageClasses.contains(cls));
        }
    }
};

QTEST_MAIN(TstAttemptVocab)

#include "tst_attempt_vocab.moc"

/*****************************************************************************
 * SeatHub fork - QML shell tests (audit F1/F2/F4/E9/E10, Plan 03-04's technique).
 *
 * QML has no compiler here: a syntax error, a missing property or a binding that
 * throws is invisible to the C++ build and only shows up when the client starts.
 * This suite loads every screen in `app/gui` that the SeatHub shell uses, and then
 * asserts the states the UI audit found missing actually render:
 *
 *   1. every shell screen instantiates (a QML error fails the test)
 *   2. `Metrics` really parses the generated tokens (F15)
 *   3. Home renders each of its four states and the end-reason sentence (F1, E10)
 *   4. Sign in shows a rejected phone number's error and the resend countdown (F2, F6)
 *   5. the forced-update modal keeps `Update` tabbable and has no dismissal path (F4)
 *   6. the settings dropdown elides long option names in its popup too (E9)
 *   7. the disabled action label stays legible (F5)
 *
 * What this suite cannot do: it cannot take a screenshot, cannot see a pixel, and
 * cannot put a real window on a screen. Loading and property inspection are the whole
 * of the evidence, and any claim about how the UI *looks* is out of scope here.
 *****************************************************************************/

#include <QtTest>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QUrl>
#include <QVariantMap>

#include <cmath>

#include "seathub/agent_config.h"

namespace {

const char* kSansFamily = "Inter";
const char* kMonoFamily = "Geist Mono";

QString guiDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 8; ++depth) {
        const QString candidate = dir.filePath(QStringLiteral("app/gui"));
        if (QFile::exists(candidate + QStringLiteral("/HomeScreen.qml"))) {
            return QDir::cleanPath(candidate);
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

// The bundled country list, read from the file the app compiles in as a resource.
QVariantList bundledCountries()
{
    QFile file(guiDir() + QStringLiteral("/../seathub/countries.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return QVariantList();
    }
    return QJsonDocument::fromJson(file.readAll()).array().toVariantList();
}

QString readSource(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void registerTokenSingletons(QQmlEngine* engine)
{
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");
}

QObject* instantiate(QQmlEngine* engine, const QString& fileName, QString* error)
{
    const QString path = guiDir() + QLatin1Char('/') + fileName;
    QQmlComponent component(engine, QUrl::fromLocalFile(path));
    QObject* root = component.create();
    if (!root && error) {
        *error = component.errorString();
    }
    return root;
}

QList<QObject*> textItems(QObject* root)
{
    QList<QObject*> items;
    for (QObject* child : root->findChildren<QObject*>()) {
        const QMetaObject* metaObject = child->metaObject();
        if (metaObject->indexOfProperty("text") >= 0 && metaObject->indexOfProperty("font") >= 0) {
            items.append(child);
        }
    }
    return items;
}

// The rendered text of every Text in the tree, in no particular order.
QStringList renderedTexts(QObject* root)
{
    QStringList texts;
    for (QObject* item : textItems(root)) {
        const QString value = item->property("text").toString();
        if (!value.isEmpty()) {
            texts.append(value);
        }
    }
    return texts;
}

// `visible` on an item whose parent is hidden is not visible. QQuickItem::isVisible()
// accounts for that, but only for items; this walks the property chain instead so the
// helper works for any text-bearing object.
bool effectivelyVisible(QObject* object)
{
    for (QObject* current = object; current; current = current->parent()) {
        const QMetaObject* metaObject = current->metaObject();
        if (metaObject->indexOfProperty("visible") >= 0
            && !current->property("visible").toBool()) {
            return false;
        }
    }
    return true;
}

QObject* findTextItem(QObject* root, const QString& text)
{
    for (QObject* item : textItems(root)) {
        if (item->property("text").toString() == text) {
            return item;
        }
    }
    return nullptr;
}

QObject* findVisibleTextItem(QObject* root, const QString& text)
{
    for (QObject* item : textItems(root)) {
        if (item->property("text").toString() == text && effectivelyVisible(item)) {
            return item;
        }
    }
    return nullptr;
}

double contrastRatio(const QColor& a, const QColor& b)
{
    auto luminance = [](const QColor& c) {
        auto channel = [](double value) {
            value /= 255.0;
            return value <= 0.04045 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green())
               + 0.0722 * channel(c.blue());
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

// A stand-in for SeatHubClient: the properties and invokables the shell screens read
// (D-35 keeps the surface this small on purpose). Signals are emitted by the test so a
// screen's reaction to the facade can be asserted without a control plane.
class FakeShellClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString homeStatus READ homeStatus NOTIFY homeStatusChanged)
    Q_PROPERTY(QString endReasonText READ endReasonText NOTIFY endReasonTextChanged)
    Q_PROPERTY(QString identity READ identity NOTIFY identityChanged)
    Q_PROPERTY(QString reference READ reference NOTIFY failureChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)
    Q_PROPERTY(qint64 balanceMinutes READ balanceMinutes NOTIFY balanceChanged)
    Q_PROPERTY(QString balanceText READ balanceText NOTIFY balanceChanged)
    Q_PROPERTY(bool balanceStale READ balanceStale NOTIFY balanceChanged)
    Q_PROPERTY(QVariantList countries READ countries CONSTANT)
    Q_PROPERTY(QString defaultCountryCode READ defaultCountryCode CONSTANT)
    Q_PROPERTY(bool animationEffects READ animationEffects CONSTANT)
    Q_PROPERTY(bool liveSession READ liveSession NOTIFY liveSessionChanged)
    Q_PROPERTY(int connectStage READ connectStage NOTIFY connectStageChanged)

public:
    int connectStage() const { return m_connectStage; }
    void setConnectStage(int stage)
    {
        m_connectStage = stage;
        emit connectStageChanged();
    }
    bool liveSession() const { return m_liveSession; }
    void setLiveSession(bool live)
    {
        m_liveSession = live;
        emit liveSessionChanged();
    }
    void setIdentity(const QString& identity)
    {
        m_identity = identity;
        emit identityChanged();
    }

    QVariantList countries() const { return bundledCountries(); }
    QString defaultCountryCode() const { return QStringLiteral("JO"); }
    // Off: the field's reveal is instant, so a test reads the end state without waiting on a timer.
    bool animationEffects() const { return false; }

    qint64 balanceMinutes() const { return m_balanceMinutes; }
    QString balanceText() const { return m_balanceText; }
    bool balanceStale() const { return m_balanceStale; }
    /// What `SeatHubClient` exposes: minutes (-1 = never read), the C++-formatted text, and
    /// whether the last read failed. The screens only display these.
    void setBalance(qint64 minutes, const QString& text, bool stale)
    {
        m_balanceMinutes = minutes;
        m_balanceText = text;
        m_balanceStale = stale;
        emit balanceChanged();
    }

    QString homeStatus() const { return m_homeStatus; }
    QString endReasonText() const { return m_endReasonText; }
    QString identity() const { return m_identity; }
    QString reference() const { return m_reference; }
    QVariantMap failure() const { return m_failure; }

    void setHomeStatus(const QString& status)
    {
        m_homeStatus = status;
        emit homeStatusChanged();
    }
    void setEndReasonText(const QString& text)
    {
        m_endReasonText = text;
        emit endReasonTextChanged();
    }
    void setFailure(const QString& message, const QString& reference)
    {
        m_failure.clear();
        m_failure.insert(QStringLiteral("error"), message);
        m_reference = reference;
        emit failureChanged();
    }

    Q_INVOKABLE void start() { ++m_starts; }
    Q_INVOKABLE void openSettings() { ++m_settingsOpens; }
    Q_INVOKABLE bool openTopUp()
    {
        ++m_topUps;
        return true;
    }
    Q_INVOKABLE void signOut() { ++m_signOuts; }
    Q_INVOKABLE void retry() { ++m_retries; }
    Q_INVOKABLE void dismissError() { ++m_dismissals; }
    Q_INVOKABLE void requestOtp(const QString& phone)
    {
        m_lastPhone = phone;
        m_otpRequests.append(phone);
    }
    Q_INVOKABLE void verifyOtp(const QString& phone, const QString& code)
    {
        m_lastPhone = phone;
        m_lastCode = code;
    }
    // A stand-in for the facade's E.164 builder: the dial code in front of the digits, one trunk
    // zero dropped, a leading plus or double zero believed. The real rules are tested in
    // `tst_control_plane`; this only has to give the screen a number to send.
    Q_INVOKABLE QString toE164(const QString& typed, const QString& dial) const
    {
        QString digits = typed;
        digits.remove(QRegularExpression(QStringLiteral("[^0-9+]")));
        if (digits.startsWith(QLatin1String("00"))) {
            digits = QStringLiteral("+") + digits.mid(2);
        }
        if (!digits.startsWith(QLatin1Char('+'))) {
            if (digits.startsWith(QLatin1Char('0'))) {
                digits.remove(0, 1);
            }
            digits = dial + digits;
        }
        return digits.size() >= 9 ? digits : QString();
    }
    Q_INVOKABLE void signInWithPassword(const QString& identifier, const QString& password)
    {
        m_passwordAttempts.append(qMakePair(identifier, password));
    }
    Q_INVOKABLE bool openWebsite(const QString& target)
    {
        m_opened.append(target);
        return true;
    }

    QStringList otpRequests() const { return m_otpRequests; }
    QList<QPair<QString, QString>> passwordAttempts() const { return m_passwordAttempts; }
    QStringList opened() const { return m_opened; }

    void deliverPasswordRejected(const QString& message, const QString& reference)
    {
        emit passwordSignInRejected(message, reference);
    }
    void deliverPasswordAccepted() { emit passwordSignInAccepted(); }

    int topUps() const { return m_topUps; }
    int settingsOpens() const { return m_settingsOpens; }
    int starts() const { return m_starts; }
    int signOuts() const { return m_signOuts; }
    int retries() const { return m_retries; }
    int dismissals() const { return m_dismissals; }

    void deliverOtpRequested(const QString& phone) { emit otpRequested(phone); }
    void deliverOtpRejected(const QString& message, const QString& reference)
    {
        setFailure(message, reference);
        emit otpRejected(message, reference);
    }
    void deliverOtpAccepted() { emit otpAccepted(); }

signals:
    void balanceChanged();
    void connectStageChanged();
    void liveSessionChanged();
    void homeStatusChanged();
    void endReasonTextChanged();
    void identityChanged();
    void failureChanged();
    void otpRequested(const QString& phoneE164);
    void otpRejected(const QString& message, const QString& reference);
    void otpAccepted();
    void passwordSignInRejected(const QString& message, const QString& reference);
    void passwordSignInAccepted();

private:
    QStringList m_otpRequests;
    QList<QPair<QString, QString>> m_passwordAttempts;
    QStringList m_opened;
    qint64 m_balanceMinutes = -1;
    QString m_balanceText;
    bool m_balanceStale = false;
    QString m_homeStatus = QStringLiteral("ready");
    QString m_endReasonText;
    QString m_identity = QStringLiteral("+962 7 0001 0002");
    QString m_reference;
    QVariantMap m_failure;
    QString m_lastPhone;
    QString m_lastCode;
    bool m_liveSession = false;
    int m_connectStage = 0;
    int m_topUps = 0;
    int m_settingsOpens = 0;
    int m_starts = 0;
    int m_signOuts = 0;
    int m_retries = 0;
    int m_dismissals = 0;
};

class FakeUpdates : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap availableUpdate READ availableUpdate NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY changed)
    Q_PROPERTY(bool blockedBySession READ blockedBySession NOTIFY changed)
    Q_PROPERTY(bool readyToInstall READ readyToInstall NOTIFY changed)

public:
    QString state() const { return m_state; }
    QVariantMap availableUpdate() const { return m_availableUpdate; }
    int progress() const { return m_progress; }
    QVariantMap failure() const { return m_failure; }
    bool blockedBySession() const { return m_blockedBySession; }
    bool readyToInstall() const { return false; }

    void offerUpdate()
    {
        m_state = QStringLiteral("available");
        m_availableUpdate = QVariantMap{{QStringLiteral("version"), QStringLiteral("9.9.9")},
                                        {QStringLiteral("url"), QStringLiteral("https://example.invalid/x")},
                                        {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}};
        emit changed();
    }

    Q_INVOKABLE void downloadUpdate(const QString&, const QString&) { ++m_downloads; }
    Q_INVOKABLE void installDownloaded() { ++m_installs; }

    int downloads() const { return m_downloads; }

signals:
    void changed();

private:
    QString m_state = QStringLiteral("idle");
    QVariantMap m_availableUpdate;
    int m_progress = 0;
    QVariantMap m_failure;
    bool m_blockedBySession = false;
    int m_downloads = 0;
    int m_installs = 0;
};

// The `bridge` a settings row talks to (SettingsBridge in production).
class FakeBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool writable READ writable NOTIFY writableChanged)

public:
    bool writable() const { return true; }

    Q_INVOKABLE QVariantList enumOptions(const QString&) const
    {
        return {QStringLiteral("Software H.264 (a deliberately long codec name)"),
                QStringLiteral("Hardware HEVC (a deliberately long decoder name)")};
    }
    Q_INVOKABLE QVariant getValue(const QString&) const
    {
        return QStringLiteral("Software H.264 (a deliberately long codec name)");
    }
    Q_INVOKABLE bool setValue(const QString&, const QVariant&) { return true; }
    Q_INVOKABLE QString negotiationWarning(const QString&) const { return QString(); }

signals:
    void writableChanged();
    void valueChanged(const QString& key);
    void negotiationChanged();
    void sessionOverridesChanged();
};

} // namespace

class TstUiScreens : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void metricsAreParsedFromTheGeneratedTokens();
    void everyShellScreenLoads();
    void homeScreenRendersEachHomeState();
    void homePlayReadsResumeSessionWhileASessionIsLiveAndResumesIt();
    void homeShowsTheServersRefusalVerbatimWithAQuietTopUpBeneathPlay();
    void noHomeStateRendersACountAQueuePositionOrANotifyControl();
    void homeWrapsLongSentencesAndNeverBreaksAReference();
    void homeScreenRendersTheSessionEndReason();
    void theClientRaisesNoNotificationOfAnyKind();
    void homeNoLongerCarriesTheBalanceElement();
    void appHeaderCarriesTheWordmarkTheBalanceAndTheMenuInThatOrder();
    void theShellGivesEverySignedInViewTheHeaderAndNeitherSignInNorTheSplash();
    void balancePillDrawsEachOfItsStates();
    void balancePillChangesColourExactlyAtTheSpecsThresholds();
    void menuHasItsTwoItemsAndTopUpAsksTheFacadeForTheWebsite();
    void menuOpensOnSpaceMovesWithArrowsAndEscapeReturnsFocusToTheTrigger();
    void restoreSplashShowsItsLineAndNeverTheSignInForm();
    void theShellRoutesTheRestoreStateToTheSplashNotToSignIn();
    void signInScreenReachesThePhoneStepError();
    void signInScreenCountsDownToResend();
    void identifierFieldShowsTheTagAtThreeDigitsAndHidesItOnALetter();
    void identifierFieldNamesTheCountryANumberWritesForItself();
    void identifierFieldKeepsTheDigitsWhenTheCountryChanges();
    void aLongEmailScrollsInsideTheFieldInsteadOfResizingIt();
    void theTagRevealsThroughOpacityAndATransformNeverALayoutProperty();
    void countryPickerSearchesByNameOrDialDigitsAndSaysSoWhenNothingMatches();
    void countryPickerHighlightsTheCountryInForceAndPicksIt();
    void countryPickerReadsOnlyTheBundledListAndElidesLongNames();
    void signInRoutesAPhoneToTheCodeStepAndKeepsTheNumberOnBack();
    void signInRoutesAnEmailToThePasswordStep();
    void signInNamesAFieldLevelMistakeBeforeAnythingIsSent();
    void signInShowsARefusalVerbatimAndClearsThePassword();
    void signInKeepsTheWidthOfTheBusyControl();
    void signInOpensTheWebsiteForSignupAndResetAndHasNoScreenOfItsOwn();
    void noSeatHubScreenBuildsAWebsiteAddressItself();
    void forcedUpdateModalKeepsUpdateTabbableAndUndismissable();
    void settingsDropdownElidesLongOptionNames();
    void disabledActionLabelStaysLegible();
    void agentConfigPanelMasksTheTokenAndNeverRendersIt();
    void theWindowSequenceRestoresOnlyAfterTheEngineIsDone();
    void theStepperDrawsThreeStagesFromTheFacadesStageAndNothingElse();
    void theStepperMarksTheStageThatWasActiveFailedWithAMarkOfItsOwn();
};

void TstUiScreens::initTestCase()
{
    QVERIFY2(!guiDir().isEmpty(), "app/gui could not be located from the test binary");
}

void TstUiScreens::metricsAreParsedFromTheGeneratedTokens()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    // Read the singleton values the way QML does, so this asserts the parsed numbers and
    // not the token strings they come from (audit F15: Metrics must derive, not duplicate).
    QQmlComponent probe(&engine);
    probe.setData(QByteArrayLiteral(
                      "import QtQml\n"
                      "import SeatHub.Tokens 1.0\n"
                      "QtObject {\n"
                      "    property int s5: Metrics.s5\n"
                      "    property int radiusLg: Metrics.radiusLg\n"
                      "    property int fontSm: Metrics.fontSm\n"
                      "    property int fontH1: Metrics.fontH1\n"
                      "    property int fontCaption: Metrics.fontCaption\n"
                      "    property int motionFast: Metrics.motionFast\n"
                      "    property int motionPage: Metrics.motionPage\n"
                      "    property int touchTarget: Metrics.touchTarget\n"
                      "    property int pointerTarget: Metrics.pointerTarget\n"
                      "    property int captionFromRem: Metrics.tokenRem(Tokens.scaleCaptionSize)\n"
                      "    property int spacingFromPx: Metrics.tokenPx(Tokens.step6)\n"
                      "}\n"),
                  QUrl(QStringLiteral("qrc:/tst_metrics_probe.qml")));
    QScopedPointer<QObject> values(probe.create());
    QVERIFY2(values, qPrintable(probe.errorString()));

    QCOMPARE(values->property("s5").toInt(), 20);              // 20px step
    QCOMPARE(values->property("radiusLg").toInt(), 20);        // 20px radius
    QCOMPARE(values->property("fontSm").toInt(), 14);          // 0.875rem
    QCOMPARE(values->property("fontH1").toInt(), 32);          // 2rem
    QCOMPARE(values->property("fontCaption").toInt(), 13);     // 0.8125rem
    QCOMPARE(values->property("motionFast").toInt(), 120);     // 120ms
    QCOMPARE(values->property("motionPage").toInt(), 420);     // 420ms
    QCOMPARE(values->property("touchTarget").toInt(), 44);     // ui.md §9
    QCOMPARE(values->property("pointerTarget").toInt(), 40);
    QCOMPARE(values->property("captionFromRem").toInt(), 13);  // rem conversion is real
    QCOMPARE(values->property("spacingFromPx").toInt(), 24);
}

void TstUiScreens::everyShellScreenLoads()
{
    // Every screen the shell can put on screen, plus the two components this pass added.
    // `main.qml` is excluded: it instantiates the real SeatHubClient type, which only the
    // application registers. The resource checker below covers it instead.
    const QStringList screens = {
        QStringLiteral("Tokens.qml"),        QStringLiteral("Metrics.qml"),
        QStringLiteral("SignInScreen.qml"),  QStringLiteral("HomeScreen.qml"),
        QStringLiteral("ErrorScreen.qml"),   QStringLiteral("SettingsPage.qml"),
        QStringLiteral("SessionSegue.qml"),  QStringLiteral("ForcedUpdateModal.qml"),
        QStringLiteral("SeatHubOTPField.qml"), QStringLiteral("SeatHubButton.qml"),
        QStringLiteral("SeatHubStepper.qml"), QStringLiteral("SeatHubToggle.qml"),
        QStringLiteral("SeatHubSelect.qml"), QStringLiteral("SeatHubNumberField.qml"),
                              QStringLiteral("SeatHubReadOnlyRow.qml"),
                              QStringLiteral("AgentConfigPanel.qml"),
                              QStringLiteral("BalancePill.qml"),
                              QStringLiteral("AppHeader.qml"),
                              QStringLiteral("SeatHubMenu.qml"),
                              QStringLiteral("RestoreSplash.qml"),
                              QStringLiteral("SeatHubIdentifierField.qml"),
                              QStringLiteral("SeatHubCountryPicker.qml"),
    };

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    FakeUpdates updates;
    FakeBridge bridge;

    for (const QString& screen : screens) {
        QString error;
        QScopedPointer<QObject> root(instantiate(&engine, screen, &error));
        QVERIFY2(root, qPrintable(screen + QStringLiteral(": ") + error));

        // The shell screens take their facade through a property; give them one so their
        // bindings and handlers come up in the state production uses.
        if (root->metaObject()->indexOfProperty("client") >= 0) {
            root->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client)));
        }
        if (root->metaObject()->indexOfProperty("updates") >= 0) {
            root->setProperty("updates", QVariant::fromValue(static_cast<QObject*>(&updates)));
        }
        if (root->metaObject()->indexOfProperty("bridge") >= 0) {
            root->setProperty("bridge", QVariant::fromValue(static_cast<QObject*>(&bridge)));
        }
    }

    // And the resources: every QML file the app compiles in must exist on disk under the
    // path `app/qml.qrc` names, so a rename cannot leave the binary without a screen.
    const QString qrcPath = QDir(guiDir()).filePath(QStringLiteral("../qml.qrc"));
    QFile qrc(qrcPath);
    QVERIFY2(qrc.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(qrcPath));
    const QString qrcText = QString::fromUtf8(qrc.readAll());
    for (const QString& screen : screens) {
        QVERIFY2(qrcText.contains(QStringLiteral("gui/") + screen),
                 qPrintable(screen + QStringLiteral(" is not listed in app/qml.qrc")));
        QVERIFY2(QFile::exists(guiDir() + QLatin1Char('/') + screen),
                 qPrintable(screen + QStringLiteral(" is not on disk")));
    }

    // The retired language switcher must not be compiled in (ADR-0043, audit F18).
    QVERIFY2(!qrcText.contains(QStringLiteral("gui/SettingsView.qml")),
             "the retired 27-locale language switcher must not be in the resource");
}

namespace {

// The buttons a screen shows, visible ones only.
QList<QObject*> visibleButtons(QObject* root)
{
    QList<QObject*> buttons;
    for (QObject* item : root->findChildren<QObject*>()) {
        if (item->inherits("QQuickButton") && effectivelyVisible(item)) {
            buttons.append(item);
        }
    }
    return buttons;
}

// Every line Home used to hold that the copy deck no longer carries.
const QStringList kRetiredHomeLines = {
    QStringLiteral("Ready"),
    QStringLiteral("Open SeatHub and press Play."),
    QStringLiteral("All rigs are busy right now."),
    QStringLiteral("Notify me"),
};

// Where `object` sits in `column`'s stacking order, whichever of the column's own children it is or
// lives inside. A Column lays its children out in declaration order, and an item tree that is not on
// a screen has not been laid out yet, so the order is read from the tree rather than from `y`.
int stackIndexIn(QObject* object, QObject* column)
{
    QQuickItem* parentColumn = qobject_cast<QQuickItem*>(column);
    for (QQuickItem* item = qobject_cast<QQuickItem*>(object); item; item = item->parentItem()) {
        if (item->parentItem() == parentColumn) {
            return parentColumn->childItems().indexOf(item);
        }
    }
    return -1;
}

} // namespace

void TstUiScreens::homeScreenRendersEachHomeState()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    QObject* play = screen->findChild<QObject*>(QStringLiteral("playButton"));
    QVERIFY2(play, "Home's primary action must be a real button, reachable by keyboard (audit F3)");
    QVERIFY(play->inherits("QQuickButton"));
    QVERIFY(play->property("activeFocusOnTab").toBool());

    // ready: nothing but Play. The two lines the deck no longer carries are gone.
    client.setHomeStatus(QStringLiteral("ready"));
    QVERIFY(effectivelyVisible(play));
    QCOMPARE(play->property("text").toString(), QStringLiteral("Play"));
    QVERIFY(play->property("enabled").toBool());
    QVERIFY(!play->property("busy").toBool());
    QVERIFY(screen->findChild<QObject*>(QStringLiteral("stateLine"))->property("text")
                .toString().isEmpty());
    for (const QString& retired : kRetiredHomeLines) {
        QVERIFY2(!findVisibleTextItem(screen.data(), retired), qPrintable(retired));
    }

    // checking: the named loading state (copy.md section 5, "name the thing loading"), and Play is
    // busy so nothing on the page moves when work starts.
    client.setHomeStatus(QStringLiteral("checking"));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Checking availability…")),
             "Home must render a named loading state while Play allocates");
    QVERIFY(play->property("busy").toBool());

    // busy: nothing is free. The deck's one sentence, verbatim, and Play is still pressable.
    client.setHomeStatus(QStringLiteral("busy"));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("None available right now.")),
             "Home must render the no-rig sentence from copy.md");
    QVERIFY(play->property("enabled").toBool());
    QVERIFY(!play->property("busy").toBool());
    QVERIFY2(!findVisibleTextItem(screen.data(), QStringLiteral("Checking availability…")),
             "the loading line must not outlive the state");
    for (const QString& retired : kRetiredHomeLines) {
        QVERIFY2(!findVisibleTextItem(screen.data(), retired), qPrintable(retired));
    }

    // offline: copy.md section Support & errors, the full sentence, verbatim. Play stays pressable
    // so a customer whose Wi-Fi came back can just press it.
    const QString offline =
        QStringLiteral("Can't reach SevenHills right now. Showing the last known balance.");
    client.setHomeStatus(QStringLiteral("offline"));
    QVERIFY2(findVisibleTextItem(screen.data(), offline),
             "Home must render the whole offline sentence from copy.md");
    QVERIFY(play->property("enabled").toBool());
    QVERIFY(effectivelyVisible(play));

    // refused has its own test. Back to ready: Play is the one action and no state line is left.
    client.setHomeStatus(QStringLiteral("ready"));
    QVERIFY(effectivelyVisible(play));
    QVERIFY2(!findVisibleTextItem(screen.data(), offline), "a state's line must not outlive the state");
}

void TstUiScreens::homePlayReadsResumeSessionWhileASessionIsLiveAndResumesIt()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));
    QObject* play = screen->findChild<QObject*>(QStringLiteral("playButton"));
    QVERIFY(play);

    QCOMPARE(play->property("text").toString(), QStringLiteral("Play"));

    // copy.md C3: the label changes while a session is live, and it is still the one control.
    client.setLiveSession(true);
    QCOMPARE(play->property("text").toString(), QStringLiteral("Resume session"));
    QVERIFY(!findVisibleTextItem(screen.data(), QStringLiteral("Play")));

    // Pressing it is the facade's `start()`, which resumes rather than asks for another session (the
    // facade's own test asserts that half).
    QVERIFY(QMetaObject::invokeMethod(play, "clicked"));
    QCOMPARE(client.starts(), 1);

    // A live session does not change the state lines: resume and the no-rig line are separate.
    client.setHomeStatus(QStringLiteral("busy"));
    QCOMPARE(play->property("text").toString(), QStringLiteral("Resume session"));

    client.setLiveSession(false);
    QCOMPARE(play->property("text").toString(), QStringLiteral("Play"));

    // Play is the largest control on the screen: 64px, taller than every other button.
    QCOMPARE(play->property("implicitHeight").toInt(), 64);
    for (QObject* button : visibleButtons(screen.data())) {
        if (button != play) {
            QVERIFY(button->property("implicitHeight").toInt() < 64);
        }
    }
}

void TstUiScreens::homeShowsTheServersRefusalVerbatimWithAQuietTopUpBeneathPlay()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    QObject* play = screen->findChild<QObject*>(QStringLiteral("playButton"));
    QObject* topUp = screen->findChild<QObject*>(QStringLiteral("topUpButton"));
    QVERIFY(play);
    QVERIFY(topUp);

    // Before a refusal there is no top-up control on Home: it is the menu's job until the server
    // says no.
    client.setHomeStatus(QStringLiteral("ready"));
    QVERIFY(!effectivelyVisible(topUp));

    // The server's own sentence and its reference. Whatever they say is shown exactly as they came:
    // the client neither rewrites the sentence nor applies a balance rule of its own.
    const QString sentence =
        QStringLiteral("You need at least 15 minutes of credit to start a session.");
    client.setFailure(sentence, QStringLiteral("SH-4F7KQ2"));
    client.setHomeStatus(QStringLiteral("refused"));

    QObject* sentenceItem = findVisibleTextItem(screen.data(), sentence);
    QVERIFY2(sentenceItem, "the refusal must be shown in the server's own words");
    QCOMPARE(sentenceItem->property("color").value<QColor>(), QColor(QStringLiteral("#ef4444")));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("◆")),
             "colour is never the only signal: the refusal carries a glyph");
    QObject* referenceItem = findVisibleTextItem(screen.data(), QStringLiteral("SH-4F7KQ2"));
    QVERIFY2(referenceItem, "the ADR-0008 reference must be rendered");
    QCOMPARE(referenceItem->property("font").value<QFont>().family(),
             QString::fromLatin1(kMonoFamily));

    // A refusal is not the error view: Home still shows Play, pressable.
    QVERIFY(effectivelyVisible(play));
    QVERIFY(play->property("enabled").toBool());

    // A quiet top-up control under Play: ghost, with the external-link mark, and it asks the facade.
    QVERIFY(effectivelyVisible(topUp));
    QCOMPARE(topUp->property("variant").toString(), QStringLiteral("ghost"));
    QCOMPARE(topUp->property("glyph").toString(), QStringLiteral("↗"));
    QCOMPARE(topUp->property("text").toString(), QStringLiteral("Top up"));
    QObject* column = play->parent();
    QVERIFY2(stackIndexIn(topUp, column) > stackIndexIn(play, column),
             "the top-up control sits beneath Play");
    QVERIFY2(stackIndexIn(sentenceItem, column) >= 0
                 && stackIndexIn(sentenceItem, column) < stackIndexIn(play, column),
             "the refusal sits above Play");

    QVERIFY(QMetaObject::invokeMethod(topUp, "clicked"));
    QCOMPARE(client.topUps(), 1);

    // A different sentence from the server is shown as it is, not looked up in a table of the client's.
    const QString other = QStringLiteral("You already have a session running.");
    client.setFailure(other, QStringLiteral("SH-9K2XQ1"));
    QVERIFY(findVisibleTextItem(screen.data(), other));
    QVERIFY(!findVisibleTextItem(screen.data(), sentence));

    // Leaving the refused state takes the refusal and the top-up control away.
    client.setHomeStatus(QStringLiteral("ready"));
    QVERIFY(!findVisibleTextItem(screen.data(), other));
    QVERIFY(!findVisibleTextItem(screen.data(), QStringLiteral("SH-9K2XQ1")));
    QVERIFY(!effectivelyVisible(topUp));

    // The client holds no balance rule: this screen does not even read the balance.
    const QString source = readSource(guiDir() + QStringLiteral("/HomeScreen.qml"));
    QVERIFY2(!source.contains(QStringLiteral("balanceMinutes")),
             "Home must not read the balance: whether a customer may play is the server's answer");
}

void TstUiScreens::noHomeStateRendersACountAQueuePositionOrANotifyControl()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    // No identity, so the only digits that could be on screen are a fleet count or a queue position.
    client.setIdentity(QString());
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    const QStringList states = { QStringLiteral("ready"), QStringLiteral("checking"),
                                 QStringLiteral("busy"), QStringLiteral("offline") };
    const QStringList forbiddenWords = { QStringLiteral("notify"),   QStringLiteral("queue"),
                                         QStringLiteral("position"), QStringLiteral("estimate"),
                                         QStringLiteral("waiting"), QStringLiteral("ahead of"),
                                         QStringLiteral("machines"), QStringLiteral("rigs") };
    for (bool live : { false, true }) {
        client.setLiveSession(live);
        for (const QString& state : states) {
            client.setHomeStatus(state);

            // Nothing that names a number of machines, a place in a line or a notification: no digit
            // anywhere, and none of the words that would say so.
            for (QObject* item : textItems(screen.data())) {
                if (!effectivelyVisible(item)) {
                    continue;
                }
                const QString text = item->property("text").toString();
                const QString where = state + QStringLiteral(": ") + text;
                QVERIFY2(!text.contains(QRegularExpression(QStringLiteral("[0-9]"))), qPrintable(where));
                for (const QString& word : forbiddenWords) {
                    QVERIFY2(!text.contains(word, Qt::CaseInsensitive), qPrintable(where));
                }
            }

            // Exactly one bright control, and it is Play: everything else is quiet.
            int bright = 0;
            for (QObject* button : visibleButtons(screen.data())) {
                const QString label = button->property("text").toString();
                QVERIFY2(!label.contains(QStringLiteral("Notify"), Qt::CaseInsensitive),
                         qPrintable(label));
                if (button->property("variant").toString() == QStringLiteral("primary")) {
                    ++bright;
                    QVERIFY(label == QStringLiteral("Play")
                            || label == QStringLiteral("Resume session"));
                }
            }
            QCOMPARE(bright, 1);
        }
    }

    // The balance is data: it is not on Home at all, so it cannot compete with Play.
    QVERIFY(!screen->findChild<QObject*>(QStringLiteral("balancePill")));
}

void TstUiScreens::homeWrapsLongSentencesAndNeverBreaksAReference()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));
    // The 960x640 minimum window: the column is 420px at most.
    QVERIFY(screen->setProperty("width", 960));
    QVERIFY(screen->setProperty("height", 640));

    const QString longSentence = QStringLiteral(
        "This is a much longer sentence than any the deck holds, written to prove that it wraps "
        "inside the column instead of running off the edge of the window or being clipped.");
    client.setFailure(longSentence, QStringLiteral("SH-4F7KQ2"));
    client.setHomeStatus(QStringLiteral("refused"));

    QObject* sentence = screen->findChild<QObject*>(QStringLiteral("refusalText"));
    QVERIFY(sentence);
    QVERIFY2(sentence->property("wrapMode").toInt() != 0, "a long sentence must wrap");
    QVERIFY2(sentence->property("lineCount").toInt() > 1, "the long sentence must have wrapped");
    QVERIFY2(sentence->property("width").toDouble() <= 420, "and stay inside the 420px column");

    // The reference is one line, always: a broken code cannot be read aloud or searched for.
    QObject* reference = screen->findChild<QObject*>(QStringLiteral("refusalReference"));
    QVERIFY(reference);
    QCOMPARE(reference->property("wrapMode").toInt(), 0); // Text.NoWrap
    QCOMPARE(reference->property("lineCount").toInt(), 1);

    // The one line of the other states wraps too.
    client.setHomeStatus(QStringLiteral("offline"));
    QObject* line = screen->findChild<QObject*>(QStringLiteral("stateLine"));
    QVERIFY(line);
    QVERIFY(line->property("wrapMode").toInt() != 0);
}

void TstUiScreens::homeScreenRendersTheSessionEndReason()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // copy.md §Session end reasons, `CUSTOMER_ENDED`, verbatim. This is the surface E10
    // asked for: before this pass no client window rendered an end reason at all.
    const QString ended = QStringLiteral("You ended the session. Unused minutes stay in your account.");
    client.setEndReasonText(ended);
    QVERIFY2(findVisibleTextItem(screen.data(), ended),
             "Home must render the session's end reason (audit E10)");

    // A long sentence must not break the layout: it wraps rather than clipping.
    QObject* reasonItem = findVisibleTextItem(screen.data(), ended);
    QVERIFY(reasonItem);
    const QMetaObject* metaObject = reasonItem->metaObject();
    if (metaObject->indexOfProperty("wrapMode") >= 0) {
        QVERIFY2(reasonItem->property("wrapMode").toInt() != 0,
                 "the end-reason sentence must wrap at the 960x640 minimum");
    }

    client.setEndReasonText(QString());
    QVERIFY2(!findVisibleTextItem(screen.data(), ended),
             "a cleared end reason must not stay on screen");
}

void TstUiScreens::homeNoLongerCarriesTheBalanceElement()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    client.setBalance(135, QStringLiteral("2 h 15 min"), false);

    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // The balance moved to the header, which is on every signed-in screen (CUST-06, D-19). Two
    // pills on Home would be two sources of the same number.
    QVERIFY2(!screen->findChild<QObject*>(QStringLiteral("balancePill")),
             "Home must not carry its own balance element: the header does");
    QVERIFY(!findVisibleTextItem(screen.data(), QStringLiteral("2 h 15 min")));
}

void TstUiScreens::appHeaderCarriesTheWordmarkTheBalanceAndTheMenuInThatOrder()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    client.setBalance(135, QStringLiteral("2 h 15 min"), false);

    QString error;
    QScopedPointer<QObject> header(instantiate(&engine, QStringLiteral("AppHeader.qml"), &error));
    QVERIFY2(header, qPrintable(error));
    QVERIFY(header->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // A 64px row (`s16`), the design contract's height. Its width comes from the window in the app;
    // here it is given one so the right-hand items have somewhere to anchor.
    QCOMPARE(header->property("height").toInt(), 64);
    QVERIFY(header->setProperty("width", 960));

    // The balance element is in it and shows the facade's own text: the client draws it and does no
    // arithmetic (CUST-06).
    QObject* pill = header->findChild<QObject*>(QStringLiteral("balancePill"));
    QVERIFY2(pill, "the header must carry the balance element");
    QVERIFY(findVisibleTextItem(pill, QStringLiteral("2 h 15 min")));

    // And it follows the facade: a new balance is drawn without a reload.
    client.setBalance(45, QStringLiteral("45 min"), false);
    QVERIFY(findVisibleTextItem(pill, QStringLiteral("45 min")));
    QVERIFY(!findVisibleTextItem(pill, QStringLiteral("2 h 15 min")));

    // The menu is in it, and the balance is not a control: nothing about the pill takes focus or a
    // click, so the customer's eye stays on Play (values).
    QObject* menu = header->findChild<QObject*>(QStringLiteral("seatHubMenu"));
    QVERIFY2(menu, "the header must carry the menu");
    QVERIFY2(!pill->property("activeFocusOnTab").toBool(), "the balance element is not focusable");
    for (QObject* item : pill->findChildren<QObject*>()) {
        QVERIFY2(!item->inherits("QQuickButton"), "the balance element is data, not a button");
        QVERIFY2(!item->inherits("QQuickMouseArea"), "the balance element takes no clicks");
    }

    // Wordmark, then balance, then menu, left to right, each measured in the header's own
    // coordinates.
    QObject* wordmark = findVisibleTextItem(header.data(), QStringLiteral("SeatHub"));
    QVERIFY2(wordmark, "the header must carry the wordmark");
    auto xIn = [&](QObject* object) {
        double x = 0;
        for (QQuickItem* item = qobject_cast<QQuickItem*>(object);
             item && item != static_cast<QQuickItem*>(header.data()); item = item->parentItem()) {
            x += item->x();
        }
        return x;
    };
    QVERIFY2(xIn(wordmark) < xIn(pill), "the wordmark is on the left of the balance");
    QVERIFY2(xIn(pill) < xIn(menu), "the balance comes before the menu button");
}

void TstUiScreens::theShellGivesEverySignedInViewTheHeaderAndNeitherSignInNorTheSplash()
{
    // `main.qml` instantiates the real SeatHubClient, which only the application registers, so it
    // cannot be loaded here (05-02 deviation 2 set this technique). What can be tested for real is
    // the rule itself: `showsHeader()` is lifted out of the source and evaluated for every state
    // the facade can be in, and the wiring around it is read as source.
    QFile file(guiDir() + QStringLiteral("/main.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "main.qml must be readable");
    const QString source = QString::fromUtf8(file.readAll());

    const QRegularExpression fn(
        QStringLiteral("(function showsHeader\\(state, signedIn\\) \\{.*?\\n    \\})"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = fn.match(source);
    QVERIFY2(match.hasMatch(), "main.qml must define showsHeader(state, signedIn)");

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    QQmlComponent probe(&engine);
    probe.setData((QStringLiteral("import QtQml\nQtObject {\n") + match.captured(1)
                   + QStringLiteral("\n}\n")).toUtf8(),
                  QUrl(QStringLiteral("qrc:/tst_header_rule.qml")));
    QScopedPointer<QObject> rule(probe.create());
    QVERIFY2(rule, qPrintable(probe.errorString()));

    auto shows = [&](const QString& state, bool signedIn) {
        QVariant answer;
        QMetaObject::invokeMethod(rule.data(), "showsHeader", Q_RETURN_ARG(QVariant, answer),
                                  Q_ARG(QVariant, state), Q_ARG(QVariant, signedIn));
        return answer.toBool();
    };

    // Every signed-in view has it: Home (and Settings, which is a view inside home), Connecting,
    // and the error view.
    for (const QString& state : { QStringLiteral("home"), QStringLiteral("connecting"),
                                  QStringLiteral("streaming"), QStringLiteral("error") }) {
        QVERIFY2(shows(state, true), qPrintable(state + QStringLiteral(" must carry the header")));
    }

    // Sign-in and the restore splash never do, and nothing does before a customer is signed in.
    QVERIFY(!shows(QStringLiteral("signed_out"), false));
    QVERIFY(!shows(QStringLiteral("signed_out"), true));
    QVERIFY(!shows(QStringLiteral("restoring"), false));
    QVERIFY(!shows(QStringLiteral("restoring"), true));
    QVERIFY(!shows(QStringLiteral("home"), false));
    QVERIFY(!shows(QStringLiteral("error"), false));
    QVERIFY2(!shows(QStringLiteral("a state nobody named"), true),
             "an unnamed state must not grow a header by accident");

    // The wiring: one header, under the forced-update modal, with the loader laid out below it, and
    // none of the screens carrying a copy of their own.
    QCOMPARE(source.count(QStringLiteral("AppHeader {")), 1);
    QVERIFY(source.contains(QStringLiteral("client: seatHub")));
    QVERIFY(source.contains(
        QStringLiteral("visible: window.showsHeader(seatHub.appState, seatHub.signedIn)")));
    QVERIFY2(source.contains(QStringLiteral("anchors.top: appHeader.visible ? appHeader.bottom")),
             "the view loader must sit below the header, not behind it");
    QVERIFY2(source.indexOf(QStringLiteral("AppHeader {"))
                 < source.indexOf(QStringLiteral("ForcedUpdateModal {")),
             "the forced-update modal must still cover the header");

    for (const QString& name : { QStringLiteral("HomeScreen.qml"), QStringLiteral("SettingsPage.qml"),
                                 QStringLiteral("ErrorScreen.qml"), QStringLiteral("SignInScreen.qml"),
                                 QStringLiteral("RestoreSplash.qml"),
                                 QStringLiteral("ForcedUpdateModal.qml") }) {
        QVERIFY2(!readSource(guiDir() + QLatin1Char('/') + name)
                       .contains(QRegularExpression(QStringLiteral("AppHeader\s*\{"))),
                 qPrintable(name + QStringLiteral(" must not carry a header of its own")));
    }

    // Compiled in: a control missing from the resource file would not exist at runtime.
    QFile qrc(guiDir() + QStringLiteral("/../qml.qrc"));
    QVERIFY(qrc.open(QIODevice::ReadOnly));
    const QString qrcText = QString::fromUtf8(qrc.readAll());
    for (const QString& name : { QStringLiteral("AppHeader.qml"), QStringLiteral("SeatHubMenu.qml"),
                                 QStringLiteral("BalancePill.qml") }) {
        QVERIFY2(qrcText.contains(QStringLiteral("gui/") + name),
                 qPrintable(name + QStringLiteral(" is not in app/qml.qrc")));
    }
}

void TstUiScreens::balancePillDrawsEachOfItsStates()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> pill(instantiate(&engine, QStringLiteral("BalancePill.qml"), &error));
    QVERIFY2(pill, qPrintable(error));
    QVERIFY(pill->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // populated: the value in the mono family, `--foreground`, tabular.
    client.setBalance(135, QStringLiteral("2 h 15 min"), false);
    QObject* value = findVisibleTextItem(pill.data(), QStringLiteral("2 h 15 min"));
    QVERIFY2(value, "the populated balance must render");
    QCOMPARE(value->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(value->property("color").value<QColor>(), QColor(QStringLiteral("#fafafa")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("last known")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("Unavailable")));

    // low (10 minutes or fewer): warn colour, and a glyph so colour is not the only signal.
    client.setBalance(8, QStringLiteral("8 min"), false);
    value = findVisibleTextItem(pill.data(), QStringLiteral("8 min"));
    QVERIFY(value);
    QCOMPARE(value->property("color").value<QColor>(), QColor(QStringLiteral("#f59e0b")));
    QVERIFY2(findVisibleTextItem(pill.data(), QStringLiteral("▲")),
             "a low balance must carry a glyph besides the colour");

    // critical (2 minutes or fewer): destructive colour, and a different glyph.
    client.setBalance(2, QStringLiteral("2 min"), false);
    value = findVisibleTextItem(pill.data(), QStringLiteral("2 min"));
    QVERIFY(value);
    QCOMPARE(value->property("color").value<QColor>(), QColor(QStringLiteral("#ef4444")));
    QVERIFY(findVisibleTextItem(pill.data(), QStringLiteral("◆")));

    // last known: the read failed but an earlier one succeeded. The value stays, muted, and says so.
    client.setBalance(8, QStringLiteral("8 min"), true);
    value = findVisibleTextItem(pill.data(), QStringLiteral("8 min"));
    QVERIFY2(value, "a failed read must not blank the balance");
    QCOMPARE(value->property("color").value<QColor>(), QColor(QStringLiteral("#a3a3a3")));
    QVERIFY2(findVisibleTextItem(pill.data(), QStringLiteral("last known")),
             "a stale balance must be labelled");

    // unavailable: no read has ever succeeded and the last one failed.
    client.setBalance(-1, QString(), true);
    QVERIFY(findVisibleTextItem(pill.data(), QStringLiteral("Unavailable")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("last known")));

    // Never a raw minute count, and never a second formatter: the element draws what the facade
    // gives it, and `duration_text` is the one place minutes become words.
    QFile source(guiDir() + QStringLiteral("/BalancePill.qml"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(source.readAll());
    QVERIFY2(!text.contains(QStringLiteral(" min\"")) && !text.contains(QStringLiteral(" h \"")),
             "the balance element must not format a duration itself");
}

void TstUiScreens::balancePillChangesColourExactlyAtTheSpecsThresholds()
{
    // `screens.md` section 25 and `timing.md`: warn at 10 minutes and under, danger at 2 minutes and
    // under. The thresholds are the spec's, so the boundary values are pinned on both sides.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> pill(instantiate(&engine, QStringLiteral("BalancePill.qml"), &error));
    QVERIFY2(pill, qPrintable(error));
    QVERIFY(pill->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    struct Case { qint64 minutes; const char* level; };
    const Case cases[] = { { 61, "normal" }, { 11, "normal" }, { 10, "low" },
                           { 3, "low" },     { 2, "critical" }, { 0, "critical" } };
    for (const Case& c : cases) {
        client.setBalance(c.minutes, QStringLiteral("x"), false);
        QCOMPARE(pill->property("level").toString(), QString::fromLatin1(c.level));
    }

    // Colour is never the only signal: the two glyphs differ from each other, and a normal balance has
    // neither.
    client.setBalance(11, QStringLiteral("11 min"), false);
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("▲")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("◆")));
    client.setBalance(10, QStringLiteral("10 min"), false);
    QVERIFY(findVisibleTextItem(pill.data(), QStringLiteral("▲")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("◆")));
    client.setBalance(2, QStringLiteral("2 min"), false);
    QVERIFY(findVisibleTextItem(pill.data(), QStringLiteral("◆")));
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("▲")));

    // A stale value is not judged against the thresholds: the read that made it stale may be hours
    // old, and a warning colour on a number that may no longer be true would say something the client
    // does not know.
    client.setBalance(2, QStringLiteral("2 min"), true);
    QCOMPARE(pill->property("level").toString(), QStringLiteral("muted"));

    // Never read and nothing failed yet: the first-read skeleton, with neither a value nor a word.
    client.setBalance(-1, QString(), false);
    QVERIFY(pill->property("loading").toBool());
    QVERIFY(!findVisibleTextItem(pill.data(), QStringLiteral("Unavailable")));
}

void TstUiScreens::menuHasItsTwoItemsAndTopUpAsksTheFacadeForTheWebsite()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> menu(instantiate(&engine, QStringLiteral("SeatHubMenu.qml"), &error));
    QVERIFY2(menu, qPrintable(error));
    QVERIFY(menu->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // The trigger is a real button with the accessible name `Menu`, tabbable, and 40x40.
    QObject* trigger = menu->property("trigger").value<QObject*>();
    QVERIFY(trigger);
    QVERIFY(trigger->inherits("QQuickButton"));
    QVERIFY(trigger->property("activeFocusOnTab").toBool());
    QCOMPARE(menu->property("implicitWidth").toInt(), 40);
    QCOMPARE(menu->property("implicitHeight").toInt(), 40);

    // Exactly the two items this plan ships, in order: Top up, then Settings. Profile is not here
    // yet, so nothing in the menu points at a screen that does not exist.
    QObject* popup = menu->property("menu").value<QObject*>();
    QVERIFY(popup);
    QCOMPARE(popup->property("count").toInt(), 2);
    QObject* topUp = menu->findChild<QObject*>(QStringLiteral("menuItemTopUp"));
    QObject* settings = menu->findChild<QObject*>(QStringLiteral("menuItemSettings"));
    QVERIFY(topUp);
    QVERIFY(settings);
    QCOMPARE(topUp->property("text").toString(), QStringLiteral("Top up"));
    QCOMPARE(settings->property("text").toString(), QStringLiteral("Settings"));

    // `Top up` leaves the app, and says so with the external-link mark; `Settings` does not.
    QCOMPARE(topUp->property("glyph").toString(), QStringLiteral("↗"));
    QVERIFY(settings->property("glyph").toString().isEmpty());

    // Activating an item calls the facade, and only the facade: no address is built here. The URL
    // itself is asserted against the real facade in `tst_facade_wiring`.
    QVERIFY(QMetaObject::invokeMethod(topUp, "triggered"));
    QCOMPARE(client.topUps(), 1);
    QCOMPARE(client.settingsOpens(), 0);
    QVERIFY(QMetaObject::invokeMethod(settings, "triggered"));
    QCOMPARE(client.settingsOpens(), 1);
    QCOMPARE(client.topUps(), 1);

    const QString source = readSource(guiDir() + QStringLiteral("/SeatHubMenu.qml"));
    QVERIFY2(source.contains(QStringLiteral("client.openTopUp()")),
             "the top-up item must ask the facade, not open an address itself");
    QVERIFY(!source.contains(QStringLiteral("Qt.openUrlExternally")));
    QVERIFY(!source.contains(QStringLiteral("http")));

    // A click outside closes it, and Escape does.
    const int policy = popup->property("closePolicy").toInt();
    QVERIFY2(policy & 0x01, "a click outside must close the menu");  // Popup.CloseOnPressOutside
    QVERIFY2(policy & 0x10, "Escape must close the menu");           // Popup.CloseOnEscape
}

void TstUiScreens::menuOpensOnSpaceMovesWithArrowsAndEscapeReturnsFocusToTheTrigger()
{
    // The keyboard behaviour the design contract asks for, driven with real key events in a real
    // (if unseen) window: Space opens, the arrow keys move, Escape closes and the trigger has focus
    // again. This is the one test here that needs a window, so it builds one.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
                          "import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "ApplicationWindow {\n"
                          "    id: win\n"
                          "    width: 400; height: 300; visible: true\n"
                          "    property var client: null\n"
                          "    SeatHubMenu {\n"
                          "        objectName: \"underTest\"\n"
                          "        anchors.right: parent.right\n"
                          "        anchors.top: parent.top\n"
                          "        client: win.client\n"
                          "    }\n"
                          "}\n"),
                      QUrl::fromLocalFile(guiDir() + QStringLiteral("/tst_menu_window.qml")));
    QScopedPointer<QObject> created(component.create());
    QVERIFY2(created, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(created.data());
    QVERIFY(window);
    QVERIFY(window->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));
    QVERIFY(QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));

    QObject* menu = window->findChild<QObject*>(QStringLiteral("underTest"));
    QVERIFY(menu);
    auto* trigger = menu->property("trigger").value<QQuickItem*>();
    QObject* popup = menu->property("menu").value<QObject*>();
    QVERIFY(trigger);
    QVERIFY(popup);

    trigger->forceActiveFocus();
    QVERIFY(trigger->hasActiveFocus());
    QVERIFY(!popup->property("opened").toBool());

    // Space opens it.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 3000);

    // The arrow keys move through the items and Enter activates one.
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Up);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!popup->property("opened").toBool(), 3000);
    QCOMPARE(client.topUps() + client.settingsOpens(), 1);
    QVERIFY2(client.topUps() == 1, "Down, Down, Up from nothing highlighted lands on the first item");

    // Escape closes it and returns focus to the trigger.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 3000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!popup->property("opened").toBool(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(trigger->hasActiveFocus(), 3000);
}

void TstUiScreens::theClientRaisesNoNotificationOfAnyKind()
{
    // CUST-09, D-20: the client raises no tray icon, no balloon, no operating-system notice and no
    // sound for any event, the balance running low included: state changes are shown in place. This
    // scans the client's own sources for the mechanisms that would, so adding one fails here.
    const QStringList forbidden = {
        QStringLiteral("QSystemTrayIcon"),       QStringLiteral("showMessage("),
        QStringLiteral("QSoundEffect"),          QStringLiteral("SoundEffect"),
        QStringLiteral("QSound"),                QStringLiteral("QMediaPlayer"),
        QStringLiteral("QAudioOutput"),          QStringLiteral("QApplication::beep"),
        QStringLiteral("QGuiApplication::beep"), QStringLiteral("MessageBeep"),
        QStringLiteral("Shell_NotifyIcon"),      QStringLiteral("ToastNotification"),
        QStringLiteral("Platform.SystemTrayIcon"),
    };

    const QDir seathub(guiDir() + QStringLiteral("/../seathub"));
    const QDir gui(guiDir());
    QStringList files;
    for (const QString& name :
         seathub.entryList({ QStringLiteral("*.cpp"), QStringLiteral("*.h") })) {
        files.append(seathub.filePath(name));
    }
    for (const QString& name : gui.entryList({ QStringLiteral("*.qml") })) {
        files.append(gui.filePath(name));
    }
    QVERIFY2(files.size() > 40, "the scan must actually see the client's sources");
    for (const QString& path : files) {
        const QString source = readSource(path);
        QVERIFY2(!source.isEmpty(), qPrintable(path));
        for (const QString& pattern : forbidden) {
            QVERIFY2(!source.contains(pattern),
                     qPrintable(QFileInfo(path).fileName() + QStringLiteral(" uses ") + pattern));
        }
    }
}

void TstUiScreens::restoreSplashShowsItsLineAndNeverTheSignInForm()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> splash(instantiate(&engine, QStringLiteral("RestoreSplash.qml"), &error));
    QVERIFY2(splash, qPrintable(error));

    // copy.md C2, verbatim.
    QVERIFY2(findVisibleTextItem(splash.data(), QStringLiteral("Signing you in…")),
             "the restore splash must show copy.md's line");

    // No form, no field, no button, and no balance element (there is no balance before sign-in).
    for (QObject* item : splash->findChildren<QObject*>()) {
        QVERIFY2(!item->inherits("QQuickButton"), "the splash has no action");
        QVERIFY2(!item->inherits("QQuickTextInput"), "the splash has no field");
    }
    QVERIFY(!splash->findChild<QObject*>(QStringLiteral("balancePill")));
    for (const QString& formText : { QStringLiteral("Enter your phone number"),
                                     QStringLiteral("Continue"), QStringLiteral("Sign in") }) {
        QVERIFY2(!findTextItem(splash.data(), formText),
                 qPrintable(QStringLiteral("the splash must not carry sign-in copy: ") + formText));
    }
}

void TstUiScreens::theShellRoutesTheRestoreStateToTheSplashNotToSignIn()
{
    // `main.qml` instantiates the real SeatHubClient, which only the application registers, so it
    // is read as source - the same technique the window-sequence test uses. The invariant: the
    // state the facade is constructed in ("restoring") has its own route to the splash, because
    // `componentForState()` falls through to the sign-in form for any state it does not name, and
    // that fall-through is exactly the flash the splash exists to prevent.
    QFile file(guiDir() + QStringLiteral("/main.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "main.qml must be readable");
    const QString source = QString::fromUtf8(file.readAll());

    QVERIFY2(source.contains(QRegularExpression(
                 QStringLiteral("case\\s+\"restoring\"\\s*:\\s*return\\s+restoreComponent"))),
             "the restoring state must route to the restore splash");

    // The splash component is the splash and nothing else.
    const QRegularExpression component(
        QStringLiteral("id:\\s*restoreComponent\\s*(.*?)\\n    \\}"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = component.match(source);
    QVERIFY2(match.hasMatch(), "restoreComponent must exist in main.qml");
    QVERIFY(match.captured(1).contains(QStringLiteral("RestoreSplash")));
    QVERIFY2(!match.captured(1).contains(QStringLiteral("SignInScreen")),
             "the restore route must never instantiate the sign-in form");

    // The facade leaves that state, and main.qml starts the restore exactly once.
    QCOMPARE(source.count(QStringLiteral("seatHub.restoreSession()")), 1);

    // The splash and the pill are compiled in.
    QFile qrc(guiDir() + QStringLiteral("/../qml.qrc"));
    QVERIFY(qrc.open(QIODevice::ReadOnly));
    const QString qrcText = QString::fromUtf8(qrc.readAll());
    QVERIFY(qrcText.contains(QStringLiteral("gui/RestoreSplash.qml")));
    QVERIFY(qrcText.contains(QStringLiteral("gui/BalancePill.qml")));
}

void TstUiScreens::signInScreenReachesThePhoneStepError()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("SignInScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // The one field is what is on screen before a code is ever sent (Phase 5: the field takes an
    // email or a phone number, so its label is no longer "Enter your phone number").
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Email or phone")),
             "the identifier step is the first thing sign-in shows");
    QVERIFY2(!findVisibleTextItem(screen.data(), QStringLiteral("Enter the 6-digit code")),
             "the code step must stay hidden until the control plane says it sent a code");

    // Audit F2: a rejected *phone number* used to print nothing, because the only error
    // Text lived inside the code step. It must now be visible without a code step.
    const QString rejected = QStringLiteral("That code didn't match. Try again or resend.");
    client.deliverOtpRejected(rejected, QStringLiteral("SH-4F7KQ2"));

    QVERIFY2(findVisibleTextItem(screen.data(), rejected),
             "a rejected phone number must show the control plane's reason (audit F2)");
    QObject* codeItem = findVisibleTextItem(screen.data(), QStringLiteral("SH-4F7KQ2"));
    QVERIFY2(codeItem, "the ADR-0008 reference must be rendered");
    QCOMPARE(codeItem->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QVERIFY2(!findVisibleTextItem(screen.data(), QStringLiteral("Enter the 6-digit code")),
             "the code step still must not appear on a failed phone step");
}

void TstUiScreens::signInScreenCountsDownToResend()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("SignInScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    QVERIFY(screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    client.deliverOtpRequested(QStringLiteral("+962700000000"));

    // copy.md §Sign in, path B: `Resend in 0:24` - ADR-0022's 24-second window.
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Resend in 0:24")),
             "the code step must offer the spec's resend countdown (audit F6)");

    // The countdown is mono and tabular, like every other number in the shell.
    QObject* countdown = findVisibleTextItem(screen.data(), QStringLiteral("Resend in 0:24"));
    QCOMPARE(countdown->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));

    // The resend control itself is a real button, not a text gesture (audit F3).
    bool sawResendButton = false;
    for (QObject* item : screen->findChildren<QObject*>()) {
        if (item->inherits("QQuickButton")
            && item->property("text").toString() == QStringLiteral("Send code")) {
            sawResendButton = true;
            break;
        }
    }
    QVERIFY2(sawResendButton, "resending must be a real control, not a MouseArea on a Text");
}

// ------------------------------------------------------------------------------------------------
// Phase 5 plan 06: one field for an email or a phone number, its country tag and picker
// ------------------------------------------------------------------------------------------------

namespace {

// TextInput.Normal and TextInput.Password (`QQuickTextInput::EchoMode`).
const int kEchoNormal = 0;
const int kEchoPassword = 2;

// A loaded identifier field with the bundled list, the machine's-region default and no animation.
struct FieldFixture
{
    QQmlEngine engine;
    QScopedPointer<QObject> field;
    QString error;

    bool load()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerTokenSingletons(&engine);
        field.reset(instantiate(&engine, QStringLiteral("SeatHubIdentifierField.qml"), &error));
        if (!field) {
            return false;
        }
        field->setProperty("animate", false);
        field->setProperty("countries", QVariant::fromValue(bundledCountries()));
        field->setProperty("defaultCountryCode", QStringLiteral("JO"));
        return true;
    }

    void type(const QString& text) { field->setProperty("text", text); }
    QString mode() const { return field->property("mode").toString(); }
    bool tagShown() const { return field->property("tagShown").toBool(); }
    QString iso() const { return field->property("countryIso").toString(); }
    QString dial() const { return field->property("countryDial").toString(); }
    double reveal() const { return field->property("reveal").toDouble(); }
};

} // namespace

void TstUiScreens::identifierFieldShowsTheTagAtThreeDigitsAndHidesItOnALetter()
{
    FieldFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    // Nothing typed: no tag, no mode.
    QCOMPARE(fx.mode(), QStringLiteral("empty"));
    QVERIFY(!fx.tagShown());
    QCOMPARE(fx.reveal(), 0.0);

    // One and two digits are a number being typed, but not yet enough for the tag (screens.md §22).
    fx.type(QStringLiteral("07"));
    QCOMPARE(fx.mode(), QStringLiteral("partial"));
    QVERIFY(!fx.tagShown());

    // Three digits: the tag appears, on the machine's region by default.
    fx.type(QStringLiteral("079"));
    QCOMPARE(fx.mode(), QStringLiteral("phone"));
    QVERIFY(fx.tagShown());
    QCOMPARE(fx.reveal(), 1.0);
    QCOMPARE(fx.iso(), QStringLiteral("JO"));
    QCOMPARE(fx.dial(), QStringLiteral("+962"));

    // Separators and a leading plus do not change what it is; Arabic-Indic digits are read as digits.
    fx.type(QStringLiteral("(079) 000-00.00"));
    QVERIFY(fx.tagShown());
    fx.type(QString::fromUtf16(u"\u0660\u0667\u0669"));
    QCOMPARE(fx.mode(), QStringLiteral("phone"));
    QVERIFY(fx.tagShown());

    // A letter hides it at once, and so does an at-sign.
    fx.type(QStringLiteral("079a"));
    QCOMPARE(fx.mode(), QStringLiteral("email"));
    QVERIFY(!fx.tagShown());
    QCOMPARE(fx.reveal(), 0.0);

    fx.type(QStringLiteral("079"));
    QVERIFY(fx.tagShown());
    fx.type(QStringLiteral("079@"));
    QCOMPARE(fx.mode(), QStringLiteral("email"));
    QVERIFY(!fx.tagShown());

    // An email as a whole is an email, and never grows a tag.
    fx.type(QStringLiteral("lina@example.com"));
    QCOMPARE(fx.mode(), QStringLiteral("email"));
    QVERIFY(!fx.tagShown());
    QVERIFY(fx.field->property("looksLikeEmail").toBool());

    // Back to nothing: the tag goes with the text.
    fx.type(QString());
    QVERIFY(!fx.tagShown());
}

void TstUiScreens::identifierFieldNamesTheCountryANumberWritesForItself()
{
    FieldFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    // A leading plus wins over the tag, and the tag then shows the country the number matched.
    fx.type(QStringLiteral("+447911123456"));
    QVERIFY(fx.tagShown());
    QCOMPARE(fx.iso(), QStringLiteral("GB"));
    QCOMPARE(fx.dial(), QStringLiteral("+44"));
    // ...without changing the country the customer picked.
    QCOMPARE(fx.field->property("countryCode").toString(), QStringLiteral("JO"));

    // So does the international prefix written as a double zero.
    fx.type(QStringLiteral("00966501234567"));
    QCOMPARE(fx.iso(), QStringLiteral("SA"));

    // The longest dial code is the match: `+1876` is Jamaica, not the `+1` of Canada or the US.
    fx.type(QStringLiteral("+18765550123"));
    QCOMPARE(fx.iso(), QStringLiteral("JM"));
    fx.type(QStringLiteral("+14155550123"));
    QCOMPARE(fx.dial(), QStringLiteral("+1"));

    // A number with no prefix is the picked country's.
    fx.type(QStringLiteral("0790000000"));
    QCOMPARE(fx.iso(), QStringLiteral("JO"));
}

void TstUiScreens::identifierFieldKeepsTheDigitsWhenTheCountryChanges()
{
    FieldFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    fx.type(QStringLiteral("0790000000"));
    QCOMPARE(fx.iso(), QStringLiteral("JO"));

    QVERIFY(QMetaObject::invokeMethod(fx.field.data(), "setCountry", Q_ARG(QVariant, QStringLiteral("GB"))));

    QCOMPARE(fx.field->property("text").toString(), QStringLiteral("0790000000"));
    QCOMPARE(fx.iso(), QStringLiteral("GB"));
    QCOMPARE(fx.dial(), QStringLiteral("+44"));
    // The dial code a national number is completed with follows the tag.
    QCOMPARE(fx.field->property("selectedDial").toString(), QStringLiteral("+44"));
    QVERIFY(fx.tagShown());
}

void TstUiScreens::aLongEmailScrollsInsideTheFieldInsteadOfResizingIt()
{
    FieldFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    QObject* input = fx.field->findChild<QObject*>(QStringLiteral("identifierInput"));
    QObject* frame = fx.field->findChild<QObject*>(QStringLiteral("identifierFrame"));
    QVERIFY(input && frame);

    fx.type(QStringLiteral("lina@example.com"));
    const double fieldWidth = fx.field->property("width").toDouble();
    const double fieldHeight = fx.field->property("height").toDouble();
    const double frameWidth = frame->property("width").toDouble();
    const double inputWidth = input->property("width").toDouble();
    QVERIFY(inputWidth > 0);

    fx.type(QString(200, QLatin1Char('a')) + QStringLiteral("@example.com"));
    QCOMPARE(fx.field->property("width").toDouble(), fieldWidth);
    QCOMPARE(fx.field->property("height").toDouble(), fieldHeight);
    QCOMPARE(frame->property("width").toDouble(), frameWidth);
    QCOMPARE(input->property("width").toDouble(), inputWidth);
    // The text is wider than the box it sits in, so it scrolls there: it does not wrap either.
    QVERIFY(input->property("contentWidth").toDouble() > inputWidth);
    QCOMPARE(input->property("clip").toBool(), true);
}

void TstUiScreens::theTagRevealsThroughOpacityAndATransformNeverALayoutProperty()
{
    // The motion contract (`ui.md` §6, screens.md §22): nothing animates a width, an x or a padding.
    // QML cannot be measured frame by frame in a test, so the source is what is read: every
    // animation, and every Behavior, in the three sign-in files.
    const QStringList files = { QStringLiteral("SeatHubIdentifierField.qml"),
                                QStringLiteral("SeatHubCountryPicker.qml"),
                                QStringLiteral("SignInScreen.qml") };
    const QRegularExpression layoutProperty(
        QStringLiteral("(?:width|height|x|y|implicitWidth|implicitHeight|padding|leftPadding|"
                       "rightPadding|topPadding|bottomPadding|leftMargin|rightMargin|spacing)"));
    const QRegularExpression behaviorOn(QStringLiteral("Behavior\\s+on\\s+([A-Za-z_.]+)"));
    const QRegularExpression animatedProperty(
        QStringLiteral("(?:Number|Property|Parallel|Sequential)?Animation\\s+on\\s+([A-Za-z_.]+)"));
    const QRegularExpression propertyList(
        QStringLiteral("\\bproperties?\\s*:\\s*\"([^\"]+)\""));

    bool sawReveal = false;
    for (const QString& file : files) {
        const QString source = readSource(guiDir() + QLatin1Char('/') + file);
        QVERIFY2(!source.isEmpty(), qPrintable(file));

        for (const QRegularExpression& re : { behaviorOn, animatedProperty }) {
            QRegularExpressionMatchIterator it = re.globalMatch(source);
            while (it.hasNext()) {
                const QString property = it.next().captured(1);
                QVERIFY2(!QRegularExpression(QStringLiteral("^(?:") + layoutProperty.pattern()
                                             + QStringLiteral(")$")).match(property).hasMatch(),
                         qPrintable(file + QStringLiteral(" animates the layout property ") + property));
                if (property == QLatin1String("reveal")) {
                    sawReveal = true;
                }
            }
        }
        QRegularExpressionMatchIterator named = propertyList.globalMatch(source);
        while (named.hasNext()) {
            const QString list = named.next().captured(1);
            for (const QString& property : list.split(QLatin1Char(','))) {
                QVERIFY2(!QRegularExpression(QStringLiteral("^(?:") + layoutProperty.pattern()
                                             + QStringLiteral(")$")).match(property.trimmed()).hasMatch(),
                         qPrintable(file + QStringLiteral(" animates the layout property ") + property));
            }
        }
    }
    QVERIFY2(sawReveal, "the tag's reveal is the one animated value");

    // The reveal drives opacity and a transform, and the animation is 180ms ease-out.
    const QString field = readSource(guiDir() + QStringLiteral("/SeatHubIdentifierField.qml"));
    QVERIFY(field.contains(QStringLiteral("opacity: root.reveal")));
    QVERIFY(field.contains(QStringLiteral("Translate { x: (1 - root.reveal) * -8 }")));
    QVERIFY(field.contains(QStringLiteral("duration: Metrics.motionBase")));
    QVERIFY(field.contains(QStringLiteral("Easing.OutCubic")));
    // With Windows' animation effects off there is no transition.
    QVERIFY(field.contains(QStringLiteral("enabled: root.animate")));
    // The tag is a real button that reads its own country aloud.
    QVERIFY(field.contains(QRegularExpression(QStringLiteral("Button\\s*\\{\\s*id:\\s*tagButton"))));
    QVERIFY(field.contains(QStringLiteral("Accessible.name: qsTr(\"Country code: %1\").arg(root.country.name)")));
}

void TstUiScreens::countryPickerSearchesByNameOrDialDigitsAndSaysSoWhenNothingMatches()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> picker(instantiate(&engine, QStringLiteral("SeatHubCountryPicker.qml"), &error));
    QVERIFY2(picker, qPrintable(error));
    const QVariantList countries = bundledCountries();
    QVERIFY(countries.size() > 100);
    picker->setProperty("model", QVariant::fromValue(countries));

    auto matched = [&picker](const QString& query) {
        picker->setProperty("query", query);
        return picker->property("matchedIsoList").toString().split(QLatin1Char(','),
                                                                    Qt::SkipEmptyParts);
    };

    // Nothing typed: every country.
    QCOMPARE(matched(QString()).size(), countries.size());
    QVERIFY(!picker->property("noMatches").toBool());

    // By name, anywhere in it, in any case.
    QVERIFY(matched(QStringLiteral("jord")).contains(QStringLiteral("JO")));
    QVERIFY(matched(QStringLiteral("KINGDOM")).contains(QStringLiteral("GB")));
    {
        const QStringList united = matched(QStringLiteral("united"));
        QCOMPARE(QSet<QString>(united.begin(), united.end()).size(), 3);
    }
    // // Arab Emirates, Kingdom, States

    // By the digits a dial code starts with: `962`, `+962` and `00962` all mean the same.
    for (const QString& query : { QStringLiteral("962"), QStringLiteral("+962"), QStringLiteral("00962") }) {
        const QStringList hits = matched(query);
        QVERIFY2(hits.contains(QStringLiteral("JO")), qPrintable(query));
        QVERIFY2(hits.size() < 10, qPrintable(query));
    }
    QVERIFY(matched(QStringLiteral("+44")).contains(QStringLiteral("GB")));
    // A dial-code query matches from the start of the code, not from its middle.
    QVERIFY(!matched(QStringLiteral("62")).contains(QStringLiteral("JO")));

    // Nothing matches: a sentence says so, and the list is gone.
    QVERIFY(matched(QStringLiteral("zzzzzz")).isEmpty());
    QVERIFY(picker->property("noMatches").toBool());
    QObject* line = picker->findChild<QObject*>(QStringLiteral("noMatchLine"));
    QVERIFY(line);
    QCOMPARE(line->property("text").toString(), QStringLiteral("No country matches that."));
    QVERIFY(line->property("visible").toBool());
    QObject* list = picker->findChild<QObject*>(QStringLiteral("countryList"));
    QVERIFY(list);
    QVERIFY(!list->property("visible").toBool());

    // And the line is quiet again once something matches.
    QVERIFY(!matched(QStringLiteral("jordan")).isEmpty());
    QVERIFY(!line->property("visible").toBool());
}

void TstUiScreens::countryPickerHighlightsTheCountryInForceAndPicksIt()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> picker(instantiate(&engine, QStringLiteral("SeatHubCountryPicker.qml"), &error));
    QVERIFY2(picker, qPrintable(error));
    picker->setProperty("model", QVariant::fromValue(bundledCountries()));
    picker->setProperty("selectedIso", QStringLiteral("SA"));

    QSignalSpy picked(picker.data(), SIGNAL(picked(QString)));
    QSignalSpy dismissed(picker.data(), SIGNAL(dismissed()));

    // Opening puts the highlight on the country in force: choosing at once picks it.
    QVERIFY(QMetaObject::invokeMethod(picker.data(), "reset"));
    QVERIFY(QMetaObject::invokeMethod(picker.data(), "pickCurrent"));
    QCOMPARE(picked.count(), 1);
    QCOMPARE(picked.first().first().toString(), QStringLiteral("SA"));

    // A search leaves the first match highlighted, so Enter picks it.
    picker->setProperty("query", QStringLiteral("jord"));
    QObject* search = picker->findChild<QObject*>(QStringLiteral("countrySearch"));
    QVERIFY(search);
    search->setProperty("text", QStringLiteral("jord"));
    QVERIFY(QMetaObject::invokeMethod(picker.data(), "pickCurrent"));
    QCOMPARE(picked.count(), 2);
    QCOMPARE(picked.last().first().toString(), QStringLiteral("JO"));

    // Escape asks the host to close the popup; it picks nothing.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(search, &press);
    QCOMPARE(dismissed.count(), 1);
    QCOMPARE(picked.count(), 2);
}

void TstUiScreens::countryPickerReadsOnlyTheBundledListAndElidesLongNames()
{
    const QString source = readSource(guiDir() + QStringLiteral("/SeatHubCountryPicker.qml"));
    QVERIFY(!source.isEmpty());

    // It fetches nothing: no network object, no address, no include.
    for (const QString& forbidden : { QStringLiteral("XMLHttpRequest"), QStringLiteral("http://"),
                                      QStringLiteral("https://"), QStringLiteral("WebSocket"),
                                      QStringLiteral("Qt.include"), QStringLiteral("fetch(") }) {
        QVERIFY2(!source.contains(forbidden), qPrintable(forbidden));
    }
    // Its data is the model it is given, and the field gives it the facade's bundled list.
    QVERIFY(source.contains(QStringLiteral("property var model")));
    const QString field = readSource(guiDir() + QStringLiteral("/SeatHubIdentifierField.qml"));
    QVERIFY(field.contains(QStringLiteral("model: root.countries")));
    QVERIFY(readSource(guiDir() + QStringLiteral("/SignInScreen.qml"))
                .contains(QStringLiteral("root.client.countries")));

    // A long name elides in its row, and the whole name is the row's accessible name.
    QVERIFY(source.contains(QStringLiteral("elide: Text.ElideRight")));
    QVERIFY(source.contains(QStringLiteral("Accessible.name: row.modelData.name + \", \" + row.modelData.dial")));
    // The list is at most 256px and scrolls inside itself.
    QVERIFY(source.contains(QStringLiteral("height: Math.min(contentHeight, 256)")));
    QVERIFY(source.contains(QStringLiteral("clip: true")));
    // Escape closes it.
    QVERIFY(source.contains(QStringLiteral("Keys.onEscapePressed: root.dismissed()")));

    // And the data really is bundled: the resource file the app links, with the list in it.
    const QString countries = readSource(guiDir() + QStringLiteral("/../seathub/countries.qrc"));
    QVERIFY(countries.contains(QStringLiteral("countries.json")));
    const QString app = readSource(guiDir() + QStringLiteral("/../app.pro"));
    QVERIFY(app.contains(QStringLiteral("seathub/countries.qrc")));
}

namespace {

// The sign-in screen with a stand-in facade, the field's animation off, ready to type into.
struct SignInFixture
{
    QQmlEngine engine;
    FakeShellClient client;
    QScopedPointer<QObject> screen;
    QString error;

    bool load()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerTokenSingletons(&engine);
        screen.reset(instantiate(&engine, QStringLiteral("SignInScreen.qml"), &error));
        if (!screen) {
            return false;
        }
        screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client)));
        // A window-sized parent, so the card has a width to be measured against.
        screen->setProperty("width", 960);
        screen->setProperty("height", 640);
        return true;
    }

    QObject* child(const char* name) const { return screen->findChild<QObject*>(QLatin1String(name)); }
    QObject* field() const { return child("identifierField"); }
    void type(const QString& text) { field()->setProperty("text", text); }
    void call(const char* method) { QVERIFY(QMetaObject::invokeMethod(screen.data(), method)); }
    void click(const char* objectName)
    {
        QObject* button = child(objectName);
        QVERIFY2(button, objectName);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }
    QString step() const { return screen->property("step").toString(); }
    bool sees(const QString& text) const { return findVisibleTextItem(screen.data(), text) != nullptr; }
};

} // namespace

void TstUiScreens::signInRoutesAPhoneToTheCodeStepAndKeepsTheNumberOnBack()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    QVERIFY(fx.field());
    QCOMPARE(fx.step(), QStringLiteral("identifier"));
    QVERIFY(fx.sees(QStringLiteral("Email or phone")));
    QVERIFY(fx.sees(QStringLiteral(
        "Enter an email address, or pick your country and enter your phone number.")));
    QVERIFY(fx.sees(QStringLiteral("Continue")));

    // A number typed the way a customer in Amman writes it: the code is asked for, as E.164.
    fx.type(QStringLiteral("0790000000"));
    fx.call("continueFromIdentifier");
    QCOMPARE(fx.client.otpRequests(), QStringList{ QStringLiteral("+962790000000") });
    // The screen waits for the server; the step has not changed yet, and the control is busy.
    QCOMPARE(fx.step(), QStringLiteral("identifier"));
    QVERIFY(fx.child("primaryAction")->property("busy").toBool());

    fx.client.deliverOtpRequested(QStringLiteral("+962790000000"));
    QCOMPARE(fx.step(), QStringLiteral("code"));
    QVERIFY(fx.sees(QStringLiteral("Enter the 6-digit code")));
    QVERIFY(fx.sees(QStringLiteral("Verify and continue")));
    QVERIFY(!fx.sees(QStringLiteral("Continue")));
    QVERIFY(!fx.child("primaryAction")->property("busy").toBool());

    // Back returns to the field with what was typed still in it.
    fx.click("backButton");
    QCOMPARE(fx.step(), QStringLiteral("identifier"));
    QCOMPARE(fx.field()->property("text").toString(), QStringLiteral("0790000000"));
    QVERIFY(fx.field()->property("tagShown").toBool());

    // A number typed with its own country code is believed over the tag.
    fx.type(QStringLiteral("+447911123456"));
    fx.call("continueFromIdentifier");
    QCOMPARE(fx.client.otpRequests().last(), QStringLiteral("+447911123456"));
}

void TstUiScreens::signInRoutesAnEmailToThePasswordStep()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    fx.type(QStringLiteral("lina@example.com"));
    fx.call("continueFromIdentifier");

    // No code is asked for and nothing is sent: the password step is the next thing.
    QVERIFY(fx.client.otpRequests().isEmpty());
    QCOMPARE(fx.step(), QStringLiteral("password"));
    QVERIFY(fx.sees(QStringLiteral("Password")));
    QVERIFY(fx.sees(QStringLiteral("Sign in")));
    QVERIFY(fx.sees(QStringLiteral("Show password")));
    QVERIFY(!fx.sees(QStringLiteral("Enter the 6-digit code")));

    // The password is hidden until the reveal control is used, and the control says what it does.
    QObject* password = fx.child("passwordField");
    QVERIFY(password);
    QCOMPARE(password->property("echoMode").toInt(), kEchoPassword);
    fx.click("revealPassword");
    QCOMPARE(password->property("echoMode").toInt(), kEchoNormal);
    QVERIFY(fx.sees(QStringLiteral("Hide password")));
    fx.click("revealPassword");
    QCOMPARE(password->property("echoMode").toInt(), kEchoPassword);

    // Sign in sends the identifier as typed and the password.
    password->setProperty("text", QStringLiteral("correct horse battery"));
    fx.call("signInWithPassword");
    QCOMPARE(fx.client.passwordAttempts().size(), 1);
    QCOMPARE(fx.client.passwordAttempts().first().first, QStringLiteral("lina@example.com"));
    QCOMPARE(fx.client.passwordAttempts().first().second, QStringLiteral("correct horse battery"));
    QVERIFY(fx.child("primaryAction")->property("busy").toBool());

    // Accepted: the screen reports it, and the password is not kept.
    QSignalSpy signedIn(fx.screen.data(), SIGNAL(signedIn()));
    fx.client.deliverPasswordAccepted();
    QCOMPARE(signedIn.count(), 1);
    QVERIFY(password->property("text").toString().isEmpty());

    // Back from the password step keeps the email, drops the password and hides it again.
    fx.click("backButton");
    QCOMPARE(fx.step(), QStringLiteral("identifier"));
    QCOMPARE(fx.field()->property("text").toString(), QStringLiteral("lina@example.com"));
    QVERIFY(password->property("text").toString().isEmpty());
    QVERIFY(!fx.field()->property("tagShown").toBool());
}

void TstUiScreens::signInNamesAFieldLevelMistakeBeforeAnythingIsSent()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    // Nothing typed.
    fx.call("continueFromIdentifier");
    QVERIFY(fx.sees(QStringLiteral("Enter your email or phone number.")));

    // Too few digits to be a number.
    fx.type(QStringLiteral("12"));
    fx.call("continueFromIdentifier");
    QVERIFY(fx.sees(QStringLiteral("That doesn't look like a phone number.")));
    QVERIFY(!fx.sees(QStringLiteral("Enter your email or phone number.")));

    // Something that is not an address.
    fx.type(QStringLiteral("lina@"));
    fx.call("continueFromIdentifier");
    QVERIFY(fx.sees(QStringLiteral("Enter your email or phone number.")));

    // A blank password, on the password step.
    fx.type(QStringLiteral("lina@example.com"));
    fx.call("continueFromIdentifier");
    QCOMPARE(fx.step(), QStringLiteral("password"));
    fx.call("signInWithPassword");
    QVERIFY(fx.sees(QStringLiteral("Enter your password.")));

    // None of it reached the facade.
    QVERIFY(fx.client.otpRequests().isEmpty());
    QVERIFY(fx.client.passwordAttempts().isEmpty());

    // And the message goes when the customer edits the field.
    fx.child("passwordField")->setProperty("text", QStringLiteral("x"));
    QVERIFY(!fx.sees(QStringLiteral("Enter your password.")));
}

void TstUiScreens::signInShowsARefusalVerbatimAndClearsThePassword()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    fx.type(QStringLiteral("lina@example.com"));
    fx.call("continueFromIdentifier");
    QObject* password = fx.child("passwordField");
    password->setProperty("text", QStringLiteral("hunter22hunter"));
    fx.call("signInWithPassword");

    const QString sentence = QString::fromUtf16(u"That account is disabled \u2014 contact support.");
    fx.client.deliverPasswordRejected(sentence, QStringLiteral("SH-5M8NP3"));

    QVERIFY2(fx.sees(sentence), "the server's own sentence, word for word");
    QObject* reference = findVisibleTextItem(fx.screen.data(), QStringLiteral("SH-5M8NP3"));
    QVERIFY2(reference, "the ADR-0008 reference must be shown");
    QCOMPARE(reference->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    // The password is held for one attempt only, and the control is usable again.
    QVERIFY(password->property("text").toString().isEmpty());
    QVERIFY(!fx.child("primaryAction")->property("busy").toBool());
    QVERIFY(fx.child("primaryAction")->property("enabled").toBool());

    // The offline sentence is a refusal like any other.
    const QString offline = QStringLiteral("We couldn't reach SevenHills. Try again in a moment.");
    fx.client.deliverPasswordRejected(offline, QString());
    QVERIFY(fx.sees(offline));
    QVERIFY(!fx.sees(sentence));
}

void TstUiScreens::signInKeepsTheWidthOfTheBusyControl()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    QObject* primary = fx.child("primaryAction");
    QVERIFY(primary);
    const double idleWidth = primary->property("width").toDouble();
    QVERIFY(idleWidth > 0);
    QVERIFY(primary->property("enabled").toBool());

    fx.type(QStringLiteral("0790000000"));
    fx.call("continueFromIdentifier");
    QVERIFY(primary->property("busy").toBool());
    QVERIFY(!primary->property("enabled").toBool());
    QCOMPARE(primary->property("width").toDouble(), idleWidth);
    // The label stays put while it is busy: the width does not move because the text does not.
    QCOMPARE(primary->property("text").toString(), QStringLiteral("Continue"));
}

void TstUiScreens::signInOpensTheWebsiteForSignupAndResetAndHasNoScreenOfItsOwn()
{
    SignInFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    QVERIFY(fx.sees(QStringLiteral("New here? Create an account")));
    QVERIFY(fx.sees(QStringLiteral("Forgot password?")));

    fx.click("createAccountLink");
    fx.click("forgotPasswordLink");
    // The screen asks by name; the facade owns the addresses, and no credential goes with either.
    QCOMPARE(fx.client.opened(), (QStringList{ QStringLiteral("signup"), QStringLiteral("reset") }));

    // The reset link is on the password step too.
    fx.type(QStringLiteral("lina@example.com"));
    fx.call("continueFromIdentifier");
    fx.click("forgotPasswordLink");
    QCOMPARE(fx.client.opened().last(), QStringLiteral("reset"));

    // Signup and reset are the website's: the client has no screen for either.
    const QDir gui(guiDir());
    for (const QString& name : gui.entryList({ QStringLiteral("*.qml") })) {
        for (const QString& retired : { QStringLiteral("signup"), QStringLiteral("register"),
                                        QStringLiteral("forgot"), QStringLiteral("reset") }) {
            QVERIFY2(!name.contains(retired, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("the client has no ") + retired + QStringLiteral(" screen: ") + name));
        }
    }
    const QString main = readSource(guiDir() + QStringLiteral("/main.qml"));
    QVERIFY(!main.contains(QStringLiteral("SignUp"), Qt::CaseInsensitive));
}

void TstUiScreens::noSeatHubScreenBuildsAWebsiteAddressItself()
{
    // T-05-25: a link is built in one place (`web_origin.h`, joined by `SeatHubClient::websiteUrl`),
    // so it cannot end up carrying anything it should not. No SeatHub-authored QML file may hold an
    // address. The three upstream Moonlight files that do (a docs link and a store link, none of
    // them a SeatHub screen) are not this client's and are left as they are.
    const QStringList upstream = { QStringLiteral("AutoResizingComboBox.qml"),
                                   QStringLiteral("NavigableMessageDialog.qml"),
                                   QStringLiteral("PcView.qml") };
    const QDir gui(guiDir());
    int checked = 0;
    for (const QString& name : gui.entryList({ QStringLiteral("*.qml") })) {
        if (upstream.contains(name)) {
            continue;
        }
        ++checked;
        const QString source = readSource(gui.filePath(name));
        QVERIFY2(!source.contains(QStringLiteral("https://")), qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("http://")), qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("sevenhills.damra.co")), qPrintable(name));
    }
    QVERIFY(checked > 10);
}

void TstUiScreens::forcedUpdateModalKeepsUpdateTabbableAndUndismissable()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeUpdates updates;
    updates.offerUpdate();

    QString error;
    QScopedPointer<QObject> modal(
        instantiate(&engine, QStringLiteral("ForcedUpdateModal.qml"), &error));
    QVERIFY2(modal, qPrintable(error));
    QVERIFY(modal->setProperty("updates", QVariant::fromValue(static_cast<QObject*>(&updates))));

    QVERIFY2(modal->property("shown").toBool(), "an offered update must show the modal");

    // Exactly one action, and it is tabbable: the audit's F4 was a keyboard that could
    // never reach `Update` because the card swallowed every key.
    QList<QObject*> buttons;
    QList<QObject*> tabbables;
    for (QObject* item : modal->findChildren<QObject*>()) {
        if (item->inherits("QQuickButton")) {
            buttons.append(item);
        }
        if (item->property("activeFocusOnTab").toBool()) {
            tabbables.append(item);
        }
    }
    QCOMPARE(buttons.size(), 1);
    QObject* action = buttons.first();
    QVERIFY2(action->property("text").toString() == QStringLiteral("Update"),
             "the one action is Update");
    QVERIFY2(action->property("activeFocusOnTab").toBool(),
             "Update must be reachable by Tab (audit F4)");
    QCOMPARE(tabbables.size(), 1);
    QCOMPARE(tabbables.first(), action);

    // And nothing that could dismiss it: D-41 keeps every customer out of a streaming
    // session on a stale build, so no Cancel / Later / Close affordance may exist, and the
    // card owns the whole window while it is up.
    const QStringList forbidden = {QStringLiteral("Cancel"), QStringLiteral("Later"),
                                   QStringLiteral("Close"), QStringLiteral("Dismiss"),
                                   QStringLiteral("Skip"), QStringLiteral("Not now")};
    for (const QString& text : renderedTexts(modal.data())) {
        for (const QString& word : forbidden) {
            QVERIFY2(!text.contains(word, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("the forced-update modal must not offer '%1'")
                                    .arg(text)));
        }
    }

    bool swallowsClicks = false;
    for (QObject* item : modal->findChildren<QObject*>()) {
        if (item->inherits("QQuickMouseArea")
            && item->property("acceptedButtons").toInt() == int(Qt::AllButtons)) {
            swallowsClicks = true;
            break;
        }
    }
    QVERIFY2(swallowsClicks, "the modal must keep swallowing clicks aimed at the page behind it");

    // The card is the root of the focus ring while it is shown; its Keys handler answers
    // Tab by moving focus inside the card (see the QML), so the ring cannot escape.
    QVERIFY2(modal->property("focus").toBool(), "the modal owns focus while it is shown");
}

void TstUiScreens::settingsDropdownElidesLongOptionNames()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeBridge bridge;
    QString error;
    QScopedPointer<QObject> row(instantiate(&engine, QStringLiteral("SeatHubSelect.qml"), &error));
    QVERIFY2(row, qPrintable(error));
    QVERIFY(row->setProperty("bridge", QVariant::fromValue(static_cast<QObject*>(&bridge))));
    row->setProperty("settingKey", QStringLiteral("videocodec"));
    row->setProperty("title", QStringLiteral("Video codec"));

    QObject* dropdown = nullptr;
    for (QObject* item : row->findChildren<QObject*>()) {
        if (item->inherits("QQuickComboBox")) {
            dropdown = item;
            break;
        }
    }
    QVERIFY2(dropdown, "the row must render a real ComboBox");

    // E9: the popup is pinned to the control's width - it may not widen the page to fit the
    // longest codec name.
    QObject* popup = dropdown->property("popup").value<QObject*>();
    QVERIFY2(popup, "the ComboBox must have a popup");
    QCOMPARE(popup->property("width").toReal(), dropdown->property("width").toReal());

    // And the popup's own delegate elides, exactly as the closed control does.
    QQmlComponent* delegate = dropdown->property("delegate").value<QQmlComponent*>();
    QVERIFY2(delegate, "the row must supply its own popup delegate");
    QQmlContext context(qmlContext(dropdown));
    context.setContextProperty(QStringLiteral("modelData"),
                               QStringLiteral("Hardware HEVC (a deliberately long decoder name)"));
    context.setContextProperty(QStringLiteral("index"), 0);
    QScopedPointer<QObject> delegateItem(delegate->create(&context));
    QVERIFY2(delegateItem, qPrintable(delegate->errorString()));

    QObject* label = delegateItem->property("contentItem").value<QObject*>();
    QVERIFY2(label, "the popup delegate must draw its label through a contentItem");
    QCOMPARE(label->property("elide").toInt(), int(Qt::ElideRight));
    QVERIFY2(label->property("width").toReal() <= dropdown->property("width").toReal(),
             "the popup label must fit the popup width");
}

void TstUiScreens::disabledActionLabelStaysLegible()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> button(instantiate(&engine, QStringLiteral("SeatHubButton.qml"), &error));
    QVERIFY2(button, qPrintable(error));
    button->setProperty("text", QStringLiteral("Verify and continue"));
    button->setProperty("enabled", false);

    QObject* contentItem = button->property("contentItem").value<QObject*>();
    QObject* background = button->property("background").value<QObject*>();
    QVERIFY2(contentItem, "the button must draw its label through a contentItem");
    QVERIFY2(background, "the button must draw its own background");

    // The contentItem is a row (the label, plus the spinner while the button is busy), so the
    // label is the text item inside it.
    QObject* label = nullptr;
    for (QObject* child : contentItem->findChildren<QObject*>()) {
        if (child->property("text").isValid() && child->property("color").isValid()) {
            label = child;
            break;
        }
    }
    QVERIFY2(label, "the button must draw its label through a text item");

    const QColor labelColor = label->property("color").value<QColor>();
    // The fill animates in `--dur-fast` (ui.md §6), so the disabled colour arrives one
    // animation after the state change: read it the way it settles, and in doing so prove the
    // motion token is wired rather than merely declared.
    QTRY_COMPARE(background->property("color").value<QColor>(),
                 QColor(QStringLiteral("#262626")));   // surface3Default
    const QColor fillColor = background->property("color").value<QColor>();
    QCOMPARE(labelColor, QColor(QStringLiteral("#a3a3a3")));  // foregroundMutedDefault

    // ui.md §3.2: disabled text may drop to 3:1, and it must not drop below. The audit
    // measured ~1.19:1 for the old primary-foreground-on-surface-3 pair (F5).
    const double ratio = contrastRatio(labelColor, fillColor);
    QVERIFY2(ratio >= 3.0, qPrintable(QStringLiteral("disabled label contrast is %1:1")
                                          .arg(ratio, 0, 'f', 2)));

    // The focus ring is a real 2px `--focus` border on the control, from the tokens.
    QFile buttonFile(guiDir() + QStringLiteral("/SeatHubButton.qml"));
    QVERIFY2(buttonFile.open(QIODevice::ReadOnly), "SeatHubButton.qml must be readable");
    const QString source = QString::fromUtf8(buttonFile.readAll());
    QVERIFY2(source.contains(QStringLiteral("border.width: root.activeFocus ? 2 : 0")),
             "the button must draw the spec's 2px focus ring (ui.md §9, audit F14)");
    QVERIFY2(source.contains(QStringLiteral("Tokens.focusDefault")),
             "the focus ring must use the focus token, not a literal colour");
}

void TstUiScreens::agentConfigPanelMasksTheTokenAndNeverRendersIt()
{
    // The file the Node Agent writes, in the agents' own shape
    // (`seathub-host-agents/crates/node-agent/src/main.rs`).
    const QString path =
        QDir(QDir::tempPath()).filePath(QStringLiteral("seathub-agent-config.json"));
    const QString token = QStringLiteral("0123456789abcdefghijklmnopqrstuv");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QStringLiteral("{\"host_id\":\"4d5e6f70-1111-2222-3333-444455556666\","
                                  "\"token\":\"%1\","
                                  "\"base_url\":\"https://api-sevenhills.damra.co\"}")
                       .arg(token)
                       .toUtf8());
    }

    const QVariantMap described = AgentConfig::describe(QUrl::fromLocalFile(path));
    QCOMPARE(described.value(QStringLiteral("path")).toString(), path);
    QCOMPARE(described.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY2(!described.contains(QStringLiteral("token")),
             "the agent token must never cross into QML (D-30)");

    const QString masked = described.value(QStringLiteral("token_masked")).toString();
    QVERIFY2(!masked.isEmpty(), "a token that was found must come back masked");
    QVERIFY2(!masked.contains(token), "the mask must not contain the token");

    // The mask's shape is `agent_config.h`'s design: the first four characters, eight bullets, the
    // last four. Both four-character ends are shown on purpose - a support reader needs something
    // to match against - so the assertion is what is true of the design. It used to assert that the
    // token's last *eight* characters were absent, under a message claiming the mask hid the tail;
    // the design never claimed that, and the assertion passed only because eight characters is a
    // wider window than the mask reveals (defect F-11).
    QCOMPARE(masked, token.left(4) + QString(8, QChar(0x2022)) + token.right(4));

    // A file that is not there is an ordinary answer with a reason, and no invented code.
    const QVariantMap missing = AgentConfig::describe(QUrl::fromLocalFile(path + QStringLiteral(".gone")));
    QCOMPARE(missing.value(QStringLiteral("ok")).toBool(), false);
    QVERIFY(!missing.value(QStringLiteral("error")).toString().isEmpty());
    QVERIFY2(missing.value(QStringLiteral("token_masked")).toString().isEmpty(),
             "nothing was read, so nothing may be shown");
    QVERIFY2(!missing.value(QStringLiteral("error")).toString().contains(QStringLiteral("SH-")),
             "ADR-0008: the client does not invent reference codes");

    QFile::remove(path);

    // And the panel renders the path and the mask, and never the token itself.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> panel(instantiate(&engine, QStringLiteral("AgentConfigPanel.qml"), &error));
    QVERIFY2(panel, qPrintable(error));
    panel->setProperty("described", described);

    bool sawPath = false;
    bool sawMask = false;
    const QList<QObject*> children = panel->findChildren<QObject*>();
    for (QObject* child : children) {
        const QVariant textValue = child->property("text");
        if (!textValue.isValid()) {
            continue;
        }
        const QString text = textValue.toString();
        QVERIFY2(!text.contains(token),
                 "the agent token must never appear in a rendered string");
        if (text == path) {
            sawPath = true;
        }
        if (text.startsWith(QStringLiteral("Agent token"))) {
            sawMask = true;
            QVERIFY2(text.contains(masked), "the mask the panel shows is not the mask it read");
        }
    }
    QVERIFY2(sawPath, "the panel must show which file was read");
    QVERIFY2(sawMask, "the panel must show the masked agent token");
}

void TstUiScreens::theWindowSequenceRestoresOnlyAfterTheEngineIsDone()
{
    // D-01 (one visible window at a time), D-03 (restore only once SDL destruction is proven) and
    // Pitfall 1 (`quitStarting` must not restore) are the phase's most-quoted invariants - and no
    // automated check touched them. What existed was a hand-run grep in the plan's verify block and
    // a reading of the file. This reads the same file the grep read, and says exactly which handler
    // each visibility line is in.
    //
    // It is source analysis, not a render: it cannot show that the window sequence is pleasant to
    // watch, only that there is one place that hides and one place that restores, and that the
    // restore is not on a signal that fires while SDL is still alive.
    QFile file(guiDir() + QStringLiteral("/SessionSegue.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "SessionSegue.qml must be readable");
    const QString source = QString::fromUtf8(file.readAll());

    // One `function name() { ... }` per handler, and the file's bodies hold no nested braces, so a
    // body runs to the first closing brace on a line of its own. That closing brace is indented
    // (`\n    }`), which the first version of this pattern got wrong: it required `}` at column
    // zero, so every "body" ran to the end of the file and every assertion about which handler a
    // line lives in was quietly meaningless.
    const auto bodyOf = [&source](const QString& name) -> QString {
        const QRegularExpression re(
            QStringLiteral("function\\s+%1\\s*\\([^)]*\\)\\s*\\{(.*?)\\n\\s*\\}").arg(name),
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = re.match(source);
        return match.hasMatch() ? match.captured(1) : QString();
    };

    // D-01: the hide is on `connectionStarted`, which the engine emits before it creates its SDL
    // window (app/streaming/session.cpp).
    const QString connectionStarted = bodyOf(QStringLiteral("connectionStarted"));
    QVERIFY2(!connectionStarted.isEmpty(), "connectionStarted() must exist in SessionSegue.qml");
    QVERIFY2(connectionStarted.contains(QStringLiteral("window.visible = false")),
             "D-01: connectionStarted must hide the Qt window");

    // D-03 / Pitfall 1: the only restore is on `readyForDeletion`, and neither of the two signals
    // that fire while SDL is still alive may touch the window.
    const QString readyForDeletion = bodyOf(QStringLiteral("sessionReadyForDeletion"));
    QVERIFY2(!readyForDeletion.isEmpty(),
             "sessionReadyForDeletion() must exist in SessionSegue.qml");
    QVERIFY2(readyForDeletion.contains(QStringLiteral("window.visible = true")),
             "D-03: readyForDeletion is where the Qt window comes back");

    const QString quitStarting = bodyOf(QStringLiteral("quitStarting"));
    QVERIFY2(!quitStarting.isEmpty(), "quitStarting() must exist in SessionSegue.qml");
    QVERIFY2(!quitStarting.contains(QStringLiteral("visible")),
             "Pitfall 1: quitStarting fires before SDL destroys its window, so it must not "
             "restore the Qt window - that would put two windows on screen at once");

    const QString sessionFinished = bodyOf(QStringLiteral("sessionFinished"));
    QVERIFY2(!sessionFinished.isEmpty(), "sessionFinished() must exist in SessionSegue.qml");
    QVERIFY2(!sessionFinished.contains(QStringLiteral("visible")),
             "D-03: at sessionFinished SDL destruction is still pending, so no restore yet");

    // And there is exactly one of each, so "the window comes back" cannot be satisfied twice or
    // somewhere unnamed.
    QCOMPARE(source.count(QStringLiteral("window.visible = false")), 1);
    QCOMPARE(source.count(QStringLiteral("window.visible = true")), 1);

    // The forwarding handler must not hide or restore behind the named function's back.
    const QString onQuit = bodyOf(QStringLiteral("onQuitStarting"));
    QVERIFY2(!onQuit.contains(QStringLiteral("visible")),
             "the Qt signal handler for quitStarting must not touch the window either");
}

QTEST_MAIN(TstUiScreens)

namespace {

// Every item named `name` in `root`'s visual tree. A Repeater's delegates are children of the item
// they are laid out in, not of the Repeater in the object tree, so `findChildren` cannot see them.
void collectNamed(QQuickItem* item, const QString& name, QList<QObject*>* out)
{
    for (QQuickItem* child : item->childItems()) {
        if (child->objectName() == name) {
            out->append(child);
        }
        collectNamed(child, name, out);
    }
}

QList<QObject*> itemsNamed(QObject* root, const QString& name)
{
    QList<QObject*> found;
    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        collectNamed(item, name, &found);
    }
    return found;
}

// Every text any item in `root`'s visual tree prints, delegates included.
void collectTexts(QQuickItem* item, QStringList* out)
{
    for (QQuickItem* child : item->childItems()) {
        if (child->metaObject()->indexOfProperty("text") >= 0
                && child->metaObject()->indexOfProperty("font") >= 0) {
            out->append(child->property("text").toString());
        }
        collectTexts(child, out);
    }
}

QObject* itemNamed(QObject* root, const QString& name)
{
    const QList<QObject*> found = itemsNamed(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

// What each of the stepper's three stages looks like, in order, read from the stage rows themselves.
QStringList stepperLooks(QObject* stepper)
{
    QStringList looks;
    for (int index = 0; index < 3; ++index) {
        QObject* row = itemNamed(stepper, QStringLiteral("stage%1").arg(index));
        looks.append(row ? row->property("look").toString() : QStringLiteral("<missing>"));
    }
    return looks;
}

// The stage names the stepper prints, in order.
QStringList stepperNames(QObject* stepper)
{
    QStringList names;
    for (QObject* item : itemsNamed(stepper, QStringLiteral("stageName"))) {
        names.append(item->property("text").toString());
    }
    return names;
}

} // namespace

void TstUiScreens::theStepperDrawsThreeStagesFromTheFacadesStageAndNothingElse()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> stepper(instantiate(&engine, QStringLiteral("SeatHubStepper.qml"), &error));
    QVERIFY2(stepper, qPrintable(error));
    stepper->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client)));

    // Three stages, in the copy deck's own words: not five, and none of the two the deck retired.
    QCOMPARE(stepperNames(stepper.data()),
             (QStringList{QStringLiteral("Preparing the rig"), QStringLiteral("Preparing the stream"),
                          QStringLiteral("Streaming")}));
    QStringList everything;
    collectTexts(qobject_cast<QQuickItem*>(stepper.data()), &everything);
    QVERIFY2(everything.contains(QStringLiteral("Preparing the stream")),
             "the walk over the stepper's items must see its delegates");
    QVERIFY(!everything.contains(QStringLiteral("Waiting for a free rig")));
    QVERIFY(!everything.contains(QStringLiteral("Ready")));
    QVERIFY(!everything.contains(QStringLiteral("Open SeatHub and press Play.")));

    // Before the first answer about the session nothing is marked done: the first stage is simply
    // where a session that exists begins.
    client.setConnectStage(0);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("active"), QStringLiteral("pending"), QStringLiteral("pending")}));

    // Each stage the facade reaches makes the earlier ones done and itself the one live signal.
    client.setConnectStage(1);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("active"), QStringLiteral("pending"), QStringLiteral("pending")}));
    client.setConnectStage(2);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("done"), QStringLiteral("active"), QStringLiteral("pending")}));
    client.setConnectStage(3);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("done"), QStringLiteral("done"), QStringLiteral("active")}));

    // Only the active stage carries its sentence, and the stepper never has a stage of its own to
    // advance: with nothing set on it, it draws whatever the facade says and no more.
    client.setConnectStage(2);
    QObject* sentenceOfSecond = itemNamed(itemNamed(stepper.data(), QStringLiteral("stage1")),
                                          QStringLiteral("stageSentence"));
    QVERIFY(sentenceOfSecond);
    QVERIFY(effectivelyVisible(sentenceOfSecond));
    QCOMPARE(sentenceOfSecond->property("text").toString(),
             QStringLiteral("Starting Sunshine and pairing your client. Usually under a minute."));
    QObject* sentenceOfFirst = itemNamed(itemNamed(stepper.data(), QStringLiteral("stage0")),
                                         QStringLiteral("stageSentence"));
    QVERIFY(sentenceOfFirst);
    QVERIFY(!effectivelyVisible(sentenceOfFirst));
}

void TstUiScreens::theStepperMarksTheStageThatWasActiveFailedWithAMarkOfItsOwn()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> stepper(instantiate(&engine, QStringLiteral("SeatHubStepper.qml"), &error));
    QVERIFY2(stepper, qPrintable(error));
    stepper->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client)));

    // Stopped at the second stage: the first stays done, the second is failed, the third pending.
    client.setConnectStage(2);
    stepper->setProperty("failed", true);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("done"), QStringLiteral("failed"), QStringLiteral("pending")}));

    // The failed stage has a mark that is not a colour alone, in the destructive colour, and its
    // name is destructive too; nothing is breathing any more.
    QObject* failedRow = itemNamed(stepper.data(), QStringLiteral("stage1"));
    QVERIFY(failedRow);
    QObject* mark = itemNamed(failedRow, QStringLiteral("failedMark"));
    QVERIFY(mark);
    QVERIFY(effectivelyVisible(mark));
    QCOMPARE(mark->property("color").value<QColor>(), QColor(QStringLiteral("#ef4444")));
    QObject* name = itemNamed(failedRow, QStringLiteral("stageName"));
    QVERIFY(name);
    // The colour changes over the page duration (420ms), so it is read once it has settled.
    QTRY_COMPARE_WITH_TIMEOUT(name->property("color").value<QColor>(),
                              QColor(QStringLiteral("#ef4444")), 3000);
    for (int index = 0; index < 3; ++index) {
        QObject* row = itemNamed(stepper.data(), QStringLiteral("stage%1").arg(index));
        QVERIFY(row);
        QObject* dot = itemNamed(row, QStringLiteral("dot"));
        QVERIFY(dot);
        QTRY_VERIFY2_WITH_TIMEOUT(
            dot->property("color").value<QColor>() != QColor(QStringLiteral("#f59e0b")),
            "no stage is drawn as live once connecting has stopped", 3000);
    }

    // Stopped at the first stage before anything else was read: it is the one that failed.
    client.setConnectStage(0);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("failed"), QStringLiteral("pending"), QStringLiteral("pending")}));

    // Leaving the failed state draws the live stage again.
    stepper->setProperty("failed", false);
    QCOMPARE(stepperLooks(stepper.data()),
             (QStringList{QStringLiteral("active"), QStringLiteral("pending"), QStringLiteral("pending")}));
}

#include "tst_ui_screens.moc"

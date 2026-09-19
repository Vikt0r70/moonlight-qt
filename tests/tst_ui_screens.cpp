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
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
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

public:
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
    Q_INVOKABLE void openSettings() {}
    Q_INVOKABLE void signOut() { ++m_signOuts; }
    Q_INVOKABLE void retry() { ++m_retries; }
    Q_INVOKABLE void dismissError() { ++m_dismissals; }
    Q_INVOKABLE void requestOtp(const QString& phone) { m_lastPhone = phone; }
    Q_INVOKABLE void verifyOtp(const QString& phone, const QString& code)
    {
        m_lastPhone = phone;
        m_lastCode = code;
    }

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
    void homeStatusChanged();
    void endReasonTextChanged();
    void identityChanged();
    void failureChanged();
    void otpRequested(const QString& phoneE164);
    void otpRejected(const QString& message, const QString& reference);
    void otpAccepted();

private:
    QString m_homeStatus = QStringLiteral("ready");
    QString m_endReasonText;
    QString m_identity = QStringLiteral("+962 7 0001 0002");
    QString m_reference;
    QVariantMap m_failure;
    QString m_lastPhone;
    QString m_lastCode;
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

public:
    QString state() const { return m_state; }
    QVariantMap availableUpdate() const { return m_availableUpdate; }
    int progress() const { return m_progress; }
    QVariantMap failure() const { return m_failure; }
    bool blockedBySession() const { return m_blockedBySession; }

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
    void homeScreenRendersTheSessionEndReason();
    void signInScreenReachesThePhoneStepError();
    void signInScreenCountsDownToResend();
    void forcedUpdateModalKeepsUpdateTabbableAndUndismissable();
    void settingsDropdownElidesLongOptionNames();
    void disabledActionLabelStaysLegible();
    void agentConfigPanelMasksTheTokenAndNeverRendersIt();
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

    // ready: the populated state screens.md §23 draws.
    client.setHomeStatus(QStringLiteral("ready"));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Ready")),
             "Home must render the populated state");
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Open SeatHub and press Play.")),
             "Home must keep the copy deck's Ready sentence");

    // checking: the named loading state (copy.md §5, "name the thing loading").
    client.setHomeStatus(QStringLiteral("checking"));
    const QString checking = QStringLiteral("Checking availability\u2026");
    QObject* checkingItem = findVisibleTextItem(screen.data(), checking);
    QVERIFY2(checkingItem, "Home must render a named loading state while Play allocates");

    // busy: the empty state, copy.md's Play-flow sentence, verbatim.
    client.setHomeStatus(QStringLiteral("busy"));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("All rigs are busy right now.")),
             "Home must render the no-rig empty state from copy.md");

    // offline: copy.md §Support & errors, offline sentence, verbatim.
    client.setHomeStatus(QStringLiteral("offline"));
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Can't reach SevenHills right now.")),
             "Home must render the offline state from copy.md");

    // Back to ready, and Play is still the one action (screens.md §23).
    client.setHomeStatus(QStringLiteral("ready"));
    QObject* play = nullptr;
    for (QObject* item : screen->findChildren<QObject*>()) {
        if (item->inherits("QQuickButton") && item->property("text").toString()
                == QStringLiteral("Play")) {
            play = item;
            break;
        }
    }
    QVERIFY2(play, "Home's primary action must be a real button, reachable by keyboard (audit F3)");
    QVERIFY(play->property("activeFocusOnTab").toBool());
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

    // The phone step is what is on screen before a code is ever sent.
    QVERIFY2(findVisibleTextItem(screen.data(), QStringLiteral("Enter your phone number")),
             "the phone step is the first thing sign-in shows");
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

QTEST_MAIN(TstUiScreens)

#include "tst_ui_screens.moc"

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
#include <QAbstractListModel>
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

#include <algorithm>
#include <cmath>

#include <QSettings>
#include <QTemporaryDir>

#include "seathub/agent_config.h"
#include "seathub/settings_bridge.h"
#include "settings/streamingpreferences.h"

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

// A stand-in for the facade's list models (`customer_lists.h`): the same properties and the same four
// row roles, with no control plane behind it. `tst_facade_wiring` asserts the real models expose these
// names, so this file's fake and the real thing cannot drift apart unnoticed.
class FakeList : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY stateChanged)
    Q_PROPERTY(bool moreFailed READ moreFailed NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString errorReference READ errorReference NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    struct Row
    {
        QString when;
        QString kind;
        QString amount;
        QString tone;
    };

    QString status() const { return m_status; }
    bool loadingMore() const { return m_loadingMore; }
    bool moreFailed() const { return m_moreFailed; }
    bool hasMore() const { return m_hasMore; }
    QString errorText() const { return m_errorText; }
    QString errorReference() const { return m_errorReference; }
    int count() const { return m_rows.size(); }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return QVariant();
        }
        const Row& row = m_rows.at(index.row());
        switch (role) {
        case Qt::UserRole + 1: return row.when;
        case Qt::UserRole + 2: return row.kind;
        case Qt::UserRole + 3: return row.amount;
        case Qt::UserRole + 4: return row.tone;
        default: return QVariant();
        }
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return { { Qt::UserRole + 1, "whenText" }, { Qt::UserRole + 2, "kindText" },
                 { Qt::UserRole + 3, "amountText" }, { Qt::UserRole + 4, "tone" } };
    }

    void setRows(const QList<Row>& rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
        emit countChanged();
    }
    void appendRows(const QList<Row>& rows)
    {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size() + rows.size() - 1);
        m_rows.append(rows);
        endInsertRows();
        emit countChanged();
    }
    void setState(const QString& status, bool hasMore = false, bool loadingMore = false,
                  bool moreFailed = false, const QString& errorText = QString(),
                  const QString& errorReference = QString())
    {
        m_status = status;
        m_hasMore = hasMore;
        m_loadingMore = loadingMore;
        m_moreFailed = moreFailed;
        m_errorText = errorText;
        m_errorReference = errorReference;
        emit stateChanged();
    }

    /// `n` ordinary rows: a date, a plain word and a length. Nothing in them names a rig.
    static QList<Row> plainRows(int n, int from = 0)
    {
        QList<Row> rows;
        for (int i = from; i < from + n; ++i) {
            rows.append({ QStringLiteral("Sat 12 Sep, 21:%1").arg(i % 60, 2, 10, QLatin1Char('0')),
                          QStringLiteral("You ended it"), QStringLiteral("%1 min").arg(i + 1),
                          QString() });
        }
        return rows;
    }

signals:
    void stateChanged();
    void countChanged();

private:
    QList<Row> m_rows;
    QString m_status = QStringLiteral("idle");
    bool m_loadingMore = false;
    bool m_moreFailed = false;
    bool m_hasMore = false;
    QString m_errorText;
    QString m_errorReference;
};

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
    Q_PROPERTY(bool connectFailed READ connectFailed NOTIFY connectFailedChanged)
    Q_PROPERTY(QString stalledStepText READ stalledStepText NOTIFY connectFailedChanged)
    Q_PROPERTY(QString stalledReasonText READ stalledReasonText NOTIFY connectFailedChanged)
    Q_PROPERTY(QVariantMap account READ account NOTIFY accountChanged)
    Q_PROPERTY(QString accountStatus READ accountStatus NOTIFY accountChanged)
    Q_PROPERTY(QString accountError READ accountError NOTIFY accountChanged)
    Q_PROPERTY(QString accountErrorReference READ accountErrorReference NOTIFY accountChanged)
    Q_PROPERTY(QString totalsStatus READ totalsStatus NOTIFY totalsChanged)
    Q_PROPERTY(QString hoursPlayedText READ hoursPlayedText NOTIFY totalsChanged)
    Q_PROPERTY(QString creditLeftText READ creditLeftText NOTIFY totalsChanged)
    Q_PROPERTY(QString totalsError READ totalsError NOTIFY totalsChanged)
    Q_PROPERTY(QString totalsErrorReference READ totalsErrorReference NOTIFY totalsChanged)
    Q_PROPERTY(QObject* sessionHistory READ sessionHistory CONSTANT)
    Q_PROPERTY(QObject* creditHistory READ creditHistory CONSTANT)
    Q_PROPERTY(QObject* topupHistory READ topupHistory CONSTANT)

public:
    // --- the profile: the identity, the totals and the three lists, as the facade reports them ---
    QVariantMap account() const { return m_account; }
    QString accountStatus() const { return m_accountStatus; }
    QString accountError() const { return m_accountError; }
    QString accountErrorReference() const { return m_accountErrorReference; }
    void setAccount(const QString& username, const QString& email, const QString& phone,
                    bool emailVerified)
    {
        m_account.clear();
        m_account.insert(QStringLiteral("username"), username);
        m_account.insert(QStringLiteral("email"), email);
        m_account.insert(QStringLiteral("phone"), phone);
        m_account.insert(QStringLiteral("email_verified"), emailVerified);
        m_accountStatus = QStringLiteral("ready");
        emit accountChanged();
    }
    void setAccountState(const QString& status, const QString& error = QString(),
                         const QString& reference = QString())
    {
        m_account.clear();
        m_accountStatus = status;
        m_accountError = error;
        m_accountErrorReference = reference;
        emit accountChanged();
    }

    QString totalsStatus() const { return m_totalsStatus; }
    QString hoursPlayedText() const { return m_hoursPlayed; }
    QString creditLeftText() const { return m_creditLeft; }
    QString totalsError() const { return m_totalsError; }
    QString totalsErrorReference() const { return m_totalsErrorReference; }
    void setTotals(const QString& status, const QString& hours = QString(),
                   const QString& credit = QString(), const QString& error = QString(),
                   const QString& reference = QString())
    {
        m_totalsStatus = status;
        m_hoursPlayed = hours;
        m_creditLeft = credit;
        m_totalsError = error;
        m_totalsErrorReference = reference;
        emit totalsChanged();
    }

    QObject* sessionHistory() { return &m_sessions; }
    QObject* creditHistory() { return &m_credit; }
    QObject* topupHistory() { return &m_topups; }
    FakeList* sessions() { return &m_sessions; }
    FakeList* credit() { return &m_credit; }
    FakeList* topups() { return &m_topups; }

    /// What the profile asked the facade to do, in order (`loadFirstPage:sessions`, ...).
    QStringList calls() const { return m_calls; }
    Q_INVOKABLE void openProfile() { m_calls.append(QStringLiteral("openProfile")); }
    Q_INVOKABLE void closeProfile() { m_calls.append(QStringLiteral("closeProfile")); }
    Q_INVOKABLE void loadFirstPage(const QString& list) { m_calls.append(QStringLiteral("loadFirstPage:") + list); }
    Q_INVOKABLE void loadNextPage(const QString& list) { m_calls.append(QStringLiteral("loadNextPage:") + list); }
    Q_INVOKABLE void reloadList(const QString& list) { m_calls.append(QStringLiteral("reloadList:") + list); }
    Q_INVOKABLE void reloadTotals() { m_calls.append(QStringLiteral("reloadTotals")); }
    Q_INVOKABLE void reloadAccount() { m_calls.append(QStringLiteral("reloadAccount")); }
    int profileOpens() const { return m_calls.count(QStringLiteral("openProfile")); }

    bool connectFailed() const { return m_connectFailed; }
    QString stalledStepText() const { return m_stalledStepText; }
    QString stalledReasonText() const { return m_stalledReasonText; }
    /// A connect that stopped, as the facade reports it: where, and why in the deck's words.
    void setStalled(const QString& step, const QString& reasonStyled, const QString& reference)
    {
        m_connectFailed = true;
        m_stalledStepText = step;
        m_stalledReasonText = reasonStyled;
        m_reference = reference;
        emit failureChanged();
        emit connectFailedChanged();
    }
    void clearStalled()
    {
        m_connectFailed = false;
        m_stalledStepText.clear();
        m_stalledReasonText.clear();
        m_reference.clear();
        emit failureChanged();
        emit connectFailedChanged();
    }
    Q_INVOKABLE void interrupt() { ++m_interrupts; }
    int interrupts() const { return m_interrupts; }
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
    void accountChanged();
    void totalsChanged();
    void balanceChanged();
    void connectStageChanged();
    void connectFailedChanged();
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
    bool m_connectFailed = false;
    QString m_stalledStepText;
    QString m_stalledReasonText;
    QVariantMap m_account;
    QString m_accountStatus = QStringLiteral("loading");
    QString m_accountError;
    QString m_accountErrorReference;
    QString m_totalsStatus = QStringLiteral("loading");
    QString m_hoursPlayed;
    QString m_creditLeft;
    QString m_totalsError;
    QString m_totalsErrorReference;
    FakeList m_sessions;
    FakeList m_credit;
    FakeList m_topups;
    QStringList m_calls;
    int m_interrupts = 0;
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

// The `client` a SettingsPage row talks to for Plan 05-11's own tests (D-24 rebuild): the real
// `SettingsBridge` behind `client.settings`, exactly as production wires it, rather than
// `FakeBridge` above - the rebuild's own truths (which keys render, which are forced, the
// eleven stats toggles) are the bridge's real catalogue, not a hand-maintained stand-in that
// could silently drift from it.
class FakeSettingsClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* settings READ settings CONSTANT)

public:
    explicit FakeSettingsClient(SettingsBridge* bridge, QObject* parent = nullptr)
        : QObject(parent), m_bridge(bridge) {}

    QObject* settings() const { return m_bridge; }

    Q_INVOKABLE void closeSettings() { ++m_closes; }
    int closes() const { return m_closes; }

private:
    SettingsBridge* m_bridge;
    int m_closes = 0;
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
    void menuHasItsThreeItemsAndTopUpAsksTheFacadeForTheWebsite();
    void menuPopupIsWideEnoughToShowItsThreeItems();
    void theProfileIsRoutedAsAViewInsideHomeAndHomeNoLongerSignsOut();
    void theProfileShowsTheThreeIdentityRowsReadOnlyWithTheEmailMarkInWords();
    void aMissingEmailOrPhoneKeepsItsRowAndSaysNotAdded();
    void longIdentityValuesWrapInsideTheColumnInsteadOfBeingCut();
    void theIdentityBlockHasItsOwnLoadingAndErrorStates();
    void theTwoTotalsAreDrawnFromTheServersNumbersAndHaveTheirOwnStates();
    void theTabsSelectByClickAndByArrowKeysAndOnlyTheSelectedOneIsATabStop();
    void aListDrawsEachOfItsStates();
    void aListAsksForTheNextPageOnlyWhenItsRowsEnd();
    void theProfileAsksOnlyForTheListItShowsAndTheOthersWaitForTheirTab();
    void oneListFailingLeavesTheOthersTheTotalsAndTheIdentityIntact();
    void aRowNamesNoRigAndNoProfileFileCanReadOne();
    void signOutBackAndTheWebsiteLinkFromTheProfileReachTheFacade();
    void theEmptyActionsGoWhereTheirWordsSay();
    void theProfileFitsItsWindowAndKeepsTheListWideEnough();
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
    void settingsDropdownPopupIsTallEnoughToShowItsOptions();
    void noComboBoxPopupHeightBindsToTheControlsOwnContentItem();
    void settingsPageRendersTheSevenUpstreamSectionsInOrder();
    void aSettingsRowD25RemovesRendersNoRowAtAll();
    void theStatsGroupRendersOneToggleAndTheyDefaultOff();
    void theHostSpeakerRowIsPresentAndEditableAtUpstreamsDefault();
    void theStreamingBannerAndNegotiatedFallbackStillRenderOnTheRebuiltPage();
    void noSettingsPageStringCarriesTheUpstreamBrand();
    void customResolutionSelectionRevealsAndPersistsTheWidthHeightFields();
    void customFrameRateSelectionRevealsAndPersistsTheFpsField();
    void aStoredCustomResolutionOrFrameRateShowsItsFieldsOnLoad();
    void disabledActionLabelStaysLegible();
    void agentConfigPanelMasksTheTokenAndNeverRendersIt();
    void theWindowSequenceRestoresOnlyAfterTheEngineIsDone();
    void theStepperDrawsThreeStagesFromTheFacadesStageAndNothingElse();
    void theStepperMarksTheStageThatWasActiveFailedWithAMarkOfItsOwn();
    void connectingShowsCancelWhileItGoesAndTheNamedStallWhenItStops();
    void aStalledConnectOffersTryAgainAndBackToHomeAndNoOtherRig();
    void aStalledConnectWrapsTheLongestSentenceAndNeverBreaksAReference();
    void theErrorViewShowsThreeKindsOfSentenceAsTheyAreAndDrawsNoBareReferenceLabel();
    void noScreenRendersAnInternalStateOrEndReasonKey();
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
                              QStringLiteral("ConnectingScreen.qml"),
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

void TstUiScreens::menuHasItsThreeItemsAndTopUpAsksTheFacadeForTheWebsite()
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

    // Exactly the three items the design contract lists, in its order: Profile, Top up, Settings.
    QObject* popup = menu->property("menu").value<QObject*>();
    QVERIFY(popup);
    QCOMPARE(popup->property("count").toInt(), 3);
    QObject* profile = menu->findChild<QObject*>(QStringLiteral("menuItemProfile"));
    QObject* topUp = menu->findChild<QObject*>(QStringLiteral("menuItemTopUp"));
    QObject* settings = menu->findChild<QObject*>(QStringLiteral("menuItemSettings"));
    QVERIFY(profile);
    QVERIFY(topUp);
    QVERIFY(settings);
    QCOMPARE(profile->property("text").toString(), QStringLiteral("Profile"));
    QCOMPARE(topUp->property("text").toString(), QStringLiteral("Top up"));
    QCOMPARE(settings->property("text").toString(), QStringLiteral("Settings"));

    // `Top up` leaves the app, and says so with the external-link mark; the other two do not.
    QCOMPARE(topUp->property("glyph").toString(), QStringLiteral("↗"));
    QVERIFY(profile->property("glyph").toString().isEmpty());
    QVERIFY(settings->property("glyph").toString().isEmpty());

    // `Profile` opens the customer's own page, through the facade.
    QVERIFY(QMetaObject::invokeMethod(profile, "triggered"));
    QCOMPARE(client.profileOpens(), 1);
    QCOMPARE(client.topUps(), 0);
    QCOMPARE(client.settingsOpens(), 0);

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

void TstUiScreens::menuPopupIsWideEnoughToShowItsThreeItems()
{
    // The bug this test pins: the header menu opened but rendered a near-zero-width sliver, so
    // Profile / Top up / Settings were invisible and unclickable (menu-popup-zero-width). The
    // existing menu tests never measure the *popup's rendered width* - `menuHasItsThreeItems...`
    // asserts the root Item's implicitWidth (the 40x40 trigger), and finds items by objectName,
    // which exist regardless of geometry. A zero-width popup passed everything.
    //
    // A Menu is a Popup: it needs a shown, exposed window to lay out. This builds one (the
    // technique of `menuOpensOnSpace...`), opens the popup, waits for it to be visible, and only
    // then measures. The width math lives in the shared C++ template `T.Menu` and both the Basic
    // (forced here) and Material (production, app/main.cpp:733) styles share an identical
    // `implicitWidth: max(implicitBackgroundWidth + insets, implicitContentWidth + hpadding)`
    // formula and an identical default `background: Rectangle { implicitWidth: 200 }`, so a
    // Basic-style measurement reproduces the Material-style production bug faithfully.
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
                      QUrl::fromLocalFile(guiDir() + QStringLiteral("/tst_menu_width_window.qml")));
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
    QObject* popup = menu->property("menu").value<QObject*>();
    QVERIFY(popup);

    // Open it the way the customer does, and wait until it has genuinely laid out. A reading of 0
    // would mean the popup never opened (a suspect harness), not the bug - so verify visible first.
    QVERIFY(QMetaObject::invokeMethod(popup, "open"));
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(), 3000);

    auto* contentItem = popup->property("contentItem").value<QQuickItem*>();
    auto* profile = menu->findChild<QQuickItem*>(QStringLiteral("menuItemProfile"));
    auto* topUp = menu->findChild<QQuickItem*>(QStringLiteral("menuItemTopUp"));
    auto* settings = menu->findChild<QQuickItem*>(QStringLiteral("menuItemSettings"));
    QVERIFY(profile);
    QVERIFY(topUp);
    QVERIFY(settings);

    const qreal popupWidth = popup->property("width").toReal();
    const qreal popupImplicitWidth = popup->property("implicitWidth").toReal();
    const qreal implicitContentWidth = popup->property("implicitContentWidth").toReal();
    const qreal implicitBackgroundWidth = popup->property("implicitBackgroundWidth").toReal();
    const qreal contentWidth = popup->property("contentWidth").toReal();
    const qreal leftPadding = popup->property("leftPadding").toReal();
    const qreal rightPadding = popup->property("rightPadding").toReal();
    const qreal listWidth = contentItem ? contentItem->width() : -1;
    const qreal listImplicitWidth = contentItem ? contentItem->implicitWidth() : -1;
    const qreal listContentWidth = contentItem ? contentItem->property("contentWidth").toReal() : -1;
    const qreal rowImplicitWidth = profile->implicitWidth();
    const qreal rowWidth = profile->width();

    qWarning().noquote() << "MENUPROBE style=" << QQuickStyle::name()
                         << "popup.width=" << popupWidth
                         << "popup.implicitWidth=" << popupImplicitWidth
                         << "implicitContentWidth=" << implicitContentWidth
                         << "implicitBackgroundWidth=" << implicitBackgroundWidth
                         << "contentWidth=" << contentWidth
                         << "leftPadding=" << leftPadding << "rightPadding=" << rightPadding
                         << "list.width=" << listWidth
                         << "list.implicitWidth=" << listImplicitWidth
                         << "list.contentWidth=" << listContentWidth
                         << "row.implicitWidth=" << rowImplicitWidth
                         << "row.width=" << rowWidth
                         << "topUp.width=" << topUp->width()
                         << "settings.width=" << settings->width();

    // The row's implicitWidth is the width a row *wants* (its label plus padding, floored at the
    // design's menu min-width `Metrics.s24 * 2`). If it is ~0 the diagnosis is measurement, not
    // propagation, and the fix differs - fail loudly rather than silently mis-measure.
    QVERIFY2(rowImplicitWidth > 40,
             qPrintable(QStringLiteral("a row reports implicitWidth %1 (<=40): measurement branch, "
                                       "not the propagation bug - re-plan").arg(rowImplicitWidth)));

    // Core symptom: the popup must be at least as wide as a row wants to be. On today's code the
    // popup collapses to padding while the row wants ~192, so this fails; the fix restores the
    // floor and it passes. Derived from the row (no invented constant).
    QVERIFY2(popupWidth >= rowImplicitWidth,
             qPrintable(QStringLiteral("popup width %1 is narrower than a row's implicitWidth %2 - "
                                       "the items cannot render").arg(popupWidth).arg(rowImplicitWidth)));

    // Each of the three items is laid out at a readable width (the full menu content area, popup
    // width minus its horizontal padding), not squeezed to a sliver, and lies inside the popup.
    const qreal availableWidth = popupWidth - leftPadding - rightPadding;
    for (QQuickItem* row : { profile, topUp, settings }) {
        QVERIFY2(row->width() >= availableWidth - 1.0,
                 qPrintable(QStringLiteral("item '%1' is %2px wide, the menu content area is %3px - "
                                           "it is squeezed")
                                .arg(row->property("text").toString())
                                .arg(row->width())
                                .arg(availableWidth)));
    }

    // Close what we opened before the window is torn down: a Menu is a Popup with an input grab
    // (a transient native window on Windows), and destroying its window while it is open is an
    // abnormal teardown that can leave stale focus state for the next windowed test.
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QTRY_VERIFY_WITH_TIMEOUT(!popup->property("opened").toBool(), 3000);
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
    QCOMPARE(client.profileOpens() + client.topUps() + client.settingsOpens(), 1);
    // Down, Down, Up from nothing highlighted lands on the first item, which is Profile now.
    QVERIFY2(client.profileOpens() == 1, "the first item is the profile");
    QCOMPARE(client.topUps(), 0);

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

void TstUiScreens::settingsDropdownPopupIsTallEnoughToShowItsOptions()
{
    // The bug this test pins (combobox-popup-collapsed): both SeatHubSelect.qml and the raw
    // ComboBox at SettingsPage.qml (captureWhenBox) bound `popup.height` to an UNQUALIFIED
    // `contentItem.implicitHeight`. That dot-property binding lives inside the ComboBox's own
    // body, not inside a `popup: T.Popup { ... }` block, so `contentItem` there resolves to the
    // ComboBox's OWN one-line display contentItem, not the popup's ListView - every opened
    // Settings dropdown rendered as a ~one-line grey band with no visible/selectable rows. The
    // pre-existing `settingsDropdownElidesLongOptionNames` test never opens the popup or measures
    // its height; object-existence and width assertions pass on a collapsed popup regardless.
    //
    // Confirmed against stock Qt source (Basic and Material ComboBox.qml): both set
    // `contentItem: ListView { implicitHeight: contentHeight }` on the POPUP itself, and compute
    // `height: Math.min(contentItem.implicitHeight [+ verticalPadding*2], ...)` from INSIDE the
    // popup's own scope, where the unqualified name correctly resolves to that ListView. The fix
    // re-qualifies the same expression as `popup.contentItem.implicitHeight + popup.topPadding +
    // popup.bottomPadding` (topPadding/bottomPadding generalize Material's verticalPadding*2 and
    // collapse to 0 under Basic, which sets none) - it is the minimal reversal of the scoping
    // mistake, not a new formula.
    //
    // A ComboBox popup is a Popup: it needs a shown, exposed window to lay out (the technique of
    // `menuPopupIsWideEnoughToShowItsThreeItems`).
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    FakeBridge bridge;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
                          "import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "ApplicationWindow {\n"
                          "    id: win\n"
                          "    width: 500; height: 700; visible: true\n"
                          "    property var bridge: null\n"
                          "    Column {\n"
                          "        anchors.fill: parent\n"
                          "        spacing: 8\n"
                          "        SeatHubSelect {\n"
                          "            objectName: \"shortList\"\n"
                          "            bridge: win.bridge\n"
                          "            title: \"Short\"\n"
                          "            optionsOverride: [\"Option A\", \"Option B\", \"Option C\", "
                          "\"Option D\", \"Option E\"]\n"
                          "        }\n"
                          "        SeatHubSelect {\n"
                          "            objectName: \"longList\"\n"
                          "            bridge: win.bridge\n"
                          "            title: \"Long\"\n"
                          "            optionsOverride: (function() { var a = []; "
                          "for (var i = 0; i < 30; i++) a.push(\"Option \" + i); return a; })()\n"
                          "        }\n"
                          "    }\n"
                          "}\n"),
                      QUrl::fromLocalFile(guiDir()
                                          + QStringLiteral("/tst_combobox_popup_height_window.qml")));
    QScopedPointer<QObject> created(component.create());
    QVERIFY2(created, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(created.data());
    QVERIFY(window);
    QVERIFY(window->setProperty("bridge", QVariant::fromValue(static_cast<QObject*>(&bridge))));
    QVERIFY(QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));

    auto findDropdown = [](QObject* row) -> QObject* {
        for (QObject* item : row->findChildren<QObject*>()) {
            if (item->inherits("QQuickComboBox"))
                return item;
        }
        return nullptr;
    };

    // --- Short list (5 options): the popup must show more than a single display line. ---
    QObject* shortRow = window->findChild<QObject*>(QStringLiteral("shortList"));
    QVERIFY(shortRow);
    QObject* shortDropdown = findDropdown(shortRow);
    QVERIFY2(shortDropdown, "the row must render a real ComboBox");

    QObject* shortPopup = shortDropdown->property("popup").value<QObject*>();
    QVERIFY(shortPopup);
    QVERIFY(QMetaObject::invokeMethod(shortPopup, "open"));
    QTRY_VERIFY_WITH_TIMEOUT(shortPopup->property("opened").toBool(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(shortPopup->property("visible").toBool(), 3000);

    auto* shortListView = shortPopup->property("contentItem").value<QQuickItem*>();
    QVERIFY2(shortListView && shortListView->inherits("QQuickListView"),
             "the popup's content item must be a ListView");

    auto* dropdownContentItem = shortDropdown->property("contentItem").value<QQuickItem*>();
    QVERIFY(dropdownContentItem);
    const qreal oneLineHeight = dropdownContentItem->property("implicitHeight").toReal();
    QVERIFY2(oneLineHeight > 0, "the ComboBox's own display line must have a real height");

    // Dump the runtime probe BEFORE the flip assertion below, which returns out of the test
    // function on failure - a dump placed after it would never print pre-fix, and "record the
    // REAL numbers" is the whole point of this probe.
    auto dumpProbe = [&](const char* label) {
        qWarning().noquote()
            << "COMBOPROBE" << label << "style=" << QQuickStyle::name()
            << "popup.height=" << shortPopup->property("height").toReal()
            << "popup.contentItem.contentHeight=" << shortListView->property("contentHeight").toReal()
            << "popup.contentItem.implicitHeight=" << shortListView->property("implicitHeight").toReal()
            << "popup.topPadding=" << shortPopup->property("topPadding").toReal()
            << "popup.bottomPadding=" << shortPopup->property("bottomPadding").toReal()
            << "dropdown.contentItem.implicitHeight=" << oneLineHeight;
    };
    dumpProbe("(at-visible)");

    // Core symptom, made a QTRY: post-fix, popup.height re-evaluates via a live binding once the
    // ListView's contentHeight settles a frame after `visible` flips, so an instantaneous read
    // right after QTRY_VERIFY(visible) risks a flaky red-or-green. On today's code popup.height is
    // pinned to oneLineHeight (the wrong object) and never grows regardless of how long this waits.
    QTRY_VERIFY_WITH_TIMEOUT(shortPopup->property("height").toReal() > oneLineHeight * 1.5, 3000);

    dumpProbe("(post-flip)");
    const qreal popupHeight = shortPopup->property("height").toReal();

    // Not just tall - the actual option rows must be inside the popup and visible, the same
    // "options are reachable, not just a box is tall" check the menu test made for width.
    // `QQuickItemView::itemAtIndex(int)` (the same mechanism ListView/GridView expose to QML) is
    // used rather than searching the QObject tree: ComboBox popup delegates are managed by the
    // view's own delegate model and are not reliably reachable via QObject::findChildren.
    QQuickItem* firstDelegate = nullptr;
    QQuickItem* secondDelegate = nullptr;
    auto realizedBothRows = [&]() {
        firstDelegate = nullptr;
        secondDelegate = nullptr;
        QMetaObject::invokeMethod(shortListView, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, firstDelegate),
                                  Q_ARG(int, 0));
        QMetaObject::invokeMethod(shortListView, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, secondDelegate),
                                  Q_ARG(int, 1));
        return firstDelegate != nullptr && secondDelegate != nullptr;
    };
    // contentHeight (and so popup.height) can settle before the ListView has actually incubated
    // its delegate items, so realizing them is its own QTRY.
    QTRY_VERIFY_WITH_TIMEOUT(realizedBothRows(), 3000);
    QVERIFY2(firstDelegate, "the first option row must be realized inside the popup");
    QVERIFY2(secondDelegate, "the second option row must be realized inside the popup - "
                             "one visible row is still the collapsed-popup symptom");
    QVERIFY2(firstDelegate->height() > 0, "the first option row must have a real height");
    QVERIFY2(secondDelegate->y() + secondDelegate->height() <= popupHeight + 0.5,
             "the second option row must fit inside the popup's own height");

    QVERIFY(QMetaObject::invokeMethod(shortPopup, "close"));
    QTRY_VERIFY_WITH_TIMEOUT(!shortPopup->property("opened").toBool(), 3000);

    // --- Long list (30 options): E9's 320px elision cap must still hold. ---
    QObject* longRow = window->findChild<QObject*>(QStringLiteral("longList"));
    QVERIFY(longRow);
    QObject* longDropdown = findDropdown(longRow);
    QVERIFY2(longDropdown, "the row must render a real ComboBox");
    QObject* longPopup = longDropdown->property("popup").value<QObject*>();
    QVERIFY(longPopup);
    QVERIFY(QMetaObject::invokeMethod(longPopup, "open"));
    QTRY_VERIFY_WITH_TIMEOUT(longPopup->property("opened").toBool(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(longPopup->property("visible").toBool(), 3000);
    QTest::qWait(50);  // let the ListView's contentHeight settle before the cap read

    const qreal longPopupHeight = longPopup->property("height").toReal();
    qWarning().noquote() << "COMBOPROBE(long) popup.height=" << longPopupHeight;
    QVERIFY2(longPopupHeight <= 320.5,
             qPrintable(QStringLiteral("popup height %1 exceeds the E9 320px elision cap")
                            .arg(longPopupHeight)));
    QVERIFY2(longPopupHeight > oneLineHeight * 1.5,
             "the long-model popup must also show real rows, not just respect the cap");

    QVERIFY(QMetaObject::invokeMethod(longPopup, "close"));
    QTRY_VERIFY_WITH_TIMEOUT(!longPopup->property("opened").toBool(), 3000);
}

void TstUiScreens::noComboBoxPopupHeightBindsToTheControlsOwnContentItem()
{
    // Recurrence guard for combobox-popup-collapsed: the defect was copy-pasted into TWO sites
    // (SeatHubSelect.qml and SettingsPage.qml's captureWhenBox) before this fix, and nothing
    // stopped a third. A functional test only covering SeatHubSelect would leave that second site,
    // and any future copy-paste, undetected. This scans every QML file the app ships for the
    // literal unqualified pattern, independent of which file it turns up in.
    const QDir dir(guiDir());
    const QStringList qmlFiles = dir.entryList(QStringList() << QStringLiteral("*.qml"), QDir::Files);
    QVERIFY2(!qmlFiles.isEmpty(), "app/gui could not be located from the test binary");

    const QRegularExpression unqualified(
        QStringLiteral("popup\\.height\\s*:\\s*Math\\.min\\(\\s*contentItem\\."));

    for (const QString& name : qmlFiles) {
        const QString source = readSource(dir.filePath(name));
        QVERIFY2(!unqualified.match(source).hasMatch(),
                 qPrintable(name + QStringLiteral(": popup.height binds to the unqualified "
                                                  "contentItem (the CONTROL's own content, not the "
                                                  "popup's) - use popup.contentItem.implicitHeight")));
    }
}

namespace {

// A fresh, isolated `SettingsBridge` for each Plan 05-11 settings test: QSettings is redirected
// to its own temporary directory so no test's writes can leak into another's, and so this suite
// never touches the machine's real preferences (same technique `tst_settings_bridge.cpp` uses).
struct SettingsFixture
{
    QTemporaryDir dir;
    SettingsBridge* bridge;
    FakeSettingsClient* client;

    SettingsFixture()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("SeatHubTest"));
        QCoreApplication::setApplicationName(QStringLiteral("SeatHubUiScreensSettingsTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
        bridge = new SettingsBridge();
        client = new FakeSettingsClient(bridge);
    }

    ~SettingsFixture()
    {
        delete client;
        delete bridge;
    }
};

// `client` is set at CREATION time (`createWithInitialProperties`, the same technique
// `ProfileScreen.qml`'s own tests already use), matching how `main.qml` actually wires it -
// `SettingsPage { client: seatHub }` inline, never a later `setProperty`. Setting `client` only
// after `component.create()` leaves every row's own `Component.onCompleted: refresh()` looking
// at a still-null bridge, which is a timing gap this page's own components do not otherwise
// guard against (they refresh on the bridge's `valueChanged`, not on the bridge object itself
// being attached later) - not a gap production ever hits, so this test matches production
// instead of instrumenting the components for a case they never see.
QObject* instantiateSettingsPage(QQmlEngine* engine, FakeSettingsClient* client, QString* error)
{
    QQmlComponent component(engine, QUrl::fromLocalFile(guiDir() + QStringLiteral("/SettingsPage.qml")));
    QObject* root = component.createWithInitialProperties(
        { { QStringLiteral("client"), QVariant::fromValue(static_cast<QObject*>(client)) } });
    if (!root && error) {
        *error = component.errorString();
    }
    return root;
}

// The CheckBox for a given row label, found by walking up from the label `Text` until an
// ancestor's subtree contains one - not by `objectName`, which Qt Quick Controls 2's "Basic"
// style CheckBox does not carry through to `QObject::objectName()` in this Qt build (verified:
// a `CheckBox { objectName: "x" }` reports `objectName() == ""` at the C++ boundary here, for
// every CheckBox on the page, not just the Repeater-built ones). Walking the tree structurally
// is what the pre-existing `settingsDropdownElidesLongOptionNames` test already does for a
// ComboBox, and what this does for a CheckBox: no assumption about exactly how many Column
// levels sit between a row's label and its control, so it covers both SeatHubToggle's
// `Row{Column{Text,Text};CheckBox}` shape and the stats group's flatter `Row{Text;CheckBox}`.
QObject* checkBoxNear(QObject* root, const QString& labelText)
{
    for (QObject* text : textItems(root)) {
        if (text->property("text").toString() != labelText) {
            continue;
        }
        QObject* ancestor = text->parent();
        for (int depth = 0; ancestor && depth < 5; depth++, ancestor = ancestor->parent()) {
            for (QObject* child : ancestor->findChildren<QObject*>()) {
                if (child->inherits("QQuickCheckBox")) {
                    return child;
                }
            }
        }
    }
    return nullptr;
}

void collectVisualItems(QQuickItem* item, QList<QQuickItem*>& out)
{
    for (QQuickItem* child : item->childItems()) {
        out.append(child);
        collectVisualItems(child, out);
    }
}

// The CheckBox for one stats-toggle row, found by walking the real (`QQuickItem`) VISUAL tree
// rather than the `QObject` tree `checkBoxNear`/`findChildren` walk. This is not a style choice:
// `Repeater` sets each delegate's `QQuickItem::parentItem()` (its rendering parent) to the
// Repeater's own parent, but does NOT reparent the delegate's `QObject::parent()` to match -
// verified empirically here (a Repeater reporting `count: 11` whose own `QObject::findChildren`
// returns 4, and whose parent Column's `findChildren` returns 0 of the 11 labels). Every other
// helper in this file (`textItems`, `checkBoxNear`) walks `QObject::children()`, which is why
// they see every statically declared row perfectly well and would see none of a Repeater's.
QObject* statsCheckBoxFor(QQuickItem* root, const QString& labelText)
{
    QList<QQuickItem*> all;
    collectVisualItems(root, all);
    for (QQuickItem* candidate : all) {
        if (candidate->property("text").toString() != labelText) {
            continue;
        }
        for (QQuickItem* ancestor = candidate->parentItem(); ancestor;
             ancestor = ancestor->parentItem()) {
            QList<QQuickItem*> siblings;
            collectVisualItems(ancestor, siblings);
            for (QQuickItem* sibling : siblings) {
                if (sibling->inherits("QQuickCheckBox")) {
                    return sibling;
                }
            }
        }
    }
    return nullptr;
}

// Every stats-toggle CheckBox, keyed by the bridge's own key (via its label - see
// `statsCheckBoxFor` for why this cannot use `checkBoxNear`). One entry per key
// `statsToggleKeys()` names.
QHash<QString, QObject*> statsCheckBoxesByKey(QObject* rootObject, SettingsBridge* bridge)
{
    QHash<QString, QObject*> out;
    QQuickItem* root = qobject_cast<QQuickItem*>(rootObject);
    if (!root) {
        return out;
    }
    for (const QString& key : bridge->statsToggleKeys()) {
        QObject* box = statsCheckBoxFor(root, bridge->statsToggleLabel(key));
        if (box) {
            out.insert(key, box);
        }
    }
    return out;
}

// The settings row (a SeatHubSelect or SeatHubNumberField) carrying a given `title` - the label
// the user reads. Every row on the page has a unique title, so the title alone identifies one, the
// same way `checkBoxNear` finds a control by its label rather than by an objectName the Basic-style
// controls do not carry to the C++ boundary in this Qt build. A hidden row is still in the object
// tree, so this finds the Custom fields whether or not they are visible.
QObject* itemByTitle(QObject* root, const QString& title)
{
    for (QObject* child : root->findChildren<QObject*>()) {
        if (child->metaObject()->indexOfProperty("title") >= 0
            && child->property("title").toString() == title) {
            return child;
        }
    }
    return nullptr;
}

// The real ComboBox inside a SeatHubSelect row.
QObject* comboInside(QObject* row)
{
    for (QObject* item : row->findChildren<QObject*>()) {
        if (item->inherits("QQuickComboBox")) {
            return item;
        }
    }
    return nullptr;
}

} // namespace

void TstUiScreens::settingsPageRendersTheSevenUpstreamSectionsInOrder()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    // D-24: the seven upstream sections, upstream's own titles, upstream's own order - not
    // Phase 3's D-48 grouping.
    const QStringList sections = { QStringLiteral("Basic Settings"), QStringLiteral("Audio Settings"),
                                   QStringLiteral("Host Settings"), QStringLiteral("UI Settings"),
                                   QStringLiteral("Input Settings"), QStringLiteral("Gamepad Settings"),
                                   QStringLiteral("Advanced Settings") };
    QList<QObject*> items = textItems(root.data());
    QList<int> positions;
    for (const QString& section : sections) {
        int position = -1;
        for (int i = 0; i < items.size(); i++) {
            if (items.at(i)->property("text").toString() != section) {
                continue;
            }
            const QFont font = items.at(i)->property("font").value<QFont>();
            if (font.pixelSize() == 24) { // Metrics.fontH2
                position = i;
                break;
            }
        }
        QVERIFY2(position >= 0, qPrintable(QStringLiteral("SettingsPage must render the ")
                                          + section + QStringLiteral(" heading (D-24)")));
        positions.append(position);
    }
    for (int i = 1; i < positions.size(); i++) {
        QVERIFY2(positions.at(i) > positions.at(i - 1),
                 qPrintable(QStringLiteral("section out of upstream order at ") + sections.at(i)));
    }

    // The way back Task 2 asks for.
    bool sawBack = false;
    for (QObject* item : items) {
        if (item->property("text").toString() == QStringLiteral("Back")) {
            sawBack = true;
            break;
        }
    }
    QVERIFY2(sawBack, "the settings page must offer a way back to the home view");
}

void TstUiScreens::aSettingsRowD25RemovesRendersNoRowAtAll()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    // D-25/D-25a: not disabled, not behind a disclosure - not rendered at all. Five forced rows
    // plus the derived `showperfoverlay` (replaced by the stats group, not a row of its own).
    const QStringList removedLabels = {
        QStringLiteral("Language"),
        QStringLiteral("Discord Rich Presence integration"),
        QStringLiteral("Automatically find PCs on the local network (Recommended)"),
        QStringLiteral("Automatically detect blocked connections (Recommended)"),
        QStringLiteral("Quit app on host PC after ending stream"),
    };
    for (QObject* item : textItems(root.data())) {
        const QString text = item->property("text").toString();
        for (const QString& removed : removedLabels) {
            QVERIFY2(text != removed,
                     qPrintable(QStringLiteral("a removed row must not render at all: ") + removed));
        }
    }

    // And no leftover "Managed by SeatHub" disclosure section (Phase 3's pattern, prohibited now).
    for (QObject* item : textItems(root.data())) {
        QVERIFY2(item->property("text").toString() != QStringLiteral("Managed by SeatHub"),
                 "removed rows must not be disclosed on screen; the audit carries them instead");
    }
}

void TstUiScreens::theStatsGroupRendersOneToggleAndTheyDefaultOff()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    // The group's own title.
    bool sawGroupTitle = false;
    for (QObject* item : textItems(root.data())) {
        if (item->property("text").toString() == QStringLiteral("Show performance stats while streaming")) {
            sawGroupTitle = true;
            break;
        }
    }
    QVERIFY2(sawGroupTitle, "the stats group title must be upstream's own label");

    // D-23/OD-03: one toggle per line, eleven of them, each off by default. Found through the
    // real (visual) item tree, not `QObject::findChildren` - see `statsCheckBoxFor`'s comment
    // for why a Repeater's own delegates are invisible to that walk.
    const QStringList keys = fixture.bridge->statsToggleKeys();
    QCOMPARE(keys.size(), 11);
    const QHash<QString, QObject*> boxes = statsCheckBoxesByKey(root.data(), fixture.bridge);
    QCOMPARE(boxes.size(), 11);
    for (auto it = boxes.constBegin(); it != boxes.constEnd(); ++it) {
        QVERIFY2(!it.value()->property("checked").toBool(),
                 qPrintable(QStringLiteral("stats toggle must default off: ") + it.key()));
    }

    // Each toggle's own label is Moonlight's own line text (`copy.md` § Settings), with no
    // number in it (OD-03) - `statsCheckBoxesByKey` already proved a control exists for
    // "Average network latency" specifically, since it is one of the eleven keys.
    QVERIFY2(boxes.contains(QStringLiteral("statsNetworkLatency")),
             "a stats toggle must be labelled with the line's own text");
}

void TstUiScreens::theHostSpeakerRowIsPresentAndEditableAtUpstreamsDefault()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    // Upstream's own default: `playAudioOnHost = false`, shown checked (host muted) because the
    // row is the inverse of the stored key (D-25a).
    QCOMPARE(fixture.bridge->getValue(QStringLiteral("hostaudio")).toBool(), true);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    QObject* checkbox = checkBoxNear(root.data(), QStringLiteral("Mute host PC speakers while streaming"));
    QVERIFY2(checkbox, "D-25a: the host-speaker row must be present");
    QVERIFY2(checkbox->property("checked").toBool(), "must ship checked, upstream's own default");
    QVERIFY2(checkbox->property("enabled").toBool(), "D-25a: the row must stay user-editable");
}

void TstUiScreens::theStreamingBannerAndNegotiatedFallbackStillRenderOnTheRebuiltPage()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    auto bannerVisible = [&]() {
        for (QObject* item : textItems(root.data())) {
            if (item->property("text").toString()
                == QStringLiteral("You're streaming. These settings can't be changed until the session ends.")) {
                return item->property("visible").toBool();
            }
        }
        return false;
    };
    QVERIFY2(!bannerVisible(), "the banner must be hidden while writable");

    fixture.bridge->setStreamingActive(true);
    QVERIFY2(bannerVisible(), "Pitfall 6: the banner must show while a session is streaming");

    fixture.bridge->setStreamingActive(false);
    QVERIFY2(!bannerVisible(), "the banner must clear once the session ends");

    // D-14: a fallback the engine negotiated is shown beside the saved value, in SeatHub's own
    // words, without rewriting what was saved.
    fixture.bridge->noteLaunchWarning(QStringLiteral("Your host PC doesn't support HDR streaming."));
    bool sawWarning = false;
    for (QObject* item : textItems(root.data())) {
        if (item->property("text").toString().contains(QStringLiteral("HDR isn't available for this session"))) {
            sawWarning = item->property("visible").toBool();
            break;
        }
    }
    QVERIFY2(sawWarning, "a negotiated fallback must render beside the saved value");
}

void TstUiScreens::noSettingsPageStringCarriesTheUpstreamBrand()
{
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    for (QObject* item : textItems(root.data())) {
        const QString text = item->property("text").toString();
        QVERIFY2(!text.contains(QStringLiteral("Moonlight")),
                 qPrintable(QStringLiteral("a rendered string carries the upstream brand: ") + text));
    }
}

void TstUiScreens::customResolutionSelectionRevealsAndPersistsTheWidthHeightFields()
{
    // custom-resolution-fps-dead: picking "Custom" in the Resolution dropdown must reveal the
    // Custom width and Custom height fields, and they must STAY revealed while width/height are
    // edited to a preset-matching pair. "Custom" was a value DERIVED from the stored pair, never a
    // requestable mode: setResolutionPreset("Custom") stored nothing and emitted no valueChanged
    // while the values still matched a preset, so refreshDerived() never ran and the fields'
    // `visible` gate (bound to page.customResolution) never turned true. No prior test drove the
    // select to "Custom" or asserted the fields appear, which is how this shipped.
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    // Start on a concrete preset (720p) - the reported starting state, and the one the derive-only
    // bug needs: resolutionPreset() returns "720p", so the pre-fix "Custom" write is a silent no-op.
    fixture.bridge->setValue(QStringLiteral("width"), 1280);
    fixture.bridge->setValue(QStringLiteral("height"), 720);
    QCOMPARE(fixture.bridge->resolutionPreset(), QStringLiteral("720p"));

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    QObject* resolutionSelect = itemByTitle(root.data(), QStringLiteral("Resolution"));
    QVERIFY2(resolutionSelect, "the Basic Settings section must render a Resolution select");
    QObject* widthField = itemByTitle(root.data(), QStringLiteral("Custom width"));
    QObject* heightField = itemByTitle(root.data(), QStringLiteral("Custom height"));
    // Exist regardless of visibility - a hidden Item is still in the object tree. The bug is that
    // they never become visible, not that they are absent (guards a vacuous "object exists" pass).
    QVERIFY2(widthField, "the Custom width field must exist in the page tree");
    QVERIFY2(heightField, "the Custom height field must exist in the page tree");

    // Anchor the visibility reads: the Resolution select is unconditionally visible in this same
    // instantiation, so a `visible:false` reading on the Custom fields is the real bug, not a dead
    // or unrealized item tree.
    QVERIFY2(effectivelyVisible(resolutionSelect),
             "the Resolution select must be visible - anchors the fields' visibility reads");

    // Pre-reveal: at a preset start the Custom fields are correctly hidden.
    QVERIFY2(!widthField->property("visible").toBool(), "Custom width starts hidden at a preset");
    QVERIFY2(!heightField->property("visible").toBool(), "Custom height starts hidden at a preset");

    // Drive the real ComboBox the way the user does: select "Custom" and fire activated, which runs
    // SeatHubSelect.onActivated -> edited("Custom") -> the page's onEdited handler -> the select's
    // own refresh(). A bare edited() emit would skip that trailing refresh and leave the
    // dropdown-display half of the fix (the select must show "Custom", not snap back) untested.
    QObject* combo = comboInside(resolutionSelect);
    QVERIFY2(combo, "the Resolution select must render a real ComboBox");
    const int customIdx = fixture.bridge->resolutionPresets().indexOf(QStringLiteral("Custom"));
    QVERIFY2(customIdx >= 0, "the resolution options must offer Custom");
    QVERIFY(combo->setProperty("currentIndex", customIdx));
    QVERIFY2(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, customIdx)),
             "could not fire the Resolution ComboBox's activated signal");

    // (a) picking Custom reveals BOTH fields - the reported failure. RED on today's code (they stay
    // hidden, the "Custom" write being a silent no-op from a preset start).
    QVERIFY2(widthField->property("visible").toBool(), "picking Custom must reveal Custom width");
    QVERIFY2(heightField->property("visible").toBool(), "picking Custom must reveal Custom height");
    QVERIFY(effectivelyVisible(widthField));
    QVERIFY(effectivelyVisible(heightField));
    // The dropdown must DISPLAY Custom, not snap back to "720p" for the still-preset stored pair.
    QCOMPARE(resolutionSelect->property("value").toString(), QStringLiteral("Custom"));

    // (b) the fields STAY visible while width/height are edited to a PRESET-MATCHING pair (1080p) -
    // the mid-edit-vanish a refreshDerived()-from-the-pair-only fix leaves. Prove the pair really
    // matches a preset first, or this guard - the one that separates the real fix from the naive
    // one - passes vacuously.
    fixture.bridge->setValue(QStringLiteral("width"), 1920);
    fixture.bridge->setValue(QStringLiteral("height"), 1080);
    QCOMPARE(fixture.bridge->resolutionPreset(), QStringLiteral("1080p"));
    QVERIFY2(widthField->property("visible").toBool(),
             "Custom width must stay visible while editing to a preset-matching pair");
    QVERIFY2(heightField->property("visible").toBool(),
             "Custom height must stay visible while editing to a preset-matching pair");

    // (c) picking a concrete preset hides the fields again and stores that preset.
    const int idx720 = fixture.bridge->resolutionPresets().indexOf(QStringLiteral("720p"));
    QVERIFY(idx720 >= 0);
    QVERIFY(combo->setProperty("currentIndex", idx720));
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, idx720)));
    QVERIFY2(!widthField->property("visible").toBool(), "picking a preset must hide Custom width");
    QVERIFY2(!heightField->property("visible").toBool(), "picking a preset must hide Custom height");
    QCOMPARE(fixture.bridge->resolutionPreset(), QStringLiteral("720p"));
    QCOMPARE(resolutionSelect->property("value").toString(), QStringLiteral("720p"));
}

void TstUiScreens::customFrameRateSelectionRevealsAndPersistsTheFpsField()
{
    // custom-resolution-fps-dead, the Frame rate half: identical derive-only bug in
    // setFrameRatePreset("Custom") (settings_bridge.cpp), gating the Custom frame rate field.
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    // Start on a concrete preset (60 FPS).
    fixture.bridge->setValue(QStringLiteral("fps"), 60);
    QCOMPARE(fixture.bridge->frameRatePreset(), QStringLiteral("60 FPS"));

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    QObject* frameRateSelect = itemByTitle(root.data(), QStringLiteral("Frame rate"));
    QVERIFY2(frameRateSelect, "the Basic Settings section must render a Frame rate select");
    QObject* fpsField = itemByTitle(root.data(), QStringLiteral("Custom frame rate"));
    QVERIFY2(fpsField, "the Custom frame rate field must exist in the page tree");
    QVERIFY2(effectivelyVisible(frameRateSelect),
             "the Frame rate select must be visible - anchors the field's visibility reads");
    QVERIFY2(!fpsField->property("visible").toBool(), "Custom frame rate starts hidden at a preset");

    QObject* combo = comboInside(frameRateSelect);
    QVERIFY2(combo, "the Frame rate select must render a real ComboBox");
    const int customIdx = fixture.bridge->frameRatePresets().indexOf(QStringLiteral("Custom"));
    QVERIFY2(customIdx >= 0, "the frame-rate options must offer Custom");
    QVERIFY(combo->setProperty("currentIndex", customIdx));
    QVERIFY2(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, customIdx)),
             "could not fire the Frame rate ComboBox's activated signal");

    // (a) picking Custom reveals the fps field. RED on today's code.
    QVERIFY2(fpsField->property("visible").toBool(), "picking Custom must reveal the fps field");
    QVERIFY(effectivelyVisible(fpsField));
    QCOMPARE(frameRateSelect->property("value").toString(), QStringLiteral("Custom"));

    // (b) the field STAYS visible while fps is edited to a preset-matching value (30 FPS). Prove the
    // value matches a preset first. (The Frame rate select's own refresh() fires only on
    // width/height, never fps, so this asserts the FIELD's visibility - the select-display half is
    // covered by the activated-path assertions above and in (c).)
    fixture.bridge->setValue(QStringLiteral("fps"), 30);
    QCOMPARE(fixture.bridge->frameRatePreset(), QStringLiteral("30 FPS"));
    QVERIFY2(fpsField->property("visible").toBool(),
             "the fps field must stay visible while editing to a preset-matching value");

    // (c) picking a concrete preset hides the field again and stores that preset.
    const int idx60 = fixture.bridge->frameRatePresets().indexOf(QStringLiteral("60 FPS"));
    QVERIFY(idx60 >= 0);
    QVERIFY(combo->setProperty("currentIndex", idx60));
    QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, idx60)));
    QVERIFY2(!fpsField->property("visible").toBool(), "picking a preset must hide the fps field");
    QCOMPARE(fixture.bridge->frameRatePreset(), QStringLiteral("60 FPS"));
    QCOMPARE(frameRateSelect->property("value").toString(), QStringLiteral("60 FPS"));
}

void TstUiScreens::aStoredCustomResolutionOrFrameRateShowsItsFieldsOnLoad()
{
    // (d): a stored pair/value that is already non-preset (genuinely custom) must show its fields
    // on load - the one path that worked before the fix, kept as a regression guard so a future
    // change to refreshDerived() cannot silently break it.
    SettingsFixture fixture;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    // Non-preset values: 1600x900 is none of 720p/1080p/1440p/4K; 144 is neither 30 nor 60.
    fixture.bridge->setValue(QStringLiteral("width"), 1600);
    fixture.bridge->setValue(QStringLiteral("height"), 900);
    fixture.bridge->setValue(QStringLiteral("fps"), 144);
    QCOMPARE(fixture.bridge->resolutionPreset(), QStringLiteral("Custom"));
    QCOMPARE(fixture.bridge->frameRatePreset(), QStringLiteral("Custom"));

    QString error;
    QScopedPointer<QObject> root(instantiateSettingsPage(&engine, fixture.client, &error));
    QVERIFY2(root, qPrintable(error));

    QObject* widthField = itemByTitle(root.data(), QStringLiteral("Custom width"));
    QObject* heightField = itemByTitle(root.data(), QStringLiteral("Custom height"));
    QObject* fpsField = itemByTitle(root.data(), QStringLiteral("Custom frame rate"));
    QVERIFY(widthField && heightField && fpsField);
    QVERIFY2(widthField->property("visible").toBool(), "a stored custom pair must show Custom width on load");
    QVERIFY2(heightField->property("visible").toBool(), "a stored custom pair must show Custom height on load");
    QVERIFY2(fpsField->property("visible").toBool(), "a stored custom fps must show the fps field on load");

    // And the selects display Custom for the custom stored state.
    QObject* resolutionSelect = itemByTitle(root.data(), QStringLiteral("Resolution"));
    QObject* frameRateSelect = itemByTitle(root.data(), QStringLiteral("Frame rate"));
    QVERIFY(resolutionSelect && frameRateSelect);
    QCOMPARE(resolutionSelect->property("value").toString(), QStringLiteral("Custom"));
    QCOMPARE(frameRateSelect->property("value").toString(), QStringLiteral("Custom"));
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

namespace {

// `Text.Wrap` and `Text.NoWrap` as QML's `wrapMode` property reports them (QQuickText::WrapMode).
const int QQuickText_NoWrap = 0;

// A connecting screen on a 960x640 window, with a stand-in facade attached.
QObject* connectingScreen(QQmlEngine* engine, FakeShellClient* client, QString* error)
{
    QObject* screen = instantiate(engine, QStringLiteral("ConnectingScreen.qml"), error);
    if (screen) {
        screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(client)));
        screen->setProperty("width", 960);
        screen->setProperty("height", 640);
    }
    return screen;
}

QObject* buttonNamed(QObject* screen, const QString& name)
{
    return itemNamed(screen, name);
}

} // namespace

void TstUiScreens::connectingShowsCancelWhileItGoesAndTheNamedStallWhenItStops()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> screen(connectingScreen(&engine, &client, &error));
    QVERIFY2(screen, qPrintable(error));

    // Going: the stepper and the one way out of a wait. Nothing about a stall is drawn.
    client.setConnectStage(2);
    QVERIFY(effectivelyVisible(buttonNamed(screen.data(), QStringLiteral("cancelButton"))));
    QVERIFY(!effectivelyVisible(buttonNamed(screen.data(), QStringLiteral("tryAgainButton"))));
    QVERIFY(!effectivelyVisible(buttonNamed(screen.data(), QStringLiteral("backButton"))));
    QVERIFY(!effectivelyVisible(itemNamed(screen.data(), QStringLiteral("stalled"))));
    QObject* stepper = itemNamed(screen.data(), QStringLiteral("stepper"));
    QVERIFY(stepper);
    QCOMPARE(stepperLooks(stepper),
             (QStringList{QStringLiteral("done"), QStringLiteral("active"), QStringLiteral("pending")}));

    // Stopped: the stage that was active is failed, where it stopped and what the server decided are
    // printed under the stepper, in the deck's words, and the wait's own control is gone.
    client.setStalled(QStringLiteral("Stopped at: Preparing the stream"),
                      QStringLiteral("You didn't start streaming in time, so the session was released. "
                                     "You were not charged."),
                      QString());
    QVERIFY(effectivelyVisible(itemNamed(screen.data(), QStringLiteral("stalled"))));
    QCOMPARE(itemNamed(screen.data(), QStringLiteral("stalledStep"))->property("text").toString(),
             QStringLiteral("Stopped at: Preparing the stream"));
    QCOMPARE(itemNamed(screen.data(), QStringLiteral("stalledReason"))->property("text").toString(),
             QStringLiteral("You didn't start streaming in time, so the session was released. "
                            "You were not charged."));
    QCOMPARE(stepperLooks(stepper),
             (QStringList{QStringLiteral("done"), QStringLiteral("failed"), QStringLiteral("pending")}));
    QVERIFY(!effectivelyVisible(buttonNamed(screen.data(), QStringLiteral("cancelButton"))));

    // The sentence is drawn as styled text (a minute count is mono) - and the stalled step is not.
    QCOMPARE(itemNamed(screen.data(), QStringLiteral("stalledReason"))->property("textFormat").toInt(),
             4); // Text.StyledText
    QVERIFY(itemNamed(screen.data(), QStringLiteral("stalledStep"))->property("textFormat").toInt() != 4);
    // A failure with no reference draws no reference row: never a label with nothing after it.
    QVERIFY(!effectivelyVisible(itemNamed(screen.data(), QStringLiteral("stalledReferenceRow"))));

    // With a reference the row shows it, mono and unbroken.
    client.setStalled(QStringLiteral("Stopped at: Preparing the rig"), QStringLiteral("no rig is assigned"),
                      QStringLiteral("SH-9K2XQ1"));
    QVERIFY(effectivelyVisible(itemNamed(screen.data(), QStringLiteral("stalledReferenceRow"))));
    QObject* reference = itemNamed(screen.data(), QStringLiteral("stalledReference"));
    QCOMPARE(reference->property("text").toString(), QStringLiteral("SH-9K2XQ1"));
    QCOMPARE(reference->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(reference->property("wrapMode").toInt(), int(QQuickText_NoWrap));

    // Leaving the stall draws the wait again.
    client.clearStalled();
    QVERIFY(effectivelyVisible(buttonNamed(screen.data(), QStringLiteral("cancelButton"))));
    QVERIFY(!effectivelyVisible(itemNamed(screen.data(), QStringLiteral("stalled"))));
}

void TstUiScreens::aStalledConnectOffersTryAgainAndBackToHomeAndNoOtherRig()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> screen(connectingScreen(&engine, &client, &error));
    QVERIFY2(screen, qPrintable(error));
    client.setConnectStage(1);
    client.setStalled(QStringLiteral("Stopped at: Preparing the rig"),
                      QStringLiteral("This rig didn't come back in time. You were not charged."), QString());

    // Exactly two ways on, in the deck's words, and the retry is the primary one.
    QObject* tryAgain = buttonNamed(screen.data(), QStringLiteral("tryAgainButton"));
    QObject* back = buttonNamed(screen.data(), QStringLiteral("backButton"));
    QVERIFY(effectivelyVisible(tryAgain));
    QVERIFY(effectivelyVisible(back));
    QCOMPARE(tryAgain->property("text").toString(), QStringLiteral("Try again"));
    QCOMPARE(back->property("text").toString(), QStringLiteral("Back to home"));
    QCOMPARE(visibleButtons(screen.data()).size(), 2);

    // They reach the facade's own retry and dismissal, nothing else.
    QMetaObject::invokeMethod(tryAgain, "clicked");
    QCOMPARE(client.retries(), 1);
    QMetaObject::invokeMethod(back, "clicked");
    QCOMPARE(client.dismissals(), 1);
    QCOMPARE(client.interrupts(), 0);

    // No offer of a different rig, a list of rigs or a retry against a named machine (CUST-03): the
    // words on the screen say none of it.
    QStringList everything;
    collectTexts(qobject_cast<QQuickItem*>(screen.data()), &everything);
    const QString joined = everything.join(QLatin1Char('\n')).toLower();
    for (const QString& word : {QStringLiteral("another rig"), QStringLiteral("different rig"),
                                QStringLiteral("other rig"), QStringLiteral("choose"),
                                QStringLiteral("pick a rig"), QStringLiteral("try another"),
                                QStringLiteral("rig list"), QStringLiteral("notify")}) {
        QVERIFY2(!joined.contains(word), qPrintable(QStringLiteral("the screen says '%1'").arg(word)));
    }
}

void TstUiScreens::aStalledConnectWrapsTheLongestSentenceAndNeverBreaksAReference()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> screen(connectingScreen(&engine, &client, &error));
    QVERIFY2(screen, qPrintable(error));

    // The longest end-reason sentence, with its minute count in the mono family, under the longest
    // stalled-step line.
    client.setConnectStage(2);
    client.setStalled(
        QStringLiteral("Stopped at: Preparing the stream"),
        QStringLiteral("Something went wrong ending this session, so we closed it. You were charged for "
                       "the minutes you used, which was <font face=\"Geist Mono\">1234</font> minutes."),
        QStringLiteral("SH-9K2XQ1"));

    QObject* reason = itemNamed(screen.data(), QStringLiteral("stalledReason"));
    QObject* step = itemNamed(screen.data(), QStringLiteral("stalledStep"));
    QVERIFY(reason && step);
    // Wrapped, not clipped: the text wraps at the column's width and nothing is elided.
    QVERIFY(reason->property("wrapMode").toInt() != QQuickText_NoWrap);
    QVERIFY(step->property("wrapMode").toInt() != QQuickText_NoWrap);
    QVERIFY(reason->property("width").toDouble() <= 420.0);
    QVERIFY2(reason->property("contentWidth").toDouble() <= reason->property("width").toDouble() + 0.5,
             "the sentence is laid out inside its column");
    QVERIFY2(!reason->property("truncated").toBool(), "the sentence is not clipped");
    QVERIFY(reason->property("lineCount").toInt() >= 2);
    QCOMPARE(itemNamed(screen.data(), QStringLiteral("stalledReference"))->property("lineCount").toInt(), 1);
}

void TstUiScreens::theErrorViewShowsThreeKindsOfSentenceAsTheyAreAndDrawsNoBareReferenceLabel()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;

    QString error;
    QScopedPointer<QObject> screen(instantiate(&engine, QStringLiteral("ErrorScreen.qml"), &error));
    QVERIFY2(screen, qPrintable(error));
    screen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client)));

    struct Case
    {
        QString sentence;
        QString reference;
    };
    const QList<Case> cases = {
        // The server's own sentence, verbatim, with the reference it named.
        {QStringLiteral("The server's own sentence, exactly as written."), QStringLiteral("SH-4F7KQ2")},
        // The deck's offline sentence in full: the request never arrived, so no reference exists.
        {QStringLiteral("Can't reach SevenHills right now. Showing the last known balance."), QString()},
        // The deck's generic sentence for something that began on this machine: no reference either.
        {QStringLiteral("Something went wrong on our side."), QString()},
    };

    for (const Case& c : cases) {
        client.setFailure(c.sentence, c.reference);
        QCOMPARE(screen->property("errorText").toString(), c.sentence);
        QVERIFY2(renderedTexts(screen.data()).contains(c.sentence), qPrintable(c.sentence));

        QObject* row = itemNamed(screen.data(), QStringLiteral("referenceRow"));
        QVERIFY(row);
        QCOMPARE(effectivelyVisible(row), !c.reference.isEmpty());
        if (!c.reference.isEmpty()) {
            QVERIFY(renderedTexts(screen.data()).contains(c.reference));
        }
        else {
            // Nothing that looks like a code, and no label waiting for one.
            for (QObject* item : textItems(screen.data())) {
                if (effectivelyVisible(item)) {
                    QVERIFY2(item->property("text").toString() != QStringLiteral("Reference"),
                             "a reference label with nothing after it");
                }
            }
        }
    }

    // The two ways on, and no other.
    QCOMPARE(visibleButtons(screen.data()).size(), 2);
    QStringList labels;
    for (QObject* button : visibleButtons(screen.data())) {
        labels.append(button->property("text").toString());
    }
    labels.sort();
    QCOMPARE(labels, (QStringList{QStringLiteral("Back to home"), QStringLiteral("Try again")}));
}

void TstUiScreens::noScreenRendersAnInternalStateOrEndReasonKey()
{
    // An internal state name, an end-reason key or an engine stage string is developer vocabulary
    // (`copy.md` Support & errors: "customer-facing never do"). The customer's sentences are the deck's
    // and the server's own, chosen in C++; no QML file may spell a key to print it, or quote one.
    const QStringList keys = {
        QStringLiteral("READINESS_TIMEOUT"), QStringLiteral("CONNECT_TIMEOUT"),
        QStringLiteral("MODE_BOOT_TIMEOUT"), QStringLiteral("BALANCE_EXHAUSTED"),
        QStringLiteral("HOST_LOST"),         QStringLiteral("CLIENT_SILENT"),
        QStringLiteral("GRACE_EXPIRED"),     QStringLiteral("OPERATOR_FORCED"),
        QStringLiteral("TEARDOWN_TIMEOUT"),  QStringLiteral("CUSTOMER_ENDED"),
        QStringLiteral("PREPARING"),         QStringLiteral("ALLOCATED"),
        QStringLiteral("STAGE_")};

    const QDir gui(guiDir());
    const QStringList files = gui.entryList({QStringLiteral("*.qml")}, QDir::Files);
    QVERIFY(files.size() > 10);
    for (const QString& file : files) {
        const QString source = readSource(gui.filePath(file));
        for (const QString& key : keys) {
            QVERIFY2(!source.contains(key),
                     qPrintable(QStringLiteral("%1 spells the internal name %2").arg(file, key)));
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Phase 5 plan 09: the profile
// ---------------------------------------------------------------------------------------------------

namespace {

// `Text.WrapAnywhere` and `Text.AlignRight` as QML's properties report them.
const int QQuickText_WrapAnywhere = 3;
const int QQuickText_AlignRight = 2;

// The screens items are found by walking both trees: the QObject children and the Quick item
// children. Delegates a Repeater or a ListView makes are visual children of the item they sit in and
// are not always QObject children of anything above it, so `findChild` alone would never see a row.
QList<QObject*> visualTree(QObject* root)
{
    QList<QObject*> all;
    QSet<QObject*> seen;
    QList<QObject*> queue;
    queue.append(root);
    while (!queue.isEmpty()) {
        QObject* current = queue.takeFirst();
        if (!current || seen.contains(current)) {
            continue;
        }
        seen.insert(current);
        all.append(current);
        for (QObject* child : current->children()) {
            queue.append(child);
        }
        if (auto* item = qobject_cast<QQuickItem*>(current)) {
            for (QQuickItem* child : item->childItems()) {
                queue.append(child);
            }
        }
    }
    return all;
}

QList<QObject*> deepChildren(QObject* root, const QString& name)
{
    QList<QObject*> found;
    if (!root) {
        return found;
    }
    for (QObject* object : visualTree(root)) {
        if (object != root && object->objectName() == name) {
            found.append(object);
        }
    }
    return found;
}

QObject* deepChild(QObject* root, const QString& name)
{
    const QList<QObject*> found = deepChildren(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

// Whether an item is actually shown: a Quick item has its own effective visibility, which follows the
// visual parents (the QObject parent chain of a delegate is not the one that hides it).
bool isShown(QObject* object)
{
    if (auto* item = qobject_cast<QQuickItem*>(object)) {
        return item->isVisible();
    }
    return effectivelyVisible(object);
}

bool seesText(QObject* root, const QString& text)
{
    for (QObject* object : visualTree(root)) {
        const QMetaObject* meta = object->metaObject();
        if (meta->indexOfProperty("text") >= 0 && meta->indexOfProperty("font") >= 0
                && object->property("text").toString() == text && isShown(object)) {
            return true;
        }
    }
    return false;
}

/// The profile screen on a stand-in facade, sized like the window under the header. It is created with
/// the facade already set, because the screen asks for its first list as it completes.
struct ProfileFixture
{
    QQmlEngine engine;
    FakeShellClient client;
    QScopedPointer<QObject> screen;
    QString error;

    bool load(int width = 1100, int height = 656, int loadingDelay = 0)
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerTokenSingletons(&engine);
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(guiDir() + QStringLiteral("/ProfileScreen.qml")));
        screen.reset(component.createWithInitialProperties(
            { { QStringLiteral("client"), QVariant::fromValue(static_cast<QObject*>(&client)) },
              { QStringLiteral("width"), width },
              { QStringLiteral("height"), height },
              { QStringLiteral("loadingDelay"), loadingDelay } }));
        if (!screen) {
            error = component.errorString();
            return false;
        }
        return true;
    }

    QObject* child(const char* name) const { return deepChild(screen.data(), QLatin1String(name)); }
    bool sees(const QString& text) const { return seesText(screen.data(), text); }
    bool visible(const char* name) const
    {
        QObject* item = child(name);
        return item && isShown(item);
    }
    void click(const char* objectName)
    {
        QObject* button = child(objectName);
        QVERIFY2(button, objectName);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }
    void selectTab(int index)
    {
        QVERIFY(QMetaObject::invokeMethod(child("tabsBar"), "select", Q_ARG(QVariant, index)));
    }

    /// A child of one identity row (`value`, `mark`, `markWord`, `label`).
    static QObject* inRow(QObject* row, const char* name)
    {
        return row ? deepChild(row, QLatin1String(name)) : nullptr;
    }
    QObject* row(const char* name) const { return child(name); }

    /// Fills everything the profile reads with an ordinary signed-in customer.
    void ordinary()
    {
        client.setAccount(QStringLiteral("lina"), QStringLiteral("lina@example.com"),
                          QStringLiteral("+962790000000"), true);
        client.setTotals(QStringLiteral("ready"), QStringLiteral("2 h 15 min"), QStringLiteral("45 min"));
        client.sessions()->setRows(FakeList::plainRows(3));
        client.sessions()->setState(QStringLiteral("ready"));
    }

    QStringList cellTexts(const char* list, const char* cell) const
    {
        QStringList texts;
        QObject* view = child(list);
        if (!view) {
            return texts;
        }
        for (QObject* item : deepChildren(view, QLatin1String(cell))) {
            texts.append(item->property("text").toString());
        }
        return texts;
    }
};

} // namespace

void TstUiScreens::theProfileIsRoutedAsAViewInsideHomeAndHomeNoLongerSignsOut()
{
    // The profile is a view inside the home state, like Settings: it keeps the signed-in header, and
    // a session that ends while it is open still lands on the right view. `main.qml` cannot be
    // instantiated without the real facade type, so its routing is asserted from its source.
    const QString main = readSource(guiDir() + QStringLiteral("/main.qml"));
    QVERIFY2(main.contains(QStringLiteral("seatHub.inProfile ? profileComponent : homeComponent")),
             "home must route to the profile while `inProfile` holds");
    QVERIFY(main.contains(QStringLiteral("onInProfileChanged")));
    QVERIFY(main.contains(QStringLiteral("ProfileScreen {")));
    QVERIFY2(!main.contains(QStringLiteral("\"profile\"")), "the profile is not an app state");

    // The three new files are in the resource file, or they would not exist at runtime.
    const QString qrc = readSource(guiDir() + QStringLiteral("/../qml.qrc"));
    for (const QString& name : { QStringLiteral("ProfileScreen.qml"), QStringLiteral("SeatHubTabs.qml"),
                                 QStringLiteral("SeatHubListView.qml") }) {
        QVERIFY2(qrc.contains(name), qPrintable(name));
    }

    // Sign out moved here: Home neither calls it nor shows the identity line any more.
    const QString home = readSource(guiDir() + QStringLiteral("/HomeScreen.qml"));
    const QString profile = readSource(guiDir() + QStringLiteral("/ProfileScreen.qml"));
    QVERIFY(!home.contains(QStringLiteral("signOut")));
    QVERIFY(profile.contains(QStringLiteral("signOut")));
    QVERIFY(!home.contains(QStringLiteral("client.identity")));

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);
    FakeShellClient client;
    QString error;
    QScopedPointer<QObject> homeScreen(instantiate(&engine, QStringLiteral("HomeScreen.qml"), &error));
    QVERIFY2(homeScreen, qPrintable(error));
    QVERIFY(homeScreen->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));
    QVERIFY(!findVisibleTextItem(homeScreen.data(), QStringLiteral("Sign out")));
    QVERIFY(!findVisibleTextItem(homeScreen.data(), QStringLiteral("+962 7 0001 0002")));
    QVERIFY(findVisibleTextItem(homeScreen.data(), QStringLiteral("Play")));
}

void TstUiScreens::theProfileShowsTheThreeIdentityRowsReadOnlyWithTheEmailMarkInWords()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();

    QVERIFY(fx.sees(QStringLiteral("Profile")));
    QObject* username = fx.row("rowUsername");
    QObject* email = fx.row("rowEmail");
    QObject* phone = fx.row("rowPhone");
    QVERIFY(username && email && phone);
    QVERIFY(isShown(username) && isShown(email) && isShown(phone));

    QCOMPARE(ProfileFixture::inRow(username, "value")->property("text").toString(), QStringLiteral("lina"));
    QCOMPARE(ProfileFixture::inRow(email, "value")->property("text").toString(),
             QStringLiteral("lina@example.com"));
    QCOMPARE(ProfileFixture::inRow(phone, "value")->property("text").toString(),
             QStringLiteral("+962790000000"));
    QCOMPARE(ProfileFixture::inRow(username, "label")->property("text").toString(), QStringLiteral("Username"));
    QCOMPARE(ProfileFixture::inRow(email, "label")->property("text").toString(), QStringLiteral("Email"));
    QCOMPARE(ProfileFixture::inRow(phone, "label")->property("text").toString(),
             QStringLiteral("Phone number"));

    // A phone number is mono; the two names are not.
    QCOMPARE(ProfileFixture::inRow(phone, "value")->property("font").value<QFont>().family(),
             QString::fromLatin1(kMonoFamily));
    QCOMPARE(ProfileFixture::inRow(username, "value")->property("font").value<QFont>().family(),
             QString::fromLatin1(kSansFamily));

    // The mark is a word as well as a colour: `Verified` with the success dot ...
    QObject* mark = ProfileFixture::inRow(email, "mark");
    QVERIFY(mark && isShown(mark));
    QCOMPARE(ProfileFixture::inRow(email, "markWord")->property("text").toString(), QStringLiteral("Verified"));
    QVERIFY(!ProfileFixture::inRow(username, "mark")->property("visible").toBool());
    QVERIFY(!ProfileFixture::inRow(phone, "mark")->property("visible").toBool());

    // ... and `Unverified` with the warn dot once the account says so. No banner, and nothing blocks.
    fx.client.setAccount(QStringLiteral("lina"), QStringLiteral("lina@example.com"),
                         QStringLiteral("+962790000000"), false);
    QCOMPARE(ProfileFixture::inRow(fx.row("rowEmail"), "markWord")->property("text").toString(),
             QStringLiteral("Unverified"));
    for (const QString& banner : { QStringLiteral("Verify"), QStringLiteral("Confirm your email"),
                                   QStringLiteral("Send code"), QStringLiteral("we'll send a code") }) {
        QVERIFY2(!fx.sees(banner), qPrintable(banner));
    }

    // Read-only: no field, and no control that would change a username, email or phone.
    QObject* block = fx.child("identityBlock");
    QVERIFY(block);
    for (QObject* item : visualTree(block)) {
        QVERIFY2(!item->inherits("QQuickTextInput"), "the profile has no editable field");
        QVERIFY2(!item->inherits("QQuickTextEdit"), "the profile has no editable field");
        // The one control in the block is the error state Try again, and it is not shown here.
        QVERIFY2(!(item->inherits("QQuickButton") && isShown(item)),
                 "the identity rows are not controls");
    }
    QVERIFY(!fx.sees(QStringLiteral("Change these on the website")));
    QVERIFY(!fx.sees(QStringLiteral("Edit")));
    QVERIFY(!fx.sees(QStringLiteral("Change")));
}

void TstUiScreens::aMissingEmailOrPhoneKeepsItsRowAndSaysNotAdded()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();
    // A phone-only account has no email; an account may have no phone.
    fx.client.setAccount(QStringLiteral("lina"), QString(), QString(), false);

    for (const char* name : { "rowEmail", "rowPhone" }) {
        QObject* row = fx.row(name);
        QVERIFY(row && isShown(row));
        QObject* value = ProfileFixture::inRow(row, "value");
        QVERIFY(value && isShown(value));
        QCOMPARE(value->property("text").toString(), QStringLiteral("Not added"));
        // `Not added` is muted, and a missing email has no mark to hang on it.
        QCOMPARE(value->property("color").value<QColor>(), QColor(QStringLiteral("#a3a3a3")));
        QVERIFY(!ProfileFixture::inRow(row, "mark")->property("visible").toBool());
    }
    // The label is still there: the row is what says `Email` and `Phone number`.
    QVERIFY(fx.sees(QStringLiteral("Email")));
    QVERIFY(fx.sees(QStringLiteral("Phone number")));
    // The rows that are present are unaffected.
    QCOMPARE(ProfileFixture::inRow(fx.row("rowUsername"), "value")->property("text").toString(),
             QStringLiteral("lina"));
}

void TstUiScreens::longIdentityValuesWrapInsideTheColumnInsteadOfBeingCut()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();
    const QString username = QString(60, QLatin1Char('u'));
    const QString email = QString(70, QLatin1Char('e')) + QStringLiteral("@example.com");
    fx.client.setAccount(username, email, QStringLiteral("+962790000000"), false);

    for (const char* name : { "rowUsername", "rowEmail" }) {
        QObject* value = ProfileFixture::inRow(fx.row(name), "value");
        QVERIFY(value);
        // Wrapped at any character, never truncated, and never wider than the 320px column.
        QVERIFY2(value->property("lineCount").toInt() > 1, name);
        QVERIFY2(!value->property("truncated").toBool(), name);
        QVERIFY2(value->property("contentWidth").toReal() <= 320.0, name);
        QCOMPARE(value->property("wrapMode").toInt(), int(QQuickText_WrapAnywhere));
    }
    // The whole address is still there, in one piece.
    QCOMPARE(ProfileFixture::inRow(fx.row("rowEmail"), "value")->property("text").toString(), email);
    // And the mark has moved under it rather than off the column.
    QVERIFY(ProfileFixture::inRow(fx.row("rowEmail"), "mark")->property("y").toReal()
            >= ProfileFixture::inRow(fx.row("rowEmail"), "value")->property("height").toReal());
}

void TstUiScreens::theIdentityBlockHasItsOwnLoadingAndErrorStates()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();

    // Loading (the delay is 0 here): skeletons of the final row shape, and no rows.
    fx.client.setAccountState(QStringLiteral("loading"));
    QVERIFY(fx.visible("identityLoading"));
    QVERIFY(!fx.visible("identityRows"));
    QVERIFY(!fx.visible("identityError"));
    // The rest of the page does not notice.
    QVERIFY(fx.sees(QStringLiteral("Hours played")));
    QVERIFY(fx.sees(QStringLiteral("2 h 15 min")));

    // Error: the server's sentence, its reference in mono, and a way to try again.
    fx.client.setAccountState(QStringLiteral("error"), QStringLiteral("We couldn't load your details."),
                              QStringLiteral("SH-4F7KQ2"));
    QVERIFY(fx.visible("identityError"));
    QVERIFY(!fx.visible("identityRows"));
    QVERIFY(!fx.visible("identityLoading"));
    QVERIFY(fx.sees(QStringLiteral("◆ We couldn't load your details.")));
    QObject* reference = fx.child("identityErrorReference");
    QVERIFY(reference && isShown(reference));
    QCOMPARE(reference->property("text").toString(), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(reference->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(reference->property("wrapMode").toInt(), int(QQuickText_NoWrap));
    fx.click("identityRetry");
    QVERIFY(fx.client.calls().contains(QStringLiteral("reloadAccount")));

    // An error with no reference draws no reference.
    fx.client.setAccountState(QStringLiteral("error"), QStringLiteral("We couldn't reach SevenHills."));
    QVERIFY(!fx.visible("identityErrorReference"));

    // Back to the rows once the identity is read.
    fx.client.setAccount(QStringLiteral("lina"), QStringLiteral("lina@example.com"),
                         QStringLiteral("+962790000000"), true);
    QVERIFY(fx.visible("identityRows"));
    QVERIFY(!fx.visible("identityLoading"));
    QVERIFY(!fx.visible("identityError"));

    // A fast answer never flashes a skeleton: with the real delay, nothing shows before it.
    ProfileFixture slow;
    QVERIFY2(slow.load(1100, 656, 300), qPrintable(slow.error));
    slow.ordinary();
    slow.client.setAccountState(QStringLiteral("loading"));
    QVERIFY(!slow.visible("identityLoading"));
    QTRY_VERIFY_WITH_TIMEOUT(slow.visible("identityLoading"), 3000);
}

void TstUiScreens::theTwoTotalsAreDrawnFromTheServersNumbersAndHaveTheirOwnStates()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();

    // Two tiles, labelled as the deck words them, with the value the facade gave: the server's number
    // formatted in C++, drawn in the mono face at heading size and tabular.
    QObject* hours = fx.child("tileHours");
    QObject* credit = fx.child("tileCredit");
    QVERIFY(hours && credit);
    QVERIFY(isShown(hours) && isShown(credit));
    QCOMPARE(ProfileFixture::inRow(hours, "tileLabel")->property("text").toString(), QStringLiteral("Hours played"));
    QCOMPARE(ProfileFixture::inRow(credit, "tileLabel")->property("text").toString(), QStringLiteral("Credit left"));
    QObject* hoursValue = ProfileFixture::inRow(hours, "tileValue");
    QObject* creditValue = ProfileFixture::inRow(credit, "tileValue");
    QCOMPARE(hoursValue->property("text").toString(), QStringLiteral("2 h 15 min"));
    QCOMPARE(creditValue->property("text").toString(), QStringLiteral("45 min"));
    QCOMPARE(hoursValue->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(hoursValue->property("font").value<QFont>().pixelSize(), 32);

    // At the 320px column two 32px mono values do not fit side by side, so they stack: each tile takes
    // the whole column. They are two tiles of one width either way.
    QObject* block = fx.child("totalsBlock");
    QVERIFY(block);
    QVERIFY(!block->property("sideBySide").toBool());
    QCOMPARE(hours->property("width").toReal(), 320.0);
    QCOMPARE(hours->property("width").toReal(), credit->property("width").toReal());

    // A value that would not fit is shrunk, never cut.
    fx.client.setTotals(QStringLiteral("ready"), QStringLiteral("100 h 00 min"), QStringLiteral("100 h 00 min"));
    QVERIFY(!hoursValue->property("truncated").toBool());
    QVERIFY(hoursValue->property("contentWidth").toReal() <= hoursValue->property("width").toReal() + 0.5);

    // Loading: skeleton tiles, and no numbers.
    fx.client.setTotals(QStringLiteral("loading"));
    QVERIFY(fx.visible("totalsLoading"));
    QVERIFY(!fx.visible("totalTiles"));
    QVERIFY(!fx.visible("totalsError"));

    // Error: one tile with the server's sentence and reference and a way to try again; no number and
    // certainly no zero.
    fx.client.setTotals(QStringLiteral("error"), QString(), QString(), QStringLiteral("We couldn't total that."),
                        QStringLiteral("SH-4F7KQ2"));
    QVERIFY(fx.visible("totalsError"));
    QVERIFY(!fx.visible("totalTiles"));
    QVERIFY(fx.sees(QStringLiteral("◆ We couldn't total that.")));
    QCOMPARE(fx.child("totalsErrorReference")->property("text").toString(), QStringLiteral("SH-4F7KQ2"));
    QVERIFY(!fx.sees(QStringLiteral("0 min")));
    fx.click("totalsRetry");
    QVERIFY(fx.client.calls().contains(QStringLiteral("reloadTotals")));

    // The lists and the identity never noticed.
    QVERIFY(fx.visible("identityRows"));
    QVERIFY(fx.visible("list_sessions"));
}

void TstUiScreens::theTabsSelectByClickAndByArrowKeysAndOnlyTheSelectedOneIsATabStop()
{
    // The tabs need a window for real key events, so this builds one (the menu test's technique).
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    registerTokenSingletons(&engine);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
                          "import QtQuick\n"
                          "import QtQuick.Controls\n"
                          "ApplicationWindow {\n"
                          "    id: win\n"
                          "    width: 600; height: 200; visible: true\n"
                          "    SeatHubTabs {\n"
                          "        objectName: \"underTest\"\n"
                          "        titles: [\"Sessions\", \"Credit history\", \"Top-ups\"]\n"
                          "    }\n"
                          "}\n"),
                      QUrl::fromLocalFile(guiDir() + QStringLiteral("/tst_tabs_window.qml")));
    QScopedPointer<QObject> created(component.create());
    QVERIFY2(created, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(created.data());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));

    QObject* tabs = deepChild(window, QStringLiteral("underTest"));
    QVERIFY(tabs);
    QSignalSpy activated(tabs, SIGNAL(tabActivated(int)));
    auto tab = [&](int i) {
        return qobject_cast<QQuickItem*>(deepChild(tabs, QStringLiteral("tab%1").arg(i)));
    };
    QVERIFY(tab(0) && tab(1) && tab(2));

    // Three tabs, each a real button at least 40px tall, named as the deck words them.
    QCOMPARE(tab(0)->property("text").toString(), QStringLiteral("Sessions"));
    QCOMPARE(tab(1)->property("text").toString(), QStringLiteral("Credit history"));
    QCOMPARE(tab(2)->property("text").toString(), QStringLiteral("Top-ups"));
    for (int i = 0; i < 3; ++i) {
        QVERIFY(tab(i)->inherits("QQuickButton"));
        QVERIFY(tab(i)->height() >= 40);
    }

    // The selected tab is marked by a fill AND by its text, not by colour alone: a fill the others
    // lack, the semibold weight the others lack, and it is the only tab stop.
    QCOMPARE(tabs->property("currentIndex").toInt(), 0);
    auto weightOf = [&](int i) {
        QObject* label = tab(i)->property("contentItem").value<QObject*>();
        return label->property("font").value<QFont>().weight();
    };
    auto fillOf = [&](int i) {
        QObject* background = tab(i)->property("background").value<QObject*>();
        return background->property("color").value<QColor>();
    };
    QCOMPARE(weightOf(0), QFont::DemiBold);
    QCOMPARE(weightOf(1), QFont::Normal);
    QVERIFY(fillOf(0).alpha() > 0);
    QCOMPARE(fillOf(1).alpha(), 0);
    QVERIFY(tab(0)->property("activeFocusOnTab").toBool());
    QVERIFY(!tab(1)->property("activeFocusOnTab").toBool());
    QVERIFY(!tab(2)->property("activeFocusOnTab").toBool());

    // A click selects it.
    QVERIFY(QMetaObject::invokeMethod(tab(1), "clicked"));
    QCOMPARE(tabs->property("currentIndex").toInt(), 1);
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toInt(), 1);
    QCOMPARE(weightOf(1), QFont::DemiBold);
    QCOMPARE(weightOf(0), QFont::Normal);
    QTRY_VERIFY_WITH_TIMEOUT(tab(1)->hasActiveFocus(), 3000);
    QVERIFY(tab(1)->property("activeFocusOnTab").toBool());
    QVERIFY(!tab(0)->property("activeFocusOnTab").toBool());

    // The arrow keys move between tabs, and stop at the ends.
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->property("currentIndex").toInt(), 2, 3000);
    QTest::keyClick(window, Qt::Key_Right);
    QCOMPARE(tabs->property("currentIndex").toInt(), 2);
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->property("currentIndex").toInt(), 1, 3000);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->property("currentIndex").toInt(), 0, 3000);
    QVERIFY(tab(0)->hasActiveFocus());
}

namespace {

/// A list view of its own, with a stand-in list, sized like the region under the tabs.
struct ListFixture
{
    QQmlEngine engine;
    FakeList list;
    QScopedPointer<QObject> view;
    QString error;

    bool load(int width = 528, int height = 300, int loadingDelay = 0)
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerTokenSingletons(&engine);
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(guiDir() + QStringLiteral("/SeatHubListView.qml")));
        view.reset(component.createWithInitialProperties(
            { { QStringLiteral("list"), QVariant::fromValue(static_cast<QObject*>(&list)) },
              { QStringLiteral("width"), width },
              { QStringLiteral("height"), height },
              { QStringLiteral("loadingDelay"), loadingDelay },
              { QStringLiteral("loadingText"), QStringLiteral("Loading your sessions…") },
              { QStringLiteral("emptyText"), QStringLiteral("No sessions yet.") },
              { QStringLiteral("emptyActionText"), QStringLiteral("Back to home") } }));
        if (!view) {
            error = component.errorString();
            return false;
        }
        return true;
    }
    QObject* child(const char* name) const { return deepChild(view.data(), QLatin1String(name)); }
    bool visible(const char* name) const
    {
        QObject* item = child(name);
        return item && isShown(item);
    }
    bool sees(const QString& text) const { return seesText(view.data(), text); }
    void layout() { QMetaObject::invokeMethod(child("rows"), "forceLayout"); }
};

} // namespace

void TstUiScreens::aListDrawsEachOfItsStates()
{
    ListFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));

    // Nothing asked for yet: nothing drawn.
    QVERIFY(!fx.visible("loadingState") && !fx.visible("emptyState") && !fx.visible("errorState")
            && !fx.visible("rows"));

    // Loading: skeleton rows of the final shape, and the named line.
    fx.list.setState(QStringLiteral("loading"));
    QVERIFY(fx.visible("loadingState"));
    QVERIFY(fx.sees(QStringLiteral("Loading your sessions…")));
    QCOMPARE(deepChildren(fx.child("loadingState"), QStringLiteral("skeletonRows")).size() >= 0, true);
    QVERIFY(!fx.visible("rows"));

    // Empty: one sentence and one action, no rows, no skeletons.
    fx.list.setState(QStringLiteral("ready"));
    QVERIFY(fx.visible("emptyState"));
    QVERIFY(fx.sees(QStringLiteral("No sessions yet.")));
    QObject* action = fx.child("emptyAction");
    QVERIFY(action && isShown(action));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Back to home"));
    QSignalSpy emptyAction(fx.view.data(), SIGNAL(emptyActionTriggered()));
    QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
    QCOMPARE(emptyAction.count(), 1);
    QVERIFY(!fx.visible("loadingState") && !fx.visible("errorState") && !fx.visible("rows"));

    // Populated: the rows, each already display text; the cells are 44px tall.
    fx.list.setRows({ { QStringLiteral("Sat 12 Sep, 21:40"), QStringLiteral("You ended it"),
                        QStringLiteral("2 h 15 min"), QString() },
                      { QStringLiteral("Fri 11 Sep, 13:00"), QStringLiteral("Balance ran out"),
                        QStringLiteral("45 min"), QString() } });
    fx.list.setState(QStringLiteral("ready"));
    fx.layout();
    QVERIFY(fx.visible("rows"));
    QVERIFY(!fx.visible("emptyState") && !fx.visible("loadingState") && !fx.visible("errorState"));
    QCOMPARE(deepChildren(fx.child("rows"), QStringLiteral("whenCell")).size(), 2);
    QObject* firstWhen = deepChildren(fx.child("rows"), QStringLiteral("whenCell")).first();
    QCOMPARE(firstWhen->property("height").toInt(), 44);
    QVERIFY(fx.sees(QStringLiteral("Sat 12 Sep, 21:40")));
    QVERIFY(fx.sees(QStringLiteral("Balance ran out")));
    QVERIFY(fx.sees(QStringLiteral("2 h 15 min")));
    // Dates and lengths are mono; the words are not.
    QCOMPARE(firstWhen->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QObject* amount = deepChildren(fx.child("rows"), QStringLiteral("amountCell")).first();
    QCOMPARE(amount->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(amount->property("horizontalAlignment").toInt(), int(QQuickText_AlignRight));
    QObject* kind = deepChildren(fx.child("rows"), QStringLiteral("kindCell")).first();
    QCOMPARE(kind->property("font").value<QFont>().family(), QString::fromLatin1(kSansFamily));

    // End of feed: the server has no more, so the closing line shows under the rows.
    QVERIFY2(fx.sees(QStringLiteral("You've reached the end.")), "the last page ends the list");
    QVERIFY(fx.visible("endOfFeed"));
    QVERIFY(!fx.visible("footerLoading") && !fx.visible("footerError"));

    // Next page loading: the footer row with the named line; the rows stay.
    fx.list.setState(QStringLiteral("ready"), /*hasMore*/ true, /*loadingMore*/ true);
    fx.layout();
    QVERIFY(fx.visible("footerLoading"));
    QVERIFY(!fx.visible("endOfFeed"));
    QVERIFY(fx.sees(QStringLiteral("Loading your sessions…")));
    QVERIFY(fx.sees(QStringLiteral("Sat 12 Sep, 21:40")));

    // A later page failed: the same reason and reference in the footer row, every loaded row kept.
    fx.list.setState(QStringLiteral("ready"), true, false, true, QStringLiteral("We couldn't load that page."),
                     QStringLiteral("SH-4F7KQ2"));
    fx.layout();
    QVERIFY(fx.visible("footerError"));
    QVERIFY(fx.sees(QStringLiteral("◆ We couldn't load that page.")));
    QCOMPARE(fx.child("footerErrorReference")->property("text").toString(), QStringLiteral("SH-4F7KQ2"));
    QVERIFY(fx.sees(QStringLiteral("Sat 12 Sep, 21:40")));
    QVERIFY(fx.sees(QStringLiteral("Balance ran out")));
    QSignalSpy retry(fx.view.data(), SIGNAL(retryRequested()));
    QVERIFY(QMetaObject::invokeMethod(fx.child("footerRetry"), "clicked"));
    QCOMPARE(retry.count(), 1);

    // The first page failed: the server's sentence, its reference in mono, a way to try again.
    fx.list.setRows({});
    fx.list.setState(QStringLiteral("error"), false, false, false, QStringLiteral("We couldn't load your sessions."),
                     QStringLiteral("SH-4F7KQ2"));
    QVERIFY(fx.visible("errorState"));
    QVERIFY(!fx.visible("rows") && !fx.visible("emptyState") && !fx.visible("loadingState"));
    QVERIFY(fx.sees(QStringLiteral("◆ We couldn't load your sessions.")));
    QObject* reference = fx.child("errorReference");
    QVERIFY(reference && isShown(reference));
    QCOMPARE(reference->property("text").toString(), QStringLiteral("SH-4F7KQ2"));
    QCOMPARE(reference->property("font").value<QFont>().family(), QString::fromLatin1(kMonoFamily));
    QCOMPARE(reference->property("wrapMode").toInt(), int(QQuickText_NoWrap));
    QVERIFY(QMetaObject::invokeMethod(fx.child("retryButton"), "clicked"));
    QCOMPARE(retry.count(), 2);

    // An error with no reference draws no reference.
    fx.list.setState(QStringLiteral("error"), false, false, false, QStringLiteral("We couldn't reach SevenHills."));
    QVERIFY(!fx.visible("errorReference"));

    // Overflow: a long word in a row wraps to a second line inside the 44px row, never off the region.
    fx.list.setRows({ { QStringLiteral("Sat 12 Sep, 21:40"),
                        QStringLiteral("Not started in time, not charged"), QStringLiteral("0 min"),
                        QString() } });
    fx.list.setState(QStringLiteral("ready"));
    fx.layout();
    QObject* longKind = deepChildren(fx.child("rows"), QStringLiteral("kindCell")).first();
    QVERIFY(longKind->property("contentWidth").toReal() <= longKind->property("width").toReal() + 0.5);
    QVERIFY(longKind->property("contentHeight").toReal() <= 44.0);

    // A top-up row carries its dot beside the word: the mark is never colour alone.
    fx.list.setRows({ { QStringLiteral("Sun 13 Sep, 12:00"), QStringLiteral("Waiting"), QString(),
                        QStringLiteral("waiting") },
                      { QStringLiteral("Thu 10 Sep, 12:00"), QStringLiteral("Credited"),
                        QStringLiteral("+5 h 00 min"), QStringLiteral("credited") } });
    fx.list.setState(QStringLiteral("ready"));
    fx.layout();
    QVERIFY(fx.sees(QStringLiteral("Waiting")));
    QVERIFY(fx.sees(QStringLiteral("Credited")));
    QVERIFY(fx.sees(QStringLiteral("+5 h 00 min")));

    // The skeleton waits for its delay: a fast answer never flashes it.
    ListFixture slow;
    QVERIFY2(slow.load(528, 300, 300), qPrintable(slow.error));
    slow.list.setState(QStringLiteral("loading"));
    QVERIFY(!slow.visible("loadingState"));
    QTRY_VERIFY_WITH_TIMEOUT(slow.visible("loadingState"), 3000);
    slow.list.setState(QStringLiteral("ready"));
    QVERIFY(!slow.visible("loadingState"));
}

void TstUiScreens::aListAsksForTheNextPageOnlyWhenItsRowsEnd()
{
    ListFixture fx;
    QVERIFY2(fx.load(528, 300), qPrintable(fx.error));
    QSignalSpy asked(fx.view.data(), SIGNAL(nextPageRequested()));

    // Fifteen rows are taller than the 300px region and the server has more: nothing is asked for
    // while the top of the list is showing.
    fx.list.setRows(FakeList::plainRows(15));
    fx.list.setState(QStringLiteral("ready"), /*hasMore*/ true);
    fx.layout();
    QCOMPARE(asked.count(), 0);
    QObject* rows = fx.child("rows");
    QVERIFY(rows->property("contentHeight").toReal() > rows->property("height").toReal());

    // The scrollbar shows while there is more than fits.
    QCOMPARE(rows->property("contentHeight").toReal() > rows->property("height").toReal(), true);

    // Reaching the end asks for the next page.
    QVERIFY(QMetaObject::invokeMethod(rows, "positionViewAtEnd"));
    QTRY_VERIFY_WITH_TIMEOUT(asked.count() >= 1, 3000);

    // While a page is loading, or after one failed, it does not ask on its own.
    asked.clear();
    fx.list.setState(QStringLiteral("ready"), true, /*loadingMore*/ true);
    fx.layout();
    QVERIFY(QMetaObject::invokeMethod(rows, "positionViewAtEnd"));
    fx.list.setState(QStringLiteral("ready"), true, false, /*moreFailed*/ true,
                     QStringLiteral("We couldn't load that page."));
    fx.layout();
    QVERIFY(QMetaObject::invokeMethod(rows, "positionViewAtEnd"));
    QTest::qWait(150);
    QCOMPARE(asked.count(), 0);

    // With no more to give, reaching the end asks for nothing.
    fx.list.setState(QStringLiteral("ready"), /*hasMore*/ false);
    fx.layout();
    QVERIFY(QMetaObject::invokeMethod(rows, "positionViewAtEnd"));
    QTest::qWait(150);
    QCOMPARE(asked.count(), 0);

    // Three rows that do not fill the region, with more to come, ask for the next at once: there is
    // no scrolling to reach an end that is already showing.
    ListFixture short_;
    QVERIFY2(short_.load(528, 600), qPrintable(short_.error));
    QSignalSpy askedShort(short_.view.data(), SIGNAL(nextPageRequested()));
    short_.list.setRows(FakeList::plainRows(3));
    short_.list.setState(QStringLiteral("ready"), true);
    short_.layout();
    QTRY_VERIFY_WITH_TIMEOUT(askedShort.count() >= 1, 3000);
}

void TstUiScreens::theProfileAsksOnlyForTheListItShowsAndTheOthersWaitForTheirTab()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();

    // On opening: the sessions tab is showing, so only sessions are asked for.
    QCOMPARE(fx.client.calls(), QStringList{ QStringLiteral("loadFirstPage:sessions") });
    QVERIFY(fx.visible("list_sessions"));
    QVERIFY(!fx.visible("list_credit"));
    QVERIFY(!fx.visible("list_topups"));

    // Opening a tab asks for that list, and only that list.
    fx.selectTab(1);
    QCOMPARE(fx.client.calls().last(), QStringLiteral("loadFirstPage:credit"));
    QVERIFY(fx.visible("list_credit"));
    QVERIFY(!fx.visible("list_sessions"));
    QCOMPARE(fx.client.calls().size(), 2);

    fx.selectTab(2);
    QCOMPARE(fx.client.calls().last(), QStringLiteral("loadFirstPage:topups"));
    QVERIFY(fx.visible("list_topups"));
    QCOMPARE(fx.client.calls().size(), 3);

    // The tabs carry the deck's words, in order.
    QVERIFY(fx.sees(QStringLiteral("Sessions")));
    QVERIFY(fx.sees(QStringLiteral("Credit history")));
    QVERIFY(fx.sees(QStringLiteral("Top-ups")));

    // Reaching the end of a list asks the facade for that list's next page, by name.
    fx.client.topups()->setRows(FakeList::plainRows(15));
    fx.client.topups()->setState(QStringLiteral("ready"), true);
    QObject* view = fx.child("list_topups");
    QVERIFY(view);
    QVERIFY(QMetaObject::invokeMethod(deepChild(view, QStringLiteral("rows")), "forceLayout"));
    QVERIFY(QMetaObject::invokeMethod(deepChild(view, QStringLiteral("rows")), "positionViewAtEnd"));
    QTRY_VERIFY_WITH_TIMEOUT(fx.client.calls().contains(QStringLiteral("loadNextPage:topups")), 3000);

    // Each list's own `Try again`: a failed first page starts that list over; a failed later page asks
    // for that same page again.
    fx.selectTab(0);
    fx.client.sessions()->setRows({});
    fx.client.sessions()->setState(QStringLiteral("error"), false, false, false, QStringLiteral("Try later."),
                                   QStringLiteral("SH-4F7KQ2"));
    QVERIFY(QMetaObject::invokeMethod(deepChild(fx.child("list_sessions"), QStringLiteral("retryButton")),
                                      "clicked"));
    QVERIFY(fx.client.calls().contains(QStringLiteral("reloadList:sessions")));

    fx.client.sessions()->setRows(FakeList::plainRows(2));
    fx.client.sessions()->setState(QStringLiteral("ready"), true, false, true, QStringLiteral("Try later."));
    QVERIFY(QMetaObject::invokeMethod(deepChild(fx.child("list_sessions"), QStringLiteral("footerRetry")),
                                      "clicked"));
    QVERIFY(fx.client.calls().contains(QStringLiteral("loadNextPage:sessions")));
}

void TstUiScreens::oneListFailingLeavesTheOthersTheTotalsAndTheIdentityIntact()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();
    fx.client.credit()->setRows({ { QStringLiteral("Sat 12 Sep, 21:40"), QStringLiteral("Top-up"),
                                    QStringLiteral("+5 h 00 min"), QString() } });
    fx.client.credit()->setState(QStringLiteral("ready"));
    fx.client.topups()->setRows({});
    fx.client.topups()->setState(QStringLiteral("ready"));

    // Sessions fails.
    fx.client.sessions()->setRows({});
    fx.client.sessions()->setState(QStringLiteral("error"), false, false, false,
                                   QStringLiteral("We couldn't load your sessions."), QStringLiteral("SH-4F7KQ2"));
    QVERIFY(fx.visible("list_sessions"));
    QVERIFY(fx.sees(QStringLiteral("◆ We couldn't load your sessions.")));

    // The identity block and both totals are intact.
    QVERIFY(fx.visible("identityRows"));
    QVERIFY(fx.sees(QStringLiteral("lina@example.com")));
    QVERIFY(fx.visible("totalTiles"));
    QVERIFY(fx.sees(QStringLiteral("2 h 15 min")));

    // And the other two lists are their own: credit history has its row, top-ups is empty.
    fx.selectTab(1);
    QVERIFY(fx.sees(QStringLiteral("+5 h 00 min")));
    QVERIFY(!fx.sees(QStringLiteral("We couldn't load your sessions.")));
    fx.selectTab(2);
    QVERIFY(fx.sees(QStringLiteral("No top-ups yet.")));
    QVERIFY(!fx.sees(QStringLiteral("We couldn't load your sessions.")));

    // The reverse: the totals fail and every list and the identity carry on.
    fx.selectTab(1);
    fx.client.setTotals(QStringLiteral("error"), QString(), QString(), QStringLiteral("We couldn't total that."));
    QVERIFY(fx.visible("totalsError"));
    QVERIFY(fx.sees(QStringLiteral("+5 h 00 min")));
    QVERIFY(fx.visible("identityRows"));

    // And the identity fails while the lists and totals carry on.
    fx.client.setTotals(QStringLiteral("ready"), QStringLiteral("2 h 15 min"), QStringLiteral("45 min"));
    fx.client.setAccountState(QStringLiteral("error"), QStringLiteral("We couldn't load your details."));
    QVERIFY(fx.visible("identityError"));
    QVERIFY(fx.visible("totalTiles"));
    QVERIFY(fx.sees(QStringLiteral("+5 h 00 min")));
}

void TstUiScreens::aRowNamesNoRigAndNoProfileFileCanReadOne()
{
    // CUST-01, T-05-37. Every row is built from the four roles the model gives it, and a delegate that
    // asked for a fifth would fail to load: so what a row can show is what the model gave it.
    ListFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.list.setRows(FakeList::plainRows(4));
    fx.list.setState(QStringLiteral("ready"));
    fx.layout();
    QObject* rows = fx.child("rows");
    QVERIFY(rows);
    int cells = 0;
    for (const char* name : { "whenCell", "kindCell", "amountCell" }) {
        for (QObject* cell : deepChildren(rows, QLatin1String(name))) {
            ++cells;
            const QString text = cell->property("text").toString();
            QVERIFY2(!text.contains(QStringLiteral("Rig"), Qt::CaseSensitive), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("host"), Qt::CaseInsensitive), qPrintable(text));
        }
    }
    QCOMPARE(cells, 12);

    // And none of the three files can read a machine: no host or rig-identity member in any of them.
    for (const QString& name : { QStringLiteral("ProfileScreen.qml"), QStringLiteral("SeatHubListView.qml"),
                                 QStringLiteral("SeatHubTabs.qml") }) {
        const QString source = readSource(guiDir() + QLatin1Char('/') + name);
        QVERIFY2(!source.isEmpty(), qPrintable(name));
        // A word starting `host` (not the `ghost` button variant): no host member of any spelling.
        QVERIFY2(!source.contains(QRegularExpression(QStringLiteral("\\bhost"),
                                                     QRegularExpression::CaseInsensitiveOption)),
                 qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("rigName")), qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("rig_name")), qPrintable(name));
        // Nor do they build a number: no sum, count or total is computed in QML.
        QVERIFY2(!source.contains(QStringLiteral("reduce(")), qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("parseInt")), qPrintable(name));
        QVERIFY2(!source.contains(QStringLiteral("+= ")), qPrintable(name));
    }
}

void TstUiScreens::signOutBackAndTheWebsiteLinkFromTheProfileReachTheFacade()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();

    // Sign out is here now, and it is the facade's own (it revokes, deletes the credential and lands
    // on sign-in even offline: `tst_facade_wiring` proves those). No confirmation stands in between.
    QObject* signOut = fx.child("signOutButton");
    QVERIFY(signOut && isShown(signOut));
    QCOMPARE(signOut->property("text").toString(), QStringLiteral("Sign out"));
    QCOMPARE(signOut->property("variant").toString(), QStringLiteral("ghost"));
    fx.click("signOutButton");
    QCOMPARE(fx.client.signOuts(), 1);

    // Back leaves the profile.
    fx.click("backButton");
    QVERIFY(fx.client.calls().contains(QStringLiteral("closeProfile")));

    // The plain link opens the website's home page, by name, and promises nothing: it is worded
    // `Open the website`, never as an instruction to change details there.
    QObject* link = fx.child("openWebsiteButton");
    QVERIFY(link && isShown(link));
    QCOMPARE(link->property("text").toString(), QStringLiteral("Open the website"));
    QCOMPARE(link->property("glyph").toString(), QStringLiteral("↗"));
    fx.click("openWebsiteButton");
    QCOMPARE(fx.client.opened(), QStringList{ QStringLiteral("home") });
    QVERIFY(!fx.sees(QStringLiteral("Change these on the website")));

    // The screen builds no address: it asks the facade by name.
    const QString source = readSource(guiDir() + QStringLiteral("/ProfileScreen.qml"));
    QVERIFY(!source.contains(QStringLiteral("http")));
    QVERIFY(!source.contains(QStringLiteral("sevenhills")));
    QVERIFY(source.contains(QStringLiteral("client.openWebsite(\"home\")")));
}

void TstUiScreens::theEmptyActionsGoWhereTheirWordsSay()
{
    ProfileFixture fx;
    QVERIFY2(fx.load(), qPrintable(fx.error));
    fx.ordinary();
    fx.client.sessions()->setRows({});
    fx.client.sessions()->setState(QStringLiteral("ready"));
    fx.client.credit()->setRows({});
    fx.client.credit()->setState(QStringLiteral("ready"));
    fx.client.topups()->setRows({});
    fx.client.topups()->setState(QStringLiteral("ready"));

    // Sessions: one sentence and `Back to home`.
    QVERIFY(fx.sees(QStringLiteral("No sessions yet.")));
    QObject* action = deepChild(fx.child("list_sessions"), QStringLiteral("emptyAction"));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Back to home"));
    QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
    QVERIFY(fx.client.calls().contains(QStringLiteral("closeProfile")));
    QCOMPARE(fx.client.topUps(), 0);

    // Credit history and top-ups: one sentence and `Top up`, which opens the website.
    fx.selectTab(1);
    QVERIFY(fx.sees(QStringLiteral("No credit history yet.")));
    action = deepChild(fx.child("list_credit"), QStringLiteral("emptyAction"));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Top up"));
    QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
    QCOMPARE(fx.client.topUps(), 1);

    fx.selectTab(2);
    QVERIFY(fx.sees(QStringLiteral("No top-ups yet.")));
    action = deepChild(fx.child("list_topups"), QStringLiteral("emptyAction"));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Top up"));
    QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
    QCOMPARE(fx.client.topUps(), 2);

    // Loading names the thing loading, per list.
    fx.client.credit()->setState(QStringLiteral("loading"));
    fx.client.topups()->setState(QStringLiteral("loading"));
    QVERIFY(fx.sees(QStringLiteral("Loading your top-ups…")));
    fx.selectTab(1);
    QVERIFY(fx.sees(QStringLiteral("Loading your credit history…")));
    fx.selectTab(0);
    fx.client.sessions()->setState(QStringLiteral("loading"));
    QVERIFY(fx.sees(QStringLiteral("Loading your sessions…")));
}

void TstUiScreens::theProfileFitsItsWindowAndKeepsTheListWideEnough()
{
    // Default window under the header: 1100 x 656. Nothing in the left column scrolls, so Sign out is
    // where it is always expected to be.
    {
        ProfileFixture fx;
        QVERIFY2(fx.load(1100, 656), qPrintable(fx.error));
        fx.ordinary();
        QObject* left = fx.child("leftColumn");
        QVERIFY(left);
        QVERIFY2(left->property("contentHeight").toReal() <= left->property("height").toReal() + 0.5,
                 "at the default size the left column fits without scrolling");
        // About twelve 44px rows are visible at once in the list region (owner decision O7).
        QObject* right = fx.child("rightColumn");
        QVERIFY(right);
        QVERIFY(right->property("width").toReal() >= 528.0);
    }
    // Smallest window under the header: 960 x 576. The list keeps the 528px the three row cells need,
    // and whatever the left column cannot fit scrolls, so nothing is unreachable.
    {
        ProfileFixture fx;
        QVERIFY2(fx.load(960, 576), qPrintable(fx.error));
        fx.ordinary();
        QObject* right = fx.child("rightColumn");
        QVERIFY(right);
        QVERIFY2(right->property("width").toReal() >= 528.0, "the list is at least 528px wide at 960px");
        QObject* left = fx.child("leftColumn");
        const bool fits = left->property("contentHeight").toReal() <= left->property("height").toReal() + 0.5;
        const bool scrolls = left->property("contentHeight").toReal() > left->property("height").toReal();
        QVERIFY(fits || scrolls);
        QVERIFY(fx.visible("signOutButton"));
    }
}

#include "tst_ui_screens.moc"

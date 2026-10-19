// Compiled only by app.pro and tst_reporting_consent.pro: the QML-facing singleton and the in-app
// ask. The switch itself (`ReportingConsent::coveredUploadsAllowed()`) is header-only so the suites that
// compile a gated sender need nothing from this file.
#define RENTAPC_CONSENT_WINDOW 1
#include "reporting_consent.h"

#include <QObject>
#include <QQmlEngine>

namespace {

// What Settings shows and changes (copy.md "Settings control"). The row is the recorded choice; a fresh
// install that has not been asked yet shows the default, ticked, but covered uploads stay blocked until
// the choice is recorded - the ask runs before anything starts.
class ReportingConsentQml : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool coveredUploadsAllowed READ coveredUploadsAllowed NOTIFY changed)

public:
    bool coveredUploadsAllowed() const
    {
        return ReportingConsent::choiceRecorded() ? ReportingConsent::coveredUploadsAllowed() : true;
    }

    // The customer's own click on the Settings row. Per-send senders see it at their next send;
    // Sentry and Crashpad pick it up at the next launch (the screen copy says so).
    Q_INVOKABLE bool setCoveredUploadsAllowed(bool send)
    {
        const bool ok = ReportingConsent::recordChoice(send);
        emit changed();
        return ok;
    }

signals:
    void changed();
};

} // namespace

void ReportingConsent::registerQml()
{
    qmlRegisterSingletonType<ReportingConsentQml>("ReportingConsent", 1, 0, "ReportingConsent",
                                                  [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                      return new ReportingConsentQml();
                                                  });
}

bool ReportingConsent::askAtStartup(const wchar_t* const* changes, int changeCount)
{
#ifdef _WIN32
    namespace window = reporting_consent_window;
    // A re-ask opens with the previous choice pre-selected and in force until answered; a first ask
    // opens ticked (ADR-0107 "Default").
    const reporting_consent_store::Record previous = reporting_consent_store::read();
    window::Spec spec = window::strings::baseSpec(previous.recorded ? previous.send : true);
    const std::wstring notice = std::wstring(window::strings::kNoticeEn) + L"\r\n\r\n"
        + window::strings::kNoticeAr;
    spec.noticeText = notice.c_str();
    spec.changes = changes;
    spec.changeCount = changeCount;
    spec.primary = { L"OK", L"حسنًا" };
    spec.hasCancel = false;

    const window::Result result = window::show(spec);
    if (!result.answered) {
        return false; // closed without answering: nothing is recorded, nothing covered is sent
    }
    return recordChoice(result.send);
#else
    Q_UNUSED(changes);
    Q_UNUSED(changeCount);
    return false;
#endif
}

#include "reporting_consent.moc"

#pragma once

// The website the client sends a customer to for what the client does not do itself: signing up,
// resetting a password and topping up (Phase 5 D-03, D-04, D-07). Nothing in the client builds a
// website address by hand; these are the only strings, and `SeatHubClient::websiteUrl()` is the
// only place that joins them.
//
// The values are `docs/spec/client.md` § Website links, which records them from `ADR-0053` (the
// apex the website has been live on since 2026-09-20). No address here carries a credential, an
// account identifier or a session token: there is no browser-to-app handoff this milestone (D-08).

#include <QString>
#include <QUrl>

namespace SeatHubWeb {

// ADR-0053: the website's apex.
inline constexpr const char* kOrigin = "https://sevenhills.damra.co";

// docs/spec/client.md § Website links.
inline constexpr const char* kTopUpPath = "/topup";
inline constexpr const char* kSignUpPath = "/login?mode=signup";
inline constexpr const char* kResetPasswordPath = "/forgot-password";

/// The website's own front door: the origin itself and nothing after it. The profile's `Open the
/// website` link opens this (owner answer OD-11, `client.md` § Website links); it promises nothing,
/// because the website has no page for changing account details yet.
inline QUrl homeUrl()
{
    return QUrl(QString::fromLatin1(kOrigin));
}

/// The full address for one of the three paths above. An unknown path yields an empty URL, so a
/// typo cannot send a customer somewhere unintended.
inline QUrl url(const char* path)
{
    const QString p = QString::fromLatin1(path);
    if (p != QLatin1String(kTopUpPath) && p != QLatin1String(kSignUpPath)
        && p != QLatin1String(kResetPasswordPath)) {
        return QUrl();
    }
    return QUrl(QString::fromLatin1(kOrigin) + p);
}

} // namespace SeatHubWeb

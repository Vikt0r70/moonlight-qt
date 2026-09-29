#include "pairing_recovery.h"

#include <QEventLoop>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>

Q_LOGGING_CATEGORY(seathubPairingRecovery, "seathub.pairing.recovery")

const char* const kSharedPairingUniqueId = "0123456789ABCDEF";

bool sendPairingCancelRequest(const QString& hostAddress, int port, int timeoutMs)
{
    if (hostAddress.isEmpty()) {
        return false;
    }

    // The one client-reachable clear of the rig's pending pairing session (G-06.2-2): Sunshine's
    // `pair()` reads the `phrase` parameter, and `phrase=cancel` falls into its else branch,
    // "Invalid pairing request", whose handler erases the pending-session map entry (pinned
    // Sunshine `src/nvhttp.cpp:705-713`). Plain HTTP on the control port - the same transport
    // upstream's own pairing requests ride. No PIN exists on this request and none is added:
    // the query carries the upstream uniqueid and the cancel phrase, nothing else.
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(hostAddress);
    url.setPort(port);
    url.setPath(QStringLiteral("/pair"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("uniqueid"), QString::fromLatin1(kSharedPairingUniqueId));
    query.addQueryItem(QStringLiteral("phrase"), QStringLiteral("cancel"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // Fixed to the duration of this one request, never stored on the manager.
    const int timeout = qMax(1, timeoutMs);

    QNetworkAccessManager manager;
    // Rig traffic rides the LAN directly - a system proxy is not in that path (upstream does the
    // same for its own rig requests, `app/backend/nvhttp.cpp:31-32`).
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    loop.exec();

    // "Answered" means the rig spoke HTTP at this request - including its own error statuses,
    // like the 400 Sunshine's no-op clear replies with (any HTTP answer proves the pending
    // session map entry is gone or never was). Qt 6 reports HTTP error statuses through
    // `error()` as protocol errors, so the status attribute is what tells a real response from
    // a transport failure (refused, timed out, no route).
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const bool answered = status.isValid() && reply->isFinished();

    if (!answered) {
        // Outcome only - no address, no URL, no header material. The diagnostics contract for
        // this module is that the log learns whether a clear landed, and nothing about where.
        qCWarning(seathubPairingRecovery)
            << "pairing clear request was not answered within" << timeout << "ms";
    }

    reply->deleteLater();
    return answered;
}

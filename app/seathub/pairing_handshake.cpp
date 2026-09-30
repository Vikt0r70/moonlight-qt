#include "pairing_handshake.h"

#include <QHostAddress>
#include <QLoggingCategory>

#include <memory>

#include "attempt_vocab.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/nvpairingmanager.h"
#include "backend/identitymanager.h"
#include "moonlight_engine_session.h"
#include "pairing_recovery.h"

Q_LOGGING_CATEGORY(seathubPairingHandshake, "seathub.pairing.handshake")

namespace {

// Diagnostics, never customer-facing (D-51). Upstream's own text is in `engineError` too, but
// these say which *stage* of the sequence gave up, which is what support needs. None of them
// mentions the PIN or the address.
const char* const kDiagnosticNoAddress = "the pairing target carries no host address";
const char* const kDiagnosticServerInfo = "the rig did not answer the server-info request";

const char* const kDiagnosticPinRejected = "the rig rejected the pairing PIN";
const char* const kDiagnosticInProgress = "another pairing attempt is already in progress on the rig";
const char* const kDiagnosticFailed = "the pairing handshake failed";

// The two attempt steps this function can fail at (ADR-0072 item 1): everything up to and
// including `getServerInfo()` is `pair_server_info`, the five-phase handshake and everything it
// constructs are `pair_handshake`. Both halves share the single try block below, so the catches
// read the step from a local rather than guessing which half of the sequence threw.
const char* const kStepServerInfo = "pair_server_info";
const char* const kStepHandshake = "pair_handshake";

QString describe(const GfeHttpResponseException& e)
{
    return QStringLiteral("%1 (HTTP %2)").arg(QString::fromLatin1(e.getStatusMessage()))
                                         .arg(e.getStatusCode());
}

QString describe(const QtNetworkReplyException& e)
{
    return e.toQString();
}

} // namespace

PairingHandshakeResult runUpstreamPairingHandshake(const PairingTarget& target)
{
    PairingHandshakeResult result;

    // Which half of the sequence is running (ADR-0072 item 1). `pair_server_info` from the
    // start through `getServerInfo()`; `pair_handshake` from the moment `NvPairingManager` is
    // built, because every failure after that point belongs to the five-phase handshake.
    QString step = QLatin1String(kStepServerInfo);

    if (target.hostAddress.isEmpty()) {
        result.attemptStep = step;
        result.stepClass = QStringLiteral("no_address");
        result.engineError = QString::fromLatin1(kDiagnosticNoAddress);
        return result;
    }

    try {
        // Step 1 and 2: the address the control plane verified, and the pre-pairing server info.
        //
        // 47989 is `ports.control` in the contract (`docs/spec/openapi.yaml:1804-1806`) and
        // `DEFAULT_HTTP_PORT` upstream (`app/backend/nvaddress.h:5`). The HTTPS port starts at the
        // contract's `ports.https` when the authorization carried one and is replaced by whatever
        // `<HttpsPort>` in the server-info response says - which is why an empty certificate with
        // httpsPort 0 stays correct either way (`app/backend/nvhttp.cpp:162-184`).
        const uint16_t httpsPort =
            (target.httpsPort > 0 && target.httpsPort <= 0xFFFF)
                ? static_cast<uint16_t>(target.httpsPort)
                : static_cast<uint16_t>(0);
        NvHTTP http(NvAddress(target.hostAddress, DEFAULT_HTTP_PORT), httpsPort, QSslCertificate());

        // NVLL_NONE, not upstream's usual NVLL_ERROR/NVLL_VERBOSE: both of those log the request
        // URL when a request fails or times out, and the URL carries the rig's address. The
        // threat model for these modules is that the log has no PIN, no token and no host address;
        // the diagnosis that matters travels back in `engineError` instead.
        const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_NONE);

        // Step 3: the same constructor upstream's own address-only host add uses
        // (`app/backend/computermanager.cpp:821`). It reads `<HttpsPort>`, `<appversion>`, the
        // host `uniqueid`, the MAC and the pair state out of `serverInfo`, and pins
        // `activeAddress` to the address we handed it.
        //
        // Heap-allocated and handed back in `result.host`, not a local: the engine streams from
        // *this* record, so the pinned certificate and the host's own reported ports have to
        // survive this function. Plan 03-03's version let it die at the closing brace, which left
        // the engine with no host to build a session from - the gap Plan 03-06 closed.
        std::shared_ptr<MoonlightPairedHost> host =
            std::make_shared<MoonlightPairedHost>(http, serverInfo);
        NvComputer& computer = *host->computer();

        // Past this point every failure belongs to the five-phase handshake (ADR-0072 item 1).
        step = QLatin1String(kStepHandshake);

        // Step 4: upstream's five-phase handshake with the control-plane PIN (ADR-0034). The
        // pinned server certificate it produces is upstream's own MITM protection, which
        // `ComputerManager::saveHost()` writes to `QSettings`. Nothing is written here: STREAM-10
        // keeps this client storing no pairing of its own, so the pin lives on `host` - in memory,
        // for the life of the session - and never on disk.
        NvPairingManager pairingManager(&computer);
        QSslCertificate pinnedCertificate;
        const NvPairingManager::PairState state =
            pairingManager.pair(computer.appVersion, target.pairingPin, pinnedCertificate);

        if (state == NvPairingManager::PAIRED) {
            host->pinCertificate(pinnedCertificate.toPem());

            // The rig's own application list, read here on the pool thread so no frame of the Qt
            // main thread's work waits on it. A failure is recorded rather than raised: pairing
            // succeeded, and the caller fails closed at `launchApp()` instead of streaming an
            // application it cannot name.
            host->fetchAppList();

            // The identity the host side can match to this client: Sunshine stores the client
            // certificate and compares it at TLS time, so its SHA-256 is the pairable handle.
            result.clientIdentity =
                clientCertificateFingerprint(IdentityManager::get()->getCertificate());
            result.host = host;
            result.engineError.clear();
            result.ok = true;
            return result;
        }

        // The step and class of a non-PAIRED state, computed once for every arm below: from the
        // `PairState` enum alone, never from the diagnostic text each arm records for support
        // (ADR-0072 item 2). `PAIRED` returned above, so this is always a failure here.
        result.attemptStep = step;
        result.stepClass = classForPairState(static_cast<int>(state));

        switch (state) {
        case NvPairingManager::PIN_WRONG:
            result.engineError = QString::fromLatin1(kDiagnosticPinRejected);
            break;
        case NvPairingManager::ALREADY_IN_PROGRESS:
            result.engineError = QString::fromLatin1(kDiagnosticInProgress);
            // G-06.2-2: the rig refused the PIN because a pairing session is already in progress
            // - the same half-open pending session the 409 below reports. Tagged so the seam's
            // recovery loop knows the clear-and-retry path applies.
            result.pairingConflict = true;
            break;
        case NvPairingManager::FAILED:
        case NvPairingManager::PAIRED: // handled above; listed so the switch stays exhaustive
            result.engineError = QString::fromLatin1(kDiagnosticFailed);
            break;
        }
    }
    catch (const GfeHttpResponseException& e) {
        // The HTTP status is the whole class (ADR-0072 item 2): `http_4xx` / `http_5xx` from the
        // integer bucket, never from `getStatusMessage()`.
        result.attemptStep = step;
        result.stepClass = classForHttpStatus(e.getStatusCode());
        result.engineError = QStringLiteral("%1: %2")
                                 .arg(QString::fromLatin1(kDiagnosticServerInfo), describe(e));
        // G-06.2-2: HTTP 409 on the getservercert step - "A pairing session with this uniqueid
        // already exists" (pinned Sunshine `src/nvhttp.cpp:975-997`). Every SeatHub install
        // pairs with the same upstream hard-coded uniqueid, so a half-open session from any
        // attempt - ours or another install's - collides with this one. Tagged for the seam's
        // clear-and-retry recovery.
        result.pairingConflict = e.getStatusCode() == 409;
    }
    catch (const QtNetworkReplyException& e) {
        // The Qt error enum integer is the whole class (`net_*`); `toQString()` - which carries
        // the peer name Qt built into the message - stays in `engineError` for support alone.
        result.attemptStep = step;
        result.stepClass = classForNetworkError(static_cast<int>(e.getError()));
        result.engineError = QStringLiteral("%1: %2")
                                 .arg(QString::fromLatin1(kDiagnosticServerInfo), describe(e));
    }
    catch (const std::exception& e) {
        // `NvPairingManager`'s constructor throws `std::runtime_error` when the client
        // certificate or private key cannot be parsed; anything else from OpenSSL lands here too.
        // An exception escaping the pool thread would take the process down, so this catch is
        // load-bearing, not politeness.
        //
        // The class is `crypto_init` for the handshake's own construction failure and the family
        // default for the same shape thrown by the server-info half - `e.what()` names neither
        // and is never read for a class (ADR-0072 item 2).
        result.attemptStep = step;
        result.stepClass = (step == QLatin1String(kStepHandshake))
                               ? QStringLiteral("crypto_init")
                               : QStringLiteral("net_other");
        result.engineError = QStringLiteral("%1: %2")
                                 .arg(QString::fromLatin1(kDiagnosticFailed),
                                      QString::fromLatin1(e.what()));
    }
    catch (...) {
        // An exception with no type at all: the same split, the same rule - a token from where
        // it happened, never a message.
        result.attemptStep = step;
        result.stepClass = (step == QLatin1String(kStepHandshake))
                               ? QStringLiteral("other")
                               : QStringLiteral("net_other");
        result.engineError = QString::fromLatin1(kDiagnosticFailed);
    }

    result.clientIdentity.clear();
    result.ok = false;

    // G-06.2-2: every non-PAIRED outcome leaves the rig holding a pending pairing session keyed
    // by the shared upstream uniqueid - whether this attempt created it, collided with one from
    // an earlier attempt, or hit one from another SeatHub install. Send the one client-reachable
    // clear (Sunshine has no /unpair route in this build; `GET /pair?uniqueid=...&phrase=cancel`
    // drives `pair()`'s else branch, whose handler erases the map entry). Best-effort and
    // bounded: the seam's retry logic runs after this returns, whatever the clear did.
    const bool cleared = sendPairingCancelRequest(target.hostAddress, kPairingControlPort);
    qCInfo(seathubPairingHandshake)
        << "pairing cancel request" << (cleared ? "answered" : "not answered");

    qCWarning(seathubPairingHandshake) << "upstream pairing handshake did not complete";
    return result;
}

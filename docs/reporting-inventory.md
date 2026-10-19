# Reporting inventory (ADR-0107)

Every outbound recipient the RentaPC client talks to, sorted into two classes.

- **covered**: optional reporting. The one setting (the setup-window checkbox, later the Settings row) switches all of it, and nothing in it is sent until a choice is recorded.
- **functional**: the client cannot work without it (sign-in, Play, End, liveness, push, updates). The setting never touches it, and streaming is never touched by the setting.

The switch is `ReportingConsent::coveredUploadsAllowed()` (`app/seathub/reporting_consent.h`). It is checked at the moment of each send, so a switch-off stops the next send at once and anything queued stays unsent on disk. Its answer is "yes" only when the stored choice is "send"; with no recorded choice it answers "no". The stored choice lives at `HKCU\Software\Seven Hills\SeatHub\Reporting` (`Choice`, `NoticeVersion`), written by the setup window and by the Settings row, and read at every start by `ReportingConsent::loadFromStore()` in `app/main.cpp`. No server value, sign-in, update or reinstall writes it.

## Covered

| Recipient | Data sent | Class | Reason | Gate and when a switch takes effect |
|---|---|---|---|---|
| Sentry (the crash and event reports, including the Crashpad handler) | Crash dumps, error events and breadcrumbs, scrubbed by the existing scrubber; never the PIN, password, email or phone number | covered | Optional diagnostics to a third party, so it must be off until the customer says yes | `telemetry.cpp`: Sentry is not initialised without a recorded "send" and the Crashpad handler is not started. Turning it off or on takes effect at the next launch of RentaPC |
| Log shipper (`app/seathub/log_shipper.cpp`) | Scrubbed info-and-above log lines, sent to Sentry's log channel through the `sentry_log` hand-off | covered | Optional diagnostics | `coveredUploadsAllowed()` checked before each hand-off and each drain; lines stay in the on-disk spool while off. At once |
| Quality outbox (`app/seathub/quality_outbox.cpp`, `POST /api/sessions/{id}/quality`) | The stream quality numbers of a play, kept on disk until the control plane confirms them | covered | Optional measurements, not needed to stream | `drainQualityOutbox()` and the outbox put/drain paths check the switch; files stay on disk while off. At once |
| Evidence-capsule delivery (`POST /api/sessions/{id}/evidence`) | The session evidence capsule from the local evidence journal | covered | Optional evidence for support | `deliverPendingCapsules()` checks the switch before each send; capsules stay pending in the journal while off and go out when it is switched on. At once |

The local files themselves (the evidence journal, the log file, the spool and the outbox directories) may still be written while the setting is off; only leaving the PC is covered.

## Functional

| Recipient | Data sent | Class | Reason |
|---|---|---|---|
| Control plane sign-in (`POST /api/auth/login`, `/api/auth/logout`, `GET /api/me`) | Phone or email and password for sign-in; the access token afterwards | functional | Without it nobody can sign in |
| Account reads (`GET /api/wallet`, `/api/wallet/history`, `/api/topup-notices`, `/api/usage`, `/api/account-state`, `/api/me/telemetry`) | The access token; read-only | functional | The balance and profile screens. `/api/me/telemetry` only reads which telemetry identity the server holds; it sends nothing covered |
| Session calls (`POST /api/sessions`, `GET /api/sessions/{id}`, `GET /api/sessions/{id}/pairing`) | The Play request and the pairing poll | functional | Starting a stream |
| End reports (`POST /api/sessions/{id}/end`) | Why the session ended | functional | The server must learn a session is over to stop the clock; the End outbox retries until confirmed |
| Liveness (`POST /api/sessions/{id}/liveness`) | Heartbeat for a running stream | functional | The lease would otherwise lapse |
| Stream time (`POST /api/sessions/{id}/stream-time`) | The measured stream time of a session | functional | It decides the minutes charged |
| Push channel (`GET /api/stream`, the SSE account stream, and `/ws/session/{id}`) | The access token; receives account and session state | functional | Live balance and session state |
| Update feed and Velopack (the release feed on `api.rentapc.online` and the package download) | The installed version and channel | functional | Updates are the way a fix reaches the customer; an update never changes the setting |
| The streaming engine to the rig (Sunshine/Moonlight ports) | The stream itself | functional | It is the product; the setting never touches it |

## What changes only with a new notice version

If a later release adds a covered recipient or a new kind of data, it raises `kCurrentNoticeVersion` in `reporting_consent_store.h` and ships a "what changed" list; the app asks once at its next start, before any covered upload, with the previous choice pre-selected and in force until the customer answers.

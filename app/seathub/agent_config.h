#pragma once

#include <QString>
#include <QUrl>
#include <QVariantMap>

// Reading the host agent's config file, and the one field of it this client may show.
//
// The file belongs to the *host* agents, not to this client. The Node Agent reads
// `%ProgramData%\SeatHub\node-agent.json` and the Host Switcher `%ProgramData%\SeatHub\host-switcher.json`
// (`seathub-host-agents/crates/node-agent/src/main.rs`, `struct Config` / `config_path()`), both of
// them the shape `{ "host_id": ..., "token": ..., "base_url": ..., ... }`. Only `token` is read
// here: `host_id` is the host's own identity and this client has no use for it.
//
// What crosses into QML is the absolute path and the *masked* token. The token itself never
// leaves this translation unit - it is not returned, not stored and not logged. D-30 forbids
// writing a credential to disk in plaintext; not making a second copy of it in the view layer is
// the same rule applied one layer up.
//
// A missing file, a file that is not JSON, and a file whose token key is absent are ordinary
// outcomes, not incidents, so they are reported as values. No sentence is invented for them: this
// header carries a reason for the log only, and the panel renders copy.md's own copy.
namespace AgentConfig {

/// The absolute path of `fileUrl` when it is a local file, or an empty string when it is not.
QString localPath(const QUrl& fileUrl);

/// Reads `path` and takes the agent token out of it.
///
/// Returns false when the file cannot be read, is not a JSON object, or carries no non-empty
/// token. `error` is filled in for the log - it is never rendered to a customer.
bool readToken(const QString& path, QString* token, QString* error);

/// The masked form: the first four and the last four characters, with a fixed run of bullets
/// between them. Short tokens are masked whole rather than shown. The run is a fixed length so
/// the mask does not disclose how long the token is.
QString maskToken(const QString& token);

/// One call for the UI: `{ ok, path, token_masked, error }`.
///
/// Deliberately has no `token` key - the value the customer must never see on screen is the value
/// QML must never receive either.
QVariantMap describe(const QUrl& fileUrl);

} // namespace AgentConfig

#include "agent_config.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

// The keys the host agents write. `token` is the schema's own name - `struct Config` in
// `seathub-host-agents/crates/node-agent/src/main.rs` (and the same shape in the Host Switcher's
// `host-switcher.json`). The other two spellings are accepted because this file is written by an
// installer and edited by hand, and a technician who writes `AGENT_TOKEN` means the same thing.
//
// They are tried in this order, so a file that carries both a canonical key and a hand-written
// one is read the way the agent itself would read it.
const char* const kTokenKeys[] = {"token", "agent_token", "AGENT_TOKEN"};

constexpr int kMaskKeep = 4;
constexpr int kMaskRun = 8;

} // namespace

QString AgentConfig::localPath(const QUrl& fileUrl)
{
    // A picked file always arrives as a URL. `toLocalFile()` is what turns
    // `file:///C:/ProgramData/SeatHub/node-agent.json` into the path the agent itself would
    // print; anything that is not a local file has no path to show.
    if (!fileUrl.isLocalFile()) {
        return QString();
    }
    return QFileInfo(fileUrl.toLocalFile()).absoluteFilePath();
}

bool AgentConfig::readToken(const QString& path, QString* token, QString* error)
{
    if (token) {
        token->clear();
    }

    if (path.isEmpty()) {
        if (error) {
            *error = QStringLiteral("no file was chosen");
        }
        return false;
    }

    QFile file(path);
    if (!file.exists()) {
        if (error) {
            *error = QStringLiteral("no file at %1").arg(path);
        }
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot read %1").arg(path);
        }
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("%1 is not a JSON object").arg(path);
        }
        return false;
    }

    const QJsonObject object = document.object();
    for (const char* key : kTokenKeys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isString() && !value.toString().isEmpty()) {
            if (token) {
                *token = value.toString();
            }
            return true;
        }
    }

    if (error) {
        *error = QStringLiteral("%1 carries no agent token").arg(path);
    }
    return false;
}

QString AgentConfig::maskToken(const QString& token)
{
    if (token.isEmpty()) {
        return QString();
    }

    // Too short to show any of it and still be a mask.
    if (token.length() <= kMaskKeep * 2) {
        return QString(token.length(), QChar(0x2022));
    }

    return token.left(kMaskKeep) + QString(kMaskRun, QChar(0x2022))
           + token.right(kMaskKeep);
}

QVariantMap AgentConfig::describe(const QUrl& fileUrl)
{
    const QString path = localPath(fileUrl);

    QVariantMap described;
    described.insert(QStringLiteral("path"), path);

    QString token;
    QString error;
    const bool ok = readToken(path, &token, &error);

    described.insert(QStringLiteral("ok"), ok);
    described.insert(QStringLiteral("token_masked"), ok ? maskToken(token) : QString());
    described.insert(QStringLiteral("error"), ok ? QString() : error);
    return described;
}

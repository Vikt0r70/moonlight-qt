#include "velopack_updater.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef SEATHUB_VELOPACK
// Velopack's C API (MIT), the pinned velopack_libc 1.2.161. The C++ wrapper header is deliberately
// not used: the C API is the documented surface (docs.velopack.io reference/cpp/c-api).
#include "Velopack.h"
#endif

Q_LOGGING_CATEGORY(seathubVelopack, "seathub.velopack")

namespace VelopackUpdater {

namespace {

// The identity `main()` gives QCoreApplication; the D-45 paths are derived from the same two names.
const char* const kOrganization = "Seven Hills";
const char* const kApplication = "SeatHub";

QString appFolder(const QString& root)
{
    if (root.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(root + QLatin1Char('/') + QLatin1String(kOrganization)
                           + QLatin1Char('/') + QLatin1String(kApplication));
}

bool removeFolder(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink() && !info.isJunction())
        return true;
    // A link is removed as a link: never followed into whatever it points at.
    if (info.isSymLink() || info.isJunction())
        return QFile::remove(path) || QDir().rmdir(path);
    return QDir(path).removeRecursively();
}

} // namespace

D45Roots d45RootsFromEnvironment()
{
    return {qEnvironmentVariable("APPDATA"), qEnvironmentVariable("LOCALAPPDATA")};
}

D45State d45State(const D45Roots& roots)
{
    D45State state;
    const QString roaming = appFolder(roots.roaming);
    const QString local = appFolder(roots.local);
    if (!roaming.isEmpty())
        state.directories.append(roaming);
    if (!local.isEmpty()) {
        for (const char* folder : {"crash-db", "log-spool", "update"})
            state.directories.append(local + QLatin1Char('/') + QLatin1String(folder));
    }
    state.registryKey = QStringLiteral("Software\\%1\\%2")
                            .arg(QLatin1String(kOrganization), QLatin1String(kApplication));
    return state;
}

bool removeCurrentUserKey(const QString& subKey)
{
#ifdef Q_OS_WIN
    const std::wstring key = subKey.toStdWString();
    const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    // An absent key is already gone.
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(subKey)
    return true;
#endif
}

bool removeD45State(const D45Roots& roots, const RegistryRemover& removeKey)
{
    const D45State state = d45State(roots);
    bool allGone = true;
    for (const QString& directory : state.directories) {
        if (!removeFolder(directory)) {
            qCWarning(seathubVelopack) << "D-45: could not remove" << directory;
            allGone = false;
        }
    }
    if (removeKey && !removeKey(state.registryKey)) {
        qCWarning(seathubVelopack) << "D-45: could not remove the HKCU key" << state.registryKey;
        allGone = false;
    }
    return allGone;
}

QUrl feedFolderOf(const QUrl& setupUrl)
{
    QUrl folder = setupUrl;
    folder.setQuery(QString());
    folder.setFragment(QString());
    return folder.adjusted(QUrl::RemoveFilename);
}

bool isTrustedFeedFolder(const QUrl& folder, const QString& controlPlaneHost)
{
    return folder.isValid() && folder.scheme() == QLatin1String("https")
        && folder.userInfo().isEmpty() && !controlPlaneHost.isEmpty()
        && folder.host().compare(controlPlaneHost, Qt::CaseInsensitive) == 0;
}

// ---------------------------------------------------------------------------------- startup and apply

#ifdef SEATHUB_VELOPACK

namespace {

// Runs inside Velopack's `--veloapp-uninstall` invocation of this exe: no window, 30 s, then the
// process exits and `Update.exe` removes the install folder and the one HKCU uninstall row.
void beforeUninstall(void*, const char*)
{
    removeD45State(d45RootsFromEnvironment(), &removeCurrentUserKey);
}

QString lastError()
{
    char buffer[1024] = {};
    const size_t needed = vpkc_get_last_error(buffer, sizeof(buffer));
    if (needed == 0)
        return QStringLiteral("Velopack reported no error text");
    return QString::fromUtf8(buffer);
}

void onProgress(void* userData, size_t percent)
{
    const auto* callback = static_cast<const std::function<void(int)>*>(userData);
    if (callback && *callback)
        (*callback)(int(percent > 100 ? 100 : percent));
}

class VelopackApplier : public UpdateApplier
{
public:
    explicit VelopackApplier(const QString& controlPlaneHost) : m_host(controlPlaneHost) {}
    ~VelopackApplier() override { release(); }

    bool download(const QString& feedFolderUrl, const std::function<void(int)>& progress,
                  QString* resolvedVersion, QString* error) override
    {
        release();
        const QUrl folder(feedFolderUrl, QUrl::StrictMode);
        if (!isTrustedFeedFolder(folder, m_host)) {
            *error = QStringLiteral("The update folder is not on the SevenHills host.");
            return false;
        }
        const std::string url = folder.toString(QUrl::StripTrailingSlash).toStdString();

        vpkc_update_source_t* source = vpkc_new_source_http_url(url.c_str());
        if (source == nullptr) {
            *error = lastError();
            return false;
        }
        const bool created = vpkc_new_update_manager_with_source(source, nullptr, nullptr, &m_manager);
        // The manager keeps its own reference to the source.
        vpkc_free_source(source);
        if (!created || m_manager == nullptr) {
            m_manager = nullptr;
            *error = lastError();
            return false;
        }

        const vpkc_update_check_t check = vpkc_check_for_updates(m_manager, &m_info);
        if (check != UPDATE_AVAILABLE || m_info == nullptr || m_info->TargetFullRelease == nullptr) {
            *error = check == UPDATE_ERROR ? lastError()
                                           : QStringLiteral("The update folder has no newer release.");
            release();
            return false;
        }
        *resolvedVersion = QString::fromUtf8(m_info->TargetFullRelease->Version);

        std::function<void(int)> callback = progress;
        if (!vpkc_download_updates(m_manager, m_info, &onProgress, &callback)) {
            *error = lastError();
            release();
            return false;
        }
        return true;
    }

    bool applyAfterExit(QString* error) override
    {
        if (m_manager == nullptr || m_info == nullptr || m_info->TargetFullRelease == nullptr) {
            *error = QStringLiteral("No downloaded release is waiting to be applied.");
            return false;
        }
        // Not silent: Velopack's own small window with its progress bar is the update experience
        // (ADR-0086). Restart: the updater relaunches SeatHub - the new version, or the old one when
        // the swap could not be made. The updater waits at most 60 s for this process to exit.
        if (!vpkc_wait_exit_then_apply_updates(m_manager, m_info->TargetFullRelease, false, true,
                                               nullptr, 0)) {
            *error = lastError();
            return false;
        }
        return true;
    }

private:
    void release()
    {
        if (m_info != nullptr) {
            vpkc_free_update_info(m_info);
            m_info = nullptr;
        }
        if (m_manager != nullptr) {
            vpkc_free_update_manager(m_manager);
            m_manager = nullptr;
        }
    }

    QString m_host;
    vpkc_update_manager_t* m_manager = nullptr;
    vpkc_update_info_t* m_info = nullptr;
};

} // namespace

void runStartupHook()
{
    vpkc_app_set_auto_apply_on_startup(false);
    vpkc_app_set_hook_before_uninstall(&beforeUninstall);
    vpkc_app_run(nullptr);
}

std::shared_ptr<UpdateApplier> createApplier(const QString& controlPlaneHost)
{
    return std::make_shared<VelopackApplier>(controlPlaneHost);
}

#else // no Velopack library in this build (the unit suites)

namespace {

class UnavailableApplier : public UpdateApplier
{
public:
    bool download(const QString&, const std::function<void(int)>&, QString*, QString* error) override
    {
        *error = QStringLiteral("This build has no Velopack updater.");
        return false;
    }
    bool applyAfterExit(QString* error) override
    {
        *error = QStringLiteral("This build has no Velopack updater.");
        return false;
    }
};

} // namespace

void runStartupHook() {}

std::shared_ptr<UpdateApplier> createApplier(const QString&)
{
    return std::make_shared<UnavailableApplier>();
}

#endif

} // namespace VelopackUpdater

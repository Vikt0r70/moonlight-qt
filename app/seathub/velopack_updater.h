#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

// SeatHub's Velopack seam (ADR-0086, I-SEATHUB; update source `url-folder`, Velopack and vpk 1.2.161
// as docs/evidence/m001/velopack-proof.md recorded them).
//
// Velopack owns exactly two things here: downloading a release into the install's packages folder
// and handing it to `Update.exe`, which swaps the app folder after this process exits and relaunches
// SeatHub (a failed swap relaunches the old version). Everything else about an update stays in
// `UpdateFeedClient` (ADR-0077): the floor, the mandatory window, newer-is-current and the bounded
// retry (V34). Velopack has no minimum-version concept and is never asked about one.
//
// The Velopack C library (`Velopack.h`) is only compiled in when SEATHUB_VELOPACK is defined (the
// app build). The unit suites build without it and replace `UpdateApplier` with a fake, so no suite
// needs the library, a network or an installed copy of SeatHub.
namespace VelopackUpdater {

// ---- D-45: the state a real uninstall removes ---------------------------------------------------

// The two roots the state lives under: `%APPDATA%` and `%LOCALAPPDATA%`. The uninstall hook runs
// before any Qt application object exists, so it cannot ask QStandardPaths; the roots come from the
// environment and the organisation/application names are the ones `main()` sets.
struct D45Roots
{
    QString roaming;
    QString local;
};

// Exactly the D-45 state of I-SEATHUB and nothing else:
//   * `directories`  - `<roaming>\Seven Hills\SeatHub` (sign-in blobs, `telemetry.json`) and, under
//                      `<local>\Seven Hills\SeatHub`, the `crash-db`, `log-spool` and `update`
//                      folders. Never `<local>\Seven Hills\SeatHub` itself, never `Seven Hills`.
//   * `registryKey`  - `Software\Seven Hills\SeatHub` under HKEY_CURRENT_USER (the Settings and the
//                      pairing certificate and key).
struct D45State
{
    QStringList directories;
    QString registryKey;
};

D45Roots d45RootsFromEnvironment();
D45State d45State(const D45Roots& roots);

using RegistryRemover = std::function<bool(const QString& subKeyUnderCurrentUser)>;
// The default remover: `RegDeleteTreeW(HKEY_CURRENT_USER, key)` on Windows. A missing key counts as
// removed.
bool removeCurrentUserKey(const QString& subKey);

// Removes the D-45 state. Returns true when nothing of it is left. It never shows a window and never
// throws (Velopack gives the hook 30 s and no UI); a failure to remove one item does not stop the others.
bool removeD45State(const D45Roots& roots, const RegistryRemover& removeKey);

// ---- startup ---------------------------------------------------------------------------------

// The first statement of `main()` (the C-API equivalent of `VelopackApp::Build().Run()`): handles the
// install, update and uninstall hook invocations Velopack makes by relaunching this exe, and may not
// return. It also registers the before-uninstall hook above. Auto-apply on startup is switched off:
// whether and when an update is applied is decided by `UpdateFeedClient`, never by a leftover
// download. A no-op in a build without the Velopack library.
void runStartupHook();

// ---- download and apply (the seam) -----------------------------------------------------------------

class UpdateApplier
{
public:
    virtual ~UpdateApplier() = default;

    // Blocking; runs on a worker thread. Resolves the newest release in `feedFolderUrl`, downloads it
    // into the packages folder and reports progress 0..100 (from that same thread). `resolvedVersion`
    // is the version the folder actually carries: the caller refuses a folder that does not carry the
    // release it chose. False with `error` set on any failure; nothing about the install changes.
    virtual bool download(const QString& feedFolderUrl, const std::function<void(int)>& progress,
                          QString* resolvedVersion, QString* error) = 0;

    // Hands the downloaded release to `Update.exe`, which waits for this process to exit, replaces the
    // app folder and relaunches SeatHub. True when the hand-off started: the caller must then exit
    // promptly. False leaves the running version exactly as it was.
    virtual bool applyAfterExit(QString* error) = 0;
};

// The real applier (the app build) or one that refuses every call (a build without the library).
// The real one only ever downloads from a folder that is https on `controlPlaneHost`.
std::shared_ptr<UpdateApplier> createApplier(const QString& controlPlaneHost);

// The release folder is the folder that holds the release row's `Setup.exe` (the feed serves
// Velopack's `releases.win.json` and the full package beside it).
QUrl feedFolderOf(const QUrl& setupUrl);
// The folder is only used when it is https on the control plane's own host - the same check the
// manual download address passes (client.md § Website links). A hostile feed row never points the
// updater anywhere else, and never at a local path.
bool isTrustedFeedFolder(const QUrl& folder, const QString& controlPlaneHost);

} // namespace VelopackUpdater

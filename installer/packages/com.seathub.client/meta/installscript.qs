// SeatHub component script.
//
// D-45: uninstalling SeatHub removes everything, not just the program. The program is per-machine
// under Program Files (D-42), but the customer's own state is not:
//
//   TokenStore::defaultDirectory() is QStandardPaths::writableLocation(AppDataLocation), and
//   app/main.cpp sets organization "Seven Hills" and application "SeatHub". On Windows that is
//   %APPDATA%\Seven Hills\SeatHub, which is where the DPAPI blobs for the customer's credentials
//   live. Uninstalling the program alone would leave them on disk, and D-45 says a reinstall starts
//   from nothing.
//
// Everything here is either read out of the shipped IFW 4.7 manual or measured against it. Three of
// these were measured because the manual does not say, and two of them killed an earlier version of
// this file:
//
//   1. There is no @LocalAppData@ predefined variable. The documented set is @RootDir@, @HomeDir@,
//      @TargetDir@, @StartMenuDir@, @AllUsersStartMenuProgramsPath@ and so on
//      (ifw-globalconfig.html, "Predefined Variables"). Paths are therefore built from the
//      environment, which scripting-installer.html documents as returning the variable's content or
//      an empty string when unset.
//   2. "Delete" removes a *file* and "Rmdir" removes a directory (operations.html), and there is no
//      recursive delete among the operations.
//   3. MEASURED: **Rmdir is not recursive.** `devtool.exe operation <installer> DO,Rmdir,<populated
//      dir>` fails with "Cannot remove directory" and leaves every file in place.
//   4. MEASURED: **component scripts are never instantiated during uninstallation.** An instrumented
//      component script logs `isUninstaller=false isInstaller=true` when it is constructed during
//      install, and logs *nothing at all* during an uninstall - the uninstaller replays the
//      operations recorded at install time and does not load component scripts to do it. So a
//      `installer.isUninstaller()` branch, a `uninstallationFinished` handler, and any other
//      uninstall-time script hook are all dead code: the first version of this file deleted nothing,
//      and the drill proved it.
//
// What works is therefore registration at install time - which is when this script does run - so
// that the uninstaller replays it: component.registerPathForUninstallation(path, wipe). The
// `wipe` argument is how a directory that the installer never populated gets removed rather than
// only the files it wrote there.
//
// Limitation, deliberate and stated rather than worked around: a per-machine install is often run by
// an administrator, and %APPDATA%/%LOCALAPPDATA% are per-user, so this registers the state of the
// account that ran the installer. State left behind by other Windows accounts on the same machine is
// out of reach. That is inherent in per-user state under a per-machine install.

function Component()
{
    // Nothing is connected here on purpose - see point 4 above.
}

Component.prototype.createOperations = function()
{
    component.createOperations();

    // Start Menu shortcut so the installed client is launchable from Search / All apps, not only by
    // navigating to Program Files. Per-machine install (D-42), so it lands in the All Users Programs
    // folder (@AllUsersStartMenuProgramsPath@ is a documented predefined variable). IFW undoes the
    // operations added here on uninstall, so the shortcut needs no separate cleanup registration.
    component.addOperation("CreateShortcut",
                           "@TargetDir@/SeatHub.exe",
                           "@AllUsersStartMenuProgramsPath@/SeatHub.lnk",
                           "workingDirectory=@TargetDir@",
                           "iconPath=@TargetDir@/SeatHub.exe",
                           "description=SeatHub");

    var paths = [
        // %APPDATA%\Seven Hills\SeatHub - where TokenStore actually puts the encrypted credentials.
        statePath("APPDATA", "/Seven Hills/SeatHub")
        // F-6: %LOCALAPPDATA%\SeatHub is deliberately NOT registered. SeatHub never writes there
        // (`TokenStore::defaultDirectory()` resolves to the Roaming path above), but the retired
        // Tauri client was installed there: registering it would delete that client's
        // seathub-client.exe and uninstall.exe without removing any of its state or ours, i.e. it
        // would break a program this installer was never asked to touch. The directory D-45 names
        // is therefore recorded as a deliberate non-target here, and the D-30/D-45 path question
        // belongs in the ADR that reconciles the documents with the resolution (see token_store.h).
    ];

    for (var i = 0; i < paths.length; ++i) {
        if (paths[i] !== "") {
            console.log("SeatHub: registering for removal on uninstall: " + paths[i]);
            component.registerPathForUninstallation(paths[i], true);
        }
    }
};

function statePath(variable, suffix)
{
    var base = installer.environmentVariable(variable);
    if (base === "") {
        // An unset variable is not worth a hard failure: the uninstall simply removes the program.
        console.log("SeatHub: " + variable + " is not set; skipping " + suffix);
        return "";
    }
    return installer.toNativeSeparators(base + suffix);
}

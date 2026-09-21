// SeatHub installer control script (config.xml <ControlScript>).
//
// Why this exists: the setup refused to install over an existing SeatHub. IFW's target directory
// page rejects any folder that already holds the maintenance tool ("TargetDirectoryInUse"), so an
// update - by hand or from the in-app updater - needed a manual uninstall first. This script
// removes the installed SeatHub before that check runs, and keeps the customer signed in while it
// does (owner decision, 2026-09-21). A real uninstall still wipes everything (D-45): nothing here
// changes what installscript.qs registers.
//
// This is a CONTROL script, not a component script. Page callbacks only fire in the control
// script (Controller Scripting, noninteractive.html); installscript.qs is a component script and
// Controller.prototype callbacks there never run.
//
// Everything below is read out of the shipped IFW 4.7 manual
// (C:\Qt\Tools\QtInstallerFramework\4.7\doc\html) or, where the manual is silent, out of the
// IFW 4.7.0 source (qtproject/installer-framework, tag 4.7.0). What the manual does not say:
//
//   1. WHEN the refusal happens. TargetDirectoryPage::validatePage() calls
//      PackageManagerCore::installationAllowedToDirectory(), i.e. on Next. The page callback runs
//      from PackageManagerGui::currentPageChanged(), i.e. on ENTERING the page. So
//      TargetDirectoryPageCallback is early enough; no Next override is needed.
//   2. WHAT the refusal keys on: only <TargetDir>/<MaintenanceToolName>.exe. A missing or empty
//      folder is accepted outright. The <Version> values in config.xml and package.xml play no part.
//   3. The installed maintenance tool cannot elevate itself in command-line mode:
//      PackageManagerCore::gainAdminRights() throws "Cannot elevate access rights while running
//      from command line" and runUninstaller() calls it before undoing anything when the folder is
//      not writable. The setup elevates lazily (RequiresAdminRights is on the component, and
//      runInstaller() asks only after the wizard; the owner's 0.1.3 InstallationLog.txt shows
//      "Starting elevated process" after the pages). So a double-clicked setup is not elevated
//      here, and the in-app update (ShellExecuteW "runas") is. After installer.gainAdminRights()
//      the child processes installer.execute() starts run in IFW's elevated server
//      (QProcessWrapper::start -> connectToServer).
//   4. `purge` deletes the folder AFTER the tool exits: deleteMaintenanceTool() starts a detached
//      %TEMP%\uninstall.vbs that deletes the tool once a second until it can, then the whole folder.
//      Letting the install start before that script finishes would let it delete the new files.

var SEATHUB_TITLE = "SeatHub Setup";                        // config.xml <Title>
var SEATHUB_MAINTENANCE_TOOL = "SeatHubMaintenanceTool.exe"; // config.xml <MaintenanceToolName> + .exe
var SEATHUB_CLIENT = "SeatHub.exe";                          // config.xml <RunProgram>
var SEATHUB_REMOVED_KEY = "SeatHubPreviousInstallRemoved";
var SEATHUB_FOLDER_WAIT_SECONDS = 30;

// ifw-cli.html: `purge` - "Uninstall all packages and remove the program directory" (`remove`
// would leave the maintenance tool, and the tool is what the refusal keys on).
// `--confirm-command` confirms without user input. `--default-answer` answers every message query
// with its default: an undo step that fails asks "installationErrorWithIgnore", default Ignore.
// Long forms on purpose: `-c` is also --create-local-repository in the same option table.
var SEATHUB_PURGE_ARGS = ["purge", "--confirm-command", "--default-answer"];

function Controller()
{
}

// noninteractive.html, "Target Directory Page": TargetDirectoryPageCallback() and its
// TargetDirectoryLineEdit widget.
Controller.prototype.TargetDirectoryPageCallback = function()
{
    // scripting-installer.html: isInstaller(). The maintenance tool embeds this script too.
    if (!installer.isInstaller() || installer.value(SEATHUB_REMOVED_KEY) === "true")
        return;

    var targetDir = seathubChosenTargetDir();
    if (targetDir === "" || !installer.fileExists(seathubJoin(targetDir, SEATHUB_MAINTENANCE_TOOL)))
        return;

    // scripting-qmessagebox.html: question(identifier, title, text, buttons, button).
    var answer = QMessageBox.question("SeatHubRemovePreviousInstall", SEATHUB_TITLE,
        "SeatHub is already installed in this folder. Setup will remove it first, then install "
        + "this version. You stay signed in.",
        QMessageBox.Ok | QMessageBox.Cancel, QMessageBox.Ok);
    if (answer != QMessageBox.Ok) {
        seathubLog("kept the installed SeatHub; the folder check will refuse it on Next");
        return;
    }

    var removed = false;
    try {
        removed = seathubRemovePreviousInstall(targetDir);
    } catch (e) {
        seathubLog("removal stopped by a script error: " + e);
    }

    if (removed) {
        installer.setValue(SEATHUB_REMOVED_KEY, "true");
        seathubLog("previous install removed from " + targetDir);
        return;
    }

    QMessageBox.critical("SeatHubRemovePreviousInstallFailed", SEATHUB_TITLE,
        "The installed SeatHub couldn't be removed, so this version can't be installed here yet. "
        + "Remove SeatHub in Windows Settings > Apps, then run this setup again.");
};

function seathubRemovePreviousInstall(targetDir)
{
    var tool = seathubJoin(targetDir, SEATHUB_MAINTENANCE_TOOL);

    if (!seathubStopClient(seathubJoin(targetDir, SEATHUB_CLIENT)))
        return false;

    // The purge replays installscript.qs's registerPathForUninstallation(tokenDir, wipe=true),
    // which would sign the customer out. Copy the token store aside first and put it back after,
    // whatever the purge did. No copy, no purge: keeping sign-in is the owner's decision.
    var tokenDir = seathubTokenDir();
    var backup = seathubBackUpTokens(tokenDir);
    if (backup === null)
        return false;

    var removed = false;
    try {
        removed = seathubPurge(tool) && seathubWaitForFolderToGo(targetDir, tool);
    } finally {
        if (backup !== "")
            seathubRestoreTokens(backup, tokenDir);
    }
    return removed;
}

// The in-app updater quits SeatHub on its own, but a hand-run setup may find it open, and open
// files make the purge leave the folder behind.
function seathubStopClient(client)
{
    // scripting-installer.html: isProcessRunning(name) is case-insensitive on Windows;
    // killProcess(absoluteFilePath) - "true if a process with absoluteFilePath could be killed or
    // is not running", "semi blocking (to keep the main thread to paint the UI)".
    // MEASURED: that semi-blocking wait is a nested event loop, so the wizard stays clickable, and
    // for a process that does not close on request it lasts 30 s (a Next press meanwhile gets the
    // stock "already contains an installation" error). 4.7.0 source: it posts WM_CLOSE, waits up to
    // 30 s, then terminates. So only call it when SeatHub is really running, after a short grace:
    // the in-app updater has already told SeatHub to quit and it may still be finishing. Check
    // afterwards, because killProcess compares paths exactly.
    for (var i = 0; i < 5 && installer.isProcessRunning(client); ++i)
        seathubSleepOneSecond();
    if (!installer.isProcessRunning(client))
        return true;
    installer.killProcess(client);
    if (!installer.isProcessRunning(client))
        return true;

    // Still running: most likely an elevated SeatHub (RunProgram starts it from the elevated
    // setup) that a non-elevated setup cannot stop. Stop it through the elevated server instead.
    seathubLog(client + " is still running; stopping it with elevated rights");
    if (!installer.hasAdminRights() && !seathubGainAdminRights())
        return false;
    installer.execute(seathubSystemTool("taskkill.exe"), ["/F", "/IM", SEATHUB_CLIENT], "");
    if (installer.isProcessRunning(client)) {
        seathubLog(client + " could not be stopped");
        return false;
    }
    return true;
}

function seathubPurge(tool)
{
    if (seathubRunPurge(tool) === 0)
        return true;
    if (installer.hasAdminRights())
        return false;

    // See point 3 at the top: under Program Files a non-elevated purge fails before it removes
    // anything, so it is safe to elevate and try once more. This is also why a folder the user can
    // write to needs no UAC prompt at all.
    seathubLog("purge needs elevated rights; asking for them");
    if (!seathubGainAdminRights())
        return false;
    return seathubRunPurge(tool) === 0;
}

function seathubRunPurge(tool)
{
    // scripting-installer.html: execute() "Returns an empty array if the program could not be
    // executed, otherwise the output of command as the first item, and the return code as the
    // second." An empty stdIn closes the tool's input, so it can never sit waiting on it.
    seathubLog("running " + tool + " " + SEATHUB_PURGE_ARGS.join(" "));
    var result = installer.execute(tool, SEATHUB_PURGE_ARGS, "");
    if (result.length < 2) {
        seathubLog("the installed maintenance tool could not be started");
        return -1;
    }
    var output = String(result[0]);
    seathubLog("purge exit code " + result[1] + "; output tail: "
        + output.substring(Math.max(0, output.length - 1500)));
    return Number(result[1]);
}

// Point 4 at the top: wait for uninstall.vbs to delete the folder before the wizard moves on.
function seathubWaitForFolderToGo(targetDir, tool)
{
    for (var i = 0; i < SEATHUB_FOLDER_WAIT_SECONDS; ++i) {
        if (!installer.fileExists(targetDir))
            return true;
        seathubSleepOneSecond();
    }
    if (installer.fileExists(tool)) {
        seathubLog("the maintenance tool is still in " + targetDir);
        return false;
    }
    // The tool is gone but files are left (something held one open). The folder check no longer
    // refuses; it asks before reusing a non-empty folder ("OverwriteTargetDirectory").
    seathubLog(targetDir + " was not fully removed; continuing");
    return true;
}

// Token store backup. TokenStore::defaultDirectory() is %APPDATA%\Seven Hills\SeatHub, built here
// exactly the way installscript.qs builds the path it registers, so the two always agree. Local
// test runs point APPDATA at a scratch folder, which moves both.
function seathubTokenDir()
{
    // scripting-installer.html: environmentVariable() - "An empty string is returned if the
    // environment variable is not set."
    var base = installer.environmentVariable("APPDATA");
    if (base === "")
        return "";
    return installer.toNativeSeparators(base + "/Seven Hills/SeatHub");
}

// Returns the backup folder, "" when there is nothing to keep, or null when the copy failed.
//
// operations.html: "CopyDirectory" sourcePath targetPath [forceOverwrite]; "Mkdir" path.
// scripting-installer.html: performOperation(name, arguments) - "Instantly performs the operation",
// so nothing is recorded for uninstall. MEASURED from the 4.7.0 source (copydirectoryoperation.cpp),
// not stated in the manual: both folders must already exist, and files land under the PARENT of
// targetPath plus the source folder's own name. Copying .../SeatHub onto an existing <backup>/SeatHub
// therefore fills <backup>/SeatHub, and the same holds in reverse.
function seathubBackUpTokens(tokenDir)
{
    if (tokenDir === "" || !installer.fileExists(tokenDir))
        return "";
    var temp = installer.environmentVariable("TEMP");
    if (temp === "") {
        seathubLog("TEMP is not set; cannot keep the sign-in");
        return null;
    }
    var backup = seathubJoin(temp, "SeatHub-sign-in-" + Date.now());
    var copy = seathubJoin(backup, seathubLeaf(tokenDir));
    if (!installer.performOperation("Mkdir", [copy])
            || !installer.performOperation("CopyDirectory", [tokenDir, copy])) {
        seathubLog("could not copy " + tokenDir + " to " + copy);
        return null;
    }
    seathubLog("sign-in kept in " + copy);
    return backup;
}

function seathubRestoreTokens(backup, tokenDir)
{
    var copy = seathubJoin(backup, seathubLeaf(tokenDir));
    if (!installer.performOperation("Mkdir", [tokenDir])
            || !installer.performOperation("CopyDirectory", [copy, tokenDir, "forceOverwrite"])) {
        // Not fatal to the install: the customer signs in again. The copy stays for recovery.
        seathubLog("could not restore the sign-in; the copy stays in " + copy);
        return false;
    }
    seathubLog("sign-in restored to " + tokenDir);
    // The copy holds the customer's (DPAPI-encrypted) credentials, so do not leave it in TEMP.
    // Only ever a folder this script named.
    if (backup.indexOf("SeatHub-sign-in-") !== -1)
        installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", "/s", "/q", backup], "");
    return true;
}

function seathubGainAdminRights()
{
    // scripting-installer.html: gainAdminRights() - "Tries to gain admin rights. On success, it
    // returns true." Shows the UAC prompt; the install that follows needs the same rights, so they
    // are kept rather than dropped.
    try {
        return installer.gainAdminRights() ? true : false;
    } catch (e) {
        seathubLog("could not gain admin rights: " + e);
        return false;
    }
}

function seathubChosenTargetDir()
{
    // gui.currentPageWidget() (scripting-gui.html) is this page; the line edit holds what the user
    // sees. installer.value("TargetDir") only catches up when the page is left.
    var page = gui.currentPageWidget();
    var dir = (page && page.TargetDirectoryLineEdit) ? String(page.TargetDirectoryLineEdit.text)
                                                     : installer.value("TargetDir");
    return installer.toNativeSeparators(dir).replace(/[\\\/]+$/, "");
}

// A one-second pause. The script engine has no sleep; ping waits a second between its two echoes.
function seathubSleepOneSecond()
{
    installer.execute(seathubSystemTool("PING.EXE"), ["-n", "2", "127.0.0.1"], "");
}

function seathubSystemTool(name)
{
    return seathubJoin(installer.environmentVariable("SystemRoot"), "System32/" + name);
}

function seathubJoin(dir, name)
{
    return installer.toNativeSeparators(dir + "/" + name);
}

function seathubLeaf(path)
{
    var parts = path.split(/[\\\/]/);
    return parts[parts.length - 1];
}

function seathubLog(message)
{
    console.log("SeatHub: " + message);
}

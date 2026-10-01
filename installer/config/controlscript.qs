// IFW 4.7 controller. ADR-0070: extract beside the live app, then swap; never run an uninstaller.
// Local 4.7 scripting-installer.html/noninteractive.html are the API authority.
var SEATHUB_TITLE = "SeatHub Setup";
var SEATHUB_MAINTENANCE_TOOL = "SeatHubMaintenanceTool.exe";
var SEATHUB_CLIENT = "SeatHub.exe";
var SEATHUB_CRASH_HANDLER = "crashpad_handler.exe";
// docs/spec V36 and ADR-0070 item 1: two waits only, on failure; no success-path delay.
var SEATHUB_RENAME_TRIES = 3;
var SEATHUB_RENAME_WAIT_S = 1;
var SEATHUB_FAILURE = "SeatHub could not finish updating. Your current version is unchanged and still works. SeatHub will try again by itself.";
var SEATHUB_STOP_FAILURE = "SeatHub is still running and could not be closed. Close it and run this setup again.";
var seathubAttempt = null;
var seathubLogLines = [];

function Controller()
{
    if (!installer.isInstaller()) return;
    installer.installationStarted.connect(this, this.seathubStarted);
    installer.installationFinished.connect(this, this.seathubFinished);
    installer.installationInterrupted.connect(this, this.seathubInterrupted);
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    if (!installer.isInstaller() || seathubAttempt) return;
    var x = seathubChosenTargetDir();
    if (!x) return;
    seathubRecover(x);
    // Nonempty directories without a tool are staged too: never accept an overwrite prompt.
    if (!installer.fileExists(x)) return;
    var entries = seathubPs("@(Get-ChildItem -LiteralPath " + seathubPsQuote(x) + " -Force).Count");
    if (entries.length >= 2 && Number(entries[1]) === 0 && Number(entries[0]) === 0) return;
    var id = seathubRandomId();
    seathubAttempt = {id:id, final:x, stage:x + ".stage-" + id, old:x + ".old-" + id,
        started:new Date().toISOString(), clock:Date.now(), stageMs:0, swapMs:0,
        outcome:"failed", step:"preflight", cls:"installer.unknown", code:0,
        rollback:"not_needed", state:"old_intact", interrupted:false, ended:false};
    installer.setValue("SeatHubFinalDir", x);
    installer.setValue("SeatHubAttemptId", id);
    // Retarget before any nested execute/kill/UAC event loop can deliver queued Next clicks.
    installer.setValue("TargetDir", seathubAttempt.stage);
    var page = gui.currentPageWidget();
    if (page && page.TargetDirectoryLineEdit) {
        page.TargetDirectoryLineEdit.setText(seathubAttempt.stage);
        page.TargetDirectoryLineEdit.enabled = false;
    }
    if (page && page.MessageLabel)
        page.MessageLabel.setText("SeatHub is already installed here. Setup installs the new version next to it and swaps it in when it is ready. You stay signed in.");
    seathubLog("retarget " + x + " -> " + seathubAttempt.stage);
    seathubJournalStart();
    seathubAttempt.step = "stop_client";
    if (!seathubStopClient(seathubJoin(x, SEATHUB_CLIENT), SEATHUB_CLIENT, false)
            || !seathubStopClient(seathubJoin(x, SEATHUB_CRASH_HANDLER), SEATHUB_CRASH_HANDLER, true)) {
        seathubFail("installer.locked_file", "stop_client", SEATHUB_STOP_FAILURE);
        return;
    }
    var pf = installer.toNativeSeparators(installer.environmentVariable("ProgramFiles")).toLowerCase();
    if (pf && x.toLowerCase().indexOf(pf + "\\") === 0 && !installer.hasAdminRights()
            && !seathubGainAdminRights()) {
        seathubFail("installer.access_denied", "preflight", SEATHUB_FAILURE);
        return;
    }
    seathubHook("callback_done");
};

Controller.prototype.seathubStarted = function()
{
    if (!seathubAttempt) return;
    var actual = installer.toNativeSeparators(installer.value("TargetDir"));
    if (actual.toLowerCase() !== seathubAttempt.stage.toLowerCase()) {
        // A different successful install must never be swapped/abandoned as this attempt's stage.
        installer.setValue("SeatHubFinalDir", "");
        seathubJournalEnd("cancelled", "preflight", "installer.unknown");
        seathubAttempt = null;
        return;
    }
    seathubAttempt.step = "extract";
    seathubHook("mid_extract");
};

Controller.prototype.seathubInterrupted = function()
{
    if (seathubAttempt) seathubAttempt.interrupted = true;
};

Controller.prototype.seathubFinished = function()
{
    if (!seathubAttempt || seathubAttempt.ended) { seathubFlushLog(); return; }
    var a = seathubAttempt;
    a.stageMs = Date.now() - a.clock;
    if (a.interrupted || installer.status != QInstaller.Success) {
        seathubAbandon();
        seathubJournalEnd("cancelled", "extract", "installer.extract_error");
        seathubFlushLog();
        return;
    }
    a.step = "verify";
    // Finished fires on abort too. Never rename the old app without these payload sentinels.
    if (!installer.fileExists(seathubJoin(a.stage, SEATHUB_MAINTENANCE_TOOL))
            || !installer.fileExists(seathubJoin(a.stage, SEATHUB_CLIENT))
            || !installer.fileExists(seathubJoin(a.stage, SEATHUB_CRASH_HANDLER))) {
        seathubAbandon();
        seathubFail("installer.verify_failed", "verify", SEATHUB_FAILURE);
        return;
    }
    var swapClock = Date.now();
    a.step = "swap_aside";
    if (!seathubRename(a.final, a.old)) {
        seathubAbandon();
        seathubFail("installer.locked_file", "swap_aside", SEATHUB_FAILURE);
        return;
    }
    seathubHook("between_renames");
    a.step = "swap_in";
    if (!seathubRename(a.stage, a.final)) {
        a.rollback = seathubRename(a.old, a.final) ? "ok" : "failed";
        a.state = a.rollback === "ok" ? "old_intact" : "none";
        seathubAbandon();
        seathubFail(a.rollback === "ok" ? "installer.locked_file" : "installer.rollback_failed", "swap_in", SEATHUB_FAILURE);
        return;
    }
    a.swapMs = Date.now() - swapClock;
    a.state = "new_live";
    seathubHook("after_swap");
    a.step = "registry";
    var newRows = seathubRows(a.stage);
    var registryOk = seathubPatchRows(a.stage, a.final);
    if (registryOk) seathubDeleteRows(a.final, newRows);
    installer.setValue("TargetDir", a.final);
    a.step = "cleanup";
    seathubRemoveTree(a.old);
    seathubJournalEnd("success", registryOk ? "cleanup" : "registry", registryOk ? "installer.unknown" : "installer.script_error");
    seathubFlushLog();
};

function seathubFail(cls, step, text)
{
    seathubLog("failed " + step + " " + cls);
    seathubJournalEnd("failed", step, cls);
    seathubFlushLog();
    QMessageBox.critical("SeatHubUpdateFailed", SEATHUB_TITLE, text);
    // interrupt() cancels ongoing installation; reject() also closes a pre-install wizard.
    installer.interrupt();
    gui.reject();
}

function seathubRandomId()
{
    var r = seathubPs("[guid]::NewGuid().ToString('N').Substring(0,16)");
    var id = String(r[0]).replace(/\s/g, "");
    if (!/^[0-9a-f]{16}$/.test(id)) throw new Error("attempt id unavailable");
    return id;
}

function seathubHook(name) {}
// Journal routines are intentionally supplied by the journal-writer task, after failure tests.
function seathubJournalOwnerTrusted(dir) { return false; }
function seathubJournalStart() {}
function seathubJournalEnd(outcome, step, cls)
{
    if (seathubAttempt) seathubAttempt.ended = true;
}

function seathubPsQuote(s) { return "'" + String(s).replace(/'/g, "''") + "'"; }
function seathubPs(script)
{
    return installer.execute(seathubJoin(installer.environmentVariable("SystemRoot"),
        "System32/WindowsPowerShell/v1.0/powershell.exe"),
        ["-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference='Stop';" + script], "");
}

function seathubRename(source, destination)
{
    for (var i = 0; i < SEATHUB_RENAME_TRIES; ++i) {
        var r = seathubPs("[IO.Directory]::Move(" + seathubPsQuote(source) + "," + seathubPsQuote(destination) + ")");
        if (r.length >= 2 && Number(r[1]) === 0) { seathubLog("rename ok " + source + " -> " + destination); return true; }
        if (i + 1 < SEATHUB_RENAME_TRIES)
            for (var s = 0; s < SEATHUB_RENAME_WAIT_S; ++s) seathubSleepOneSecond();
    }
    return false;
}

function seathubRemoveTree(path)
{
    // cmd rmdir removes a junction itself, not its destination. Never execute an uninstaller here.
    var r = installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", "/s", "/q", path], "");
    return !installer.fileExists(path);
}

function seathubRows(location)
{
    var ps = "$loc=" + seathubPsQuote(location) + ";"
        + "foreach($root in @('HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall','HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall','HKLM:\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall')){"
        + "Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | ForEach-Object {"
        + "$p=Get-ItemProperty -LiteralPath $_.PSPath;if($p.DisplayName -eq 'SeatHub' -and $p.InstallLocation -eq $loc){$_.Name}}}";
    var r = seathubPs(ps);
    return r.length >= 2 && Number(r[1]) === 0 ? String(r[0]).split(/[\r\n]+/).filter(function(s){return s.indexOf("HKEY_") === 0;}) : [];
}

function seathubPatchRows(stage, finalDir)
{
    var rows = seathubRows(stage);
    var tool = seathubJoin(finalDir, SEATHUB_MAINTENANCE_TOOL);
    var fields = [["InstallLocation",finalDir],["UninstallString",'"' + tool + '"'],
        ["ModifyPath",'"' + tool + '"'],["DisplayIcon",seathubJoin(finalDir, SEATHUB_CLIENT)]];
    if (rows.length !== 1) return false;
    for (var j = 0; j < fields.length; ++j) {
        var r = installer.execute(seathubSystemTool("reg.exe"), ["add",rows[0],"/v",fields[j][0],"/t","REG_SZ","/d",fields[j][1],"/f"], "");
        if (r.length < 2 || Number(r[1]) !== 0) return false;
        seathubLog("registry write ok " + fields[j][0]);
    }
    return true;
}

function seathubDeleteRows(location, exclude)
{
    var rows = seathubRows(location);
    exclude = exclude || [];
    for (var i = 0; i < rows.length; ++i)
        if (exclude.indexOf(rows[i]) < 0) installer.execute(seathubSystemTool("reg.exe"), ["delete",rows[i],"/f"], "");
}

function seathubAbandon()
{
    if (!seathubAttempt) return;
    var a = seathubAttempt;
    seathubDeleteRows(a.stage);
    seathubRemoveTree(a.stage);
    if (installer.fileExists(seathubJoin(a.final, SEATHUB_CLIENT)))
        installer.performOperation("CreateShortcut", [seathubJoin(a.final, SEATHUB_CLIENT),
            installer.value("AllUsersStartMenuProgramsPath") + "/SeatHub.lnk", "workingDirectory=" + a.final,
            "iconPath=" + seathubJoin(a.final, SEATHUB_CLIENT), "description=SeatHub"]);
}

function seathubRecover(x)
{
    // Recovery-table implementation is expanded by the failure-matrix task.
    var r = seathubPs("$x=" + seathubPsQuote(x) + ";$parent=Split-Path $x;$leaf=Split-Path $x -Leaf;"
        + "$dirs=@(Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue | Where-Object {$_.Name -match ('^'+[regex]::Escape($leaf)+'\\.(stage|old)-[0-9a-f]{16}$')} | Sort-Object LastWriteTimeUtc -Descending);"
        + "$dirs.FullName -join [Environment]::NewLine");
    if (r.length < 2 || Number(r[1]) !== 0) return;
    var dirs = String(r[0]).split(/[\r\n]+/).filter(function(p){return p !== "";});
    if (!installer.fileExists(x)) {
        for (var i = 0; i < dirs.length; ++i)
            if (dirs[i].indexOf(x + ".old-") === 0 && seathubRename(dirs[i], x)) break;
    }
    for (var j = 0; j < dirs.length; ++j) {
        if (dirs[j].indexOf(x + ".stage-") === 0) seathubDeleteRows(dirs[j]);
        if (installer.fileExists(x)) seathubRemoveTree(dirs[j]);
    }
}

// The in-app updater quits SeatHub on its own, but a hand-run setup may find it open, and open
// files can prevent the live folder from being renamed.
// `imageName` is the bare filename taskkill's /IM wants (SEATHUB_CLIENT only - see `pathScopedKill`
// below); `client` is the full path isProcessRunning()/killProcess() key on. They used to be the
// same hardcoded constant, which meant this function could only ever wait for SeatHub.exe; naming
// both explicitly is what makes it reusable for the crash handler too (06.3.1 D-02).
// `pathScopedKill`: CR-03 fix. `SEATHUB_CLIENT` ("SeatHub.exe") is specific enough that a
// system-wide `taskkill /IM` was low risk when it was the only caller. `crashpad_handler.exe` is
// the upstream, unmodified binary name every application bundling sentry-native/crashpad ships -
// this phase's own SPIKE research (06.3.1-RESEARCH-SPIKE-CRASHPAD.md § 6.1) found, from a real
// registry read on the target class of machine, that Discord, two games and a screen-recording
// tool already register their own `crashpad_handler.exe` on that exact machine. A blind `/IM` kill
// for the crash handler's elevated fallback would forcibly terminate one of THOSE unrelated
// processes as a side effect of installing or updating SeatHub. Callers pass `true` for the crash
// handler (path-scoped only, see `seathubKillProcessByPath()`) and `false` for `SeatHub.exe`
// (unchanged - the existing, already-low-risk `/IM` fallback).
function seathubStopClient(client, imageName, pathScopedKill)
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

    // Still running: most likely an elevated process (RunProgram starts SeatHub from the elevated
    // setup, and crashpad_handler.exe is spawned by SeatHub) that a non-elevated setup cannot stop.
    // Stop it through the elevated server instead.
    seathubLog(client + " is still running; stopping it with elevated rights");
    if (!installer.hasAdminRights() && !seathubGainAdminRights())
        return false;

    if (pathScopedKill) {
        // CR-03: never kill by bare image name here - only the process whose own image path is
        // inside this install (`client`, already an absolute path). If the scoped kill cannot even
        // be run (no PowerShell, an unexpected script failure), skip the kill entirely rather than
        // fall back to an unscoped `taskkill /IM` - the existing wait/ladder above already gave
        // this process every other chance to stop, and the uninstall/update still proceeds (or
        // fails cleanly at the purge step below) exactly as it did before this elevated fallback
        // existed.
        if (!seathubKillProcessByPath(client))
            seathubLog("could not confirm the path-scoped kill ran for " + client + "; skipping it");
    }
    else {
        installer.execute(seathubSystemTool("taskkill.exe"), ["/F", "/IM", imageName], "");
    }

    if (installer.isProcessRunning(client)) {
        seathubLog(client + " could not be stopped");
        return false;
    }
    return true;
}

// CR-03: stops only the process at `fullPath` - never by bare image name (see `seathubStopClient`'s
// own comment on `pathScopedKill`). Returns false only when the scoped kill could not even be
// attempted (so the caller skips it rather than widen to an unscoped kill); a script that ran but
// matched nothing (the target already exited, or was never this machine's own SeatHub install) is
// still a successful attempt.
function seathubKillProcessByPath(fullPath)
{
    var baseName = seathubLeaf(fullPath).replace(/\.exe$/i, "");
    var escapedPath = fullPath.replace(/'/g, "''");
    // Get-Process -Name (no extension) finds every process with this image name on the machine;
    // Where-Object narrows that to the one whose own .Path matches this install's exact file -
    // -ErrorAction SilentlyContinue on both cmdlets means "no match" is not a script error.
    var script = "Get-Process -Name '" + baseName + "' -ErrorAction SilentlyContinue | "
        + "Where-Object { $_.Path -eq '" + escapedPath + "' } | "
        + "Stop-Process -Force -ErrorAction SilentlyContinue";
    var powershell = seathubJoin(installer.environmentVariable("SystemRoot"),
        "System32/WindowsPowerShell/v1.0/powershell.exe");
    var result = installer.execute(powershell, ["-NoProfile", "-NonInteractive", "-Command", script], "");
    if (result.length < 2) {
        seathubLog("could not run the path-scoped kill for " + fullPath);
        return false;
    }
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

// IFW AppendFile makes a backup when its destination already exists. Buffer, then write once
// to a new attempt-specific local debug path; this log is not the machine-wide typed journal.
function seathubLog(message)
{
    console.log("SeatHub: " + message);
    seathubLogLines.push(new Date().toISOString() + " SeatHub: " + message + "\r\n");
}

function seathubFlushLog()
{
    try {
        var temp = installer.environmentVariable("TEMP");
        if (temp !== "" && seathubLogLines.length) {
            var path = seathubJoin(temp, "SeatHub-update-" + seathubRandomId() + ".txt");
            installer.performOperation("AppendFile",
                [path, seathubLogLines.join("")]);
            seathubLogLines = [];
        }
    } catch (e) {
        // Local debug logging cannot change the update outcome.
    }
}

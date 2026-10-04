// IFW 4.7 controller. ADR-0070: extract beside the live app, then swap; never run an uninstaller.
// Local 4.7 scripting-installer.html/noninteractive.html are the API authority.
//
// ADR-0077 / INTENT SC8 + SC9. What this script now guarantees, and the IFW 4.7.0 facts it rests on:
//   * runInstaller() calls dropAdminRights() BEFORE it emits installationFinished
//     (packagemanagercore_p.cpp, "if (adminRightsGained) m_core->dropAdminRights();"), so a setup that
//     was elevated only by IFW's internal elevation runs this whole handler unelevated: installer.execute
//     goes through QProcessWrapper, which only reaches the elevated server while RemoteClient is active.
//     seathubRegainElevation() re-activates the already started server first (no second UAC prompt:
//     RemoteClientPrivate::maybeStartServer returns at once when m_serverStarted is set).
//   * Every failing operation records its real numeric Windows error; the FIRST one wins and later
//     rollback/cleanup results never overwrite it (journal `code`, V37 clamp unchanged).
//   * Registration rows are addressed by hive + view + key, where the key must equal the ProductUUID that
//     the maintenance tool stores, plus the install/maintenance paths. A failed query is unknown, every
//     write and delete is read back, and only proven-dead owned rows are removed.
//   * Pre-journal quiescence (ADR-0077 item 7) is a STATE rule: no relevant installer, child or elevation
//     owner remains, and the mutation boundary is exclusively acquired. No duration is encoded anywhere
//     (docs/spec/timing.md V38 is open). Journal age (V35, the journal-start rule) is never read here.
var SEATHUB_TITLE = "SeatHub Setup";
var SEATHUB_MAINTENANCE_TOOL = "SeatHubMaintenanceTool.exe";
var SEATHUB_CLIENT = "SeatHub.exe";
var SEATHUB_CRASH_HANDLER = "crashpad_handler.exe";
var SEATHUB_PUBLISHER = "Seven Hills"; // config.xml <Publisher>
var SEATHUB_UNINSTALL_ROOTS = [
    "HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    "HKCU:\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    "HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    "HKLM:\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"];
// docs/spec V36 and ADR-0070 item 1: two waits only, on failure; no success-path delay.
var SEATHUB_RENAME_TRIES = 3;
var SEATHUB_RENAME_WAIT_S = 1;
var SEATHUB_FAILURE = "SeatHub could not finish updating. Your current version is unchanged and still works. SeatHub will try again by itself.";
var SEATHUB_STOP_FAILURE = "SeatHub is still running and could not be closed. Close it and run this setup again.";
var seathubAttempt = null;
var seathubLogLines = [];
var seathubBoundaryHeld = false;
var seathubLastExit = NaN;

function Controller()
{
    if (!installer.isInstaller()) return;
    installer.installationStarted.connect(this, this.seathubStarted);
    installer.installationFinished.connect(this, this.seathubFinished);
    // IFW 4.7.0 error-dialog Cancel calls core.interrupt() too. Only the GUI
    // interrupted signal identifies a genuine progress-page cancellation.
    gui.interrupted.connect(this, this.seathubInterrupted);
    installer.statusChanged.connect(this, this.seathubStatus);
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    if (!installer.isInstaller() || seathubAttempt) return;
    var x = seathubChosenTargetDir();
    if (!x) return;
    var dirs = seathubSiblingDirs(x);
    seathubPrepareJournal();
    var interrupted = dirs !== null && dirs.length > 0;
    if (!installer.fileExists(x) && !interrupted) return;
    if (!interrupted) {
        // Nonempty directories without a tool are staged too: never accept an overwrite prompt.
        var entries = seathubPs("@(Get-ChildItem -LiteralPath " + seathubPsQuote(x) + " -Force).Count");
        if (entries.length >= 2 && Number(entries[1]) === 0 && Number(entries[0]) === 0) return;
    }
    var id = seathubRandomId();
    seathubAttempt = {id:id, final:x, stage:x + ".stage-" + id, old:x + ".old-" + id,
        started:new Date().toISOString(), clock:Date.now(), stageMs:0, swapMs:0,
        outcome:"failed", step:"preflight", cls:"installer.unknown", code:0,
        rollback:"not_needed", state:"old_intact", interrupted:false, ended:false,
        needsAdmin:seathubNeedsAdmin(x), oldId:"", newId:""};
    // ADR-0077 item 7: admit a mutation only when no other installer is live. Ambiguity is blocked, not
    // abandoned: nothing on disk is touched and the client's retry state tries again later.
    var other = seathubOtherSetupState();
    if (other !== "none") {
        seathubLog("another SeatHub setup is " + other + "; not mutating");
        seathubBlock("installer.locked_file", SEATHUB_FAILURE);
        return;
    }
    if (interrupted) {
        // Recovery renames and deletes inside the install parent: it needs the same elevation as the swap.
        if (seathubAttempt.needsAdmin && !installer.hasAdminRights() && !seathubGainAdminRights()) {
            seathubFail("installer.access_denied", "preflight", SEATHUB_FAILURE);
            return;
        }
        seathubPrepareJournal();
        if (seathubAcquireBoundary() !== "acquired") {
            seathubBlock("installer.locked_file", SEATHUB_FAILURE);
            return;
        }
        seathubRecoverInterruptedSwap(x, dirs);
        if (!installer.fileExists(x)) {
            // Only a stage remained: this is a fresh install, which IFW handles by itself.
            seathubReleaseBoundary();
            seathubAttempt = null;
            return;
        }
    }
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
    var pf = seathubProgramFiles().toLowerCase();
    if (pf && x.toLowerCase().indexOf(pf + "\\") === 0 && !installer.hasAdminRights()
            && !seathubGainAdminRights()) {
        seathubFail("installer.access_denied", "preflight", SEATHUB_FAILURE);
        return;
    }
    seathubPrepareJournal();
    if (!seathubAttempt.journalStarted) seathubJournalStart();
    if (seathubAcquireBoundary() !== "acquired") {
        seathubBlock("installer.locked_file", SEATHUB_FAILURE);
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

Controller.prototype.seathubStatus = function(status)
{
    if (status == QInstaller.Canceled && seathubAttempt && !seathubAttempt.ended
            && (seathubAttempt.step === "preflight" || seathubAttempt.step === "stop_client")) {
        // Ready-page quit has no installationFinished signal, and no IFW
        // operations can have failed yet. Extraction cancellation waits for
        // installationFinished, after IFW has rolled back.
        seathubJournalEnd("cancelled", seathubAttempt.step, "installer.unknown");
        seathubCloseWizardAfterEnd();
    }
};

Controller.prototype.seathubFinished = function()
{
    if (!seathubAttempt || seathubAttempt.ended) { seathubFlushLog(); return; }
    var a = seathubAttempt;
    // IFW dropped its internal elevation before emitting this signal. Everything below (rename, registry,
    // stage removal, shortcut, journal) must run with the same elevation as the rest of the install.
    seathubRegainElevation(a);
    a.stageMs = Date.now() - a.clock;
    if (a.interrupted || installer.status != QInstaller.Success) {
        seathubAbandon();
        seathubJournalEnd(a.interrupted ? "cancelled" : "failed", a.step,
            a.interrupted ? "installer.unknown" : "installer.extract_error");
        seathubFlushLog();
        return;
    }
    a.step = "verify";
    seathubHook("before_verify");
    // Finished fires on abort too. Never rename the old app without these payload sentinels.
    if (!installer.fileExists(seathubJoin(a.stage, SEATHUB_MAINTENANCE_TOOL))
            || !installer.fileExists(seathubJoin(a.stage, SEATHUB_CLIENT))
            || !installer.fileExists(seathubJoin(a.stage, SEATHUB_CRASH_HANDLER))) {
        seathubAbandon();
        seathubFail("installer.verify_failed", "verify", SEATHUB_FAILURE);
        return;
    }
    // The ProductUUID each maintenance tool stores is the proof that ties a registry row to an install.
    a.newId = seathubToolId(a.stage);
    a.oldId = seathubToolId(a.final);
    var swapClock = Date.now();
    a.step = "swap_aside";
    if (!seathubRename(a.final, a.old)) {
        seathubAbandon();
        seathubFail(seathubClassForCode(seathubLastExit, "installer.unknown"), "swap_aside", SEATHUB_FAILURE);
        return;
    }
    seathubHook("between_renames");
    a.step = "swap_in";
    if (!seathubRename(a.stage, a.final)) {
        var inFailure = seathubLastExit;
        a.rollback = seathubRename(a.old, a.final) ? "ok" : "failed";
        a.state = a.rollback === "ok" ? "old_intact" : "none";
        seathubAbandon();
        seathubFail(a.rollback === "ok" ? seathubClassForCode(inFailure, "installer.unknown") : "installer.rollback_failed",
            "swap_in", SEATHUB_FAILURE);
        return;
    }
    a.swapMs = Date.now() - swapClock;
    a.state = "new_live";
    seathubHook("after_swap");
    a.step = "registry";
    var registryOk = seathubRepairRegistration({final:a.final, stage:a.stage, old:a.old,
        newId:a.newId || seathubToolId(a.final), oldId:a.oldId});
    installer.setValue("TargetDir", a.final);
    seathubEnsureShortcut(a.final);
    a.step = "cleanup";
    seathubRemoveTree(a.old);
    // A rejected registry write or delete never reports success: the app is updated, the journal says what is left.
    seathubJournalEnd(registryOk ? "success" : "failed", registryOk ? "cleanup" : "registry",
        registryOk ? "installer.unknown" : "installer.script_error");
    seathubFlushLog();
};

// A quiescence block is not a failure: nothing was mutated and the old app is untouched, so the attempt is
// journaled as `cancelled`. The client never reports a cancelled record and it spends none of the four
// attempts, which is the same policy as its own blocks (ADR-0077 item 7: blocked, not abandoned).
function seathubBlock(cls, text)
{
    seathubFail(cls, "preflight", text, "cancelled");
}

function seathubFail(cls, step, text, outcome)
{
    outcome = outcome || "failed";
    seathubLog(outcome + " " + step + " " + cls);
    seathubJournalEnd(outcome, step, cls);
    seathubFlushLog();
    QMessageBox.critical("SeatHubUpdateFailed", SEATHUB_TITLE, text);
    // interrupt() cancels ongoing installation; reject() also closes a pre-install wizard.
    installer.interrupt();
    installer.setCanceled();
    gui.reject();
}

// The Ready-page Cancel closes the wizard (QDialog::done) BEFORE the Canceled status callback
// runs, and the journal end's threaded AppendFile/Delete enter a nested QEventLoop that removes
// the automatic Quit Qt posted at last-window closure (IFW 4.7.0 packagemanagercore_p.cpp 320-335;
// Qt 6.6 qeventloop.cpp 176-179), so the process never terminates. With the attempt already ended,
// re-showing the wizard and rejecting again posts a fresh Quit AFTER every nested journal loop.
// Guarded exactly-once, and only when the end record is written; nothing runs after the final
// close - no operation, execute, prune, flush or journal write.
function seathubCloseWizardAfterEnd()
{
    if (!seathubAttempt || !seathubAttempt.ended || seathubAttempt.wizardClosed) return;
    seathubAttempt.wizardClosed = true;
    seathubLog("re-show wizard");
    gui.setSilent(false);
    seathubLog("close wizard");
    gui.rejectWithoutPrompt();
}

function seathubRandomId()
{
    var r = seathubPs("[guid]::NewGuid().ToString('N').Substring(0,16)");
    var id = String(r[0]).replace(/\s/g, "");
    if (!/^[0-9a-f]{16}$/.test(id)) throw new Error("attempt id unavailable");
    return id;
}

function seathubHook(name) {}
function seathubProgramFiles()
{
    return installer.toNativeSeparators(installer.environmentVariable("ProgramFiles"));
}

// ---------------------------------------------------------------------------------------- numeric errors

function seathubIsOk(r) { return r && r.length >= 2 && Number(r[1]) === 0; }

function seathubExitCode(r) { return r && r.length >= 2 ? Number(r[1]) : NaN; }

// The first failing operation's real number is the one the journal keeps; rollback, abandon and cleanup
// run after it and must never replace it. Zero is "no failure" and an operation that never started has
// no number, so neither is recorded.
function seathubNoteCode(code)
{
    var a = seathubAttempt;
    code = Number(code);
    if (!a || !isFinite(code) || code === 0) return;
    if (a.code === 0) a.code = code;
}

// 5 is ERROR_ACCESS_DENIED, 32/33 are the sharing and lock violations. Anything else stays honest.
function seathubClassForCode(code, fallback)
{
    if (code === 5) return "installer.access_denied";
    if (code === 32 || code === 33) return "installer.locked_file";
    return fallback;
}

// Exits with the Win32 error of the innermost exception: the low word of an 0x8007xxxx HRESULT, otherwise
// the HRESULT itself. -2147024896 is 0x80070000 as a signed 32-bit integer, -65536 is 0xFFFF0000.
function seathubMoveScript(source, destination)
{
    return "try{[IO.Directory]::Move(" + seathubPsQuote(source) + "," + seathubPsQuote(destination) + ")}"
        + "catch{$e=$_.Exception;while($e.InnerException){$e=$e.InnerException};$h=[int]$e.HResult;"
        + "if($h -eq 0){exit 1};if(($h -band -65536) -eq -2147024896){exit ($h -band 65535)}else{exit $h}}";
}

// ---------------------------------------------------------------------------------------- elevation

function seathubUnderProgramFiles(path)
{
    var pf = seathubProgramFiles().toLowerCase();
    return pf !== "" && path.toLowerCase().indexOf(pf + "\\") === 0;
}

// True when renaming beside the install needs more than the launching token: Program Files, or any folder
// whose parent this token cannot write to (IFW elevates for those too, and drops the elevation again).
function seathubNeedsAdmin(dir)
{
    if (seathubUnderProgramFiles(dir)) return true;
    var probe = seathubJoin(seathubParent(dir), ".seathub-write-" + Date.now());
    var r = seathubPs("$f=[IO.File]::Open(" + seathubPsQuote(probe) + ",[IO.FileMode]::CreateNew);$f.Dispose();Remove-Item -LiteralPath "
        + seathubPsQuote(probe) + " -Force");
    return !seathubIsOk(r);
}

function seathubRegainElevation(a)
{
    if (!a.needsAdmin || installer.hasAdminRights()) return true;
    var regained = seathubGainAdminRights();
    seathubLog("admin rights were dropped by IFW; regained " + regained);
    return regained;
}

// ---------------------------------------------------------------------------------------- quiescence

// Is any other SeatHub setup or maintenance tool process alive? Our own process chain (this PowerShell,
// the installer and, once elevated, its server child) is excluded. A failed query is "unknown".
function seathubOtherSetupState()
{
    var self = seathubLeaf(installer.value("InstallerFilePath"));
    var r = seathubPs("$self=" + seathubPsQuote(self) + ";$all=@(Get-CimInstance Win32_Process);$mine=@{};$cur=[int]$PID;"
        + "while($cur -gt 0 -and -not $mine.ContainsKey($cur)){$p=$all | Where-Object {$_.ProcessId -eq $cur} | Select-Object -First 1;"
        + "if(-not $p){break};$mine[$cur]=1;$cur=[int]$p.ParentProcessId};"
        + "$others=@($all | Where-Object {($_.Name -like 'SeatHub*Setup*.exe' -or $_.Name -eq 'SeatHubMaintenanceTool.exe' -or ($self -ne '' -and $_.Name -eq $self))"
        + " -and -not $mine.ContainsKey([int]$_.ProcessId) -and -not ($mine.ContainsKey([int]$_.ParentProcessId) -and $self -ne '' -and $_.Name -eq $self)});"
        + "$others | ForEach-Object {$_.ProcessId.ToString()+' '+$_.Name}");
    if (!seathubIsOk(r)) return "unknown";
    return String(r[0]).replace(/\s/g, "") === "" ? "none" : "live";
}

function seathubBoundaryDir()
{
    return seathubJoin(seathubJoin(installer.environmentVariable("ProgramData"), "SeatHubSetup"), "mutation-boundary");
}

// The mutation boundary is a directory made by `mkdir`, which fails if it exists, so only one setup holds it.
// A leftover boundary is reclaimed only on proof: the caller has just seen no other live setup, so its
// holder is dead. Elapsed time is never the proof.
function seathubAcquireBoundary()
{
    if (seathubBoundaryHeld) return "acquired";
    var dir = seathubBoundaryDir();
    // `mkdir` would also create a missing parent with inherited, untrusted permissions: the secured
    // SeatHubSetup folder must already exist.
    if (!installer.fileExists(seathubParent(dir))) { seathubLog("mutation boundary unavailable: no secured folder"); return "error"; }
    for (var pass = 0; pass < 2; ++pass) {
        var r = installer.execute(seathubSystemTool("cmd.exe"), ["/c", "mkdir", dir], "");
        if (seathubIsOk(r) && installer.fileExists(dir)) {
            seathubBoundaryHeld = true;
            seathubLog("mutation boundary acquired");
            return "acquired";
        }
        if (!installer.fileExists(dir)) { seathubLog("mutation boundary unavailable"); return "error"; }
        if (pass === 0) {
            seathubLog("stale mutation boundary: no live setup holds it; reclaiming");
            installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", dir], "");
            if (installer.fileExists(dir)) return "error";
        }
    }
    return "contested";
}

function seathubReleaseBoundary()
{
    if (!seathubBoundaryHeld) return;
    seathubBoundaryHeld = false;
    var dir = seathubBoundaryDir();
    installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", dir], "");
    if (installer.fileExists(dir)) seathubLog("mutation boundary could not be released");
}

// ---------------------------------------------------------------------------------------- journal

var seathubJournalDir = "";
function seathubJournalOwnerTrusted(dir)
{
    var r = seathubPs("$p=" + seathubPsQuote(dir) + ";$d=Get-Item -LiteralPath $p -Force;"
        + "if($d.Attributes -band [IO.FileAttributes]::ReparsePoint){exit 1};$a=Get-Acl -LiteralPath $p;"
        + "$owner=$a.GetOwner([Security.Principal.SecurityIdentifier]).Value;"
        + "if($owner -notin ('S-1-5-18','S-1-5-32-544') -or !$a.AreAccessRulesProtected){exit 1};"
        + "$rules=$a.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]);if($rules.Count -ne 3){exit 1};"
        + "foreach($sid in ('S-1-5-18','S-1-5-32-544','S-1-5-32-545')){"
        + "$v=[Security.AccessControl.FileSystemAccessRule[]]($rules | Where-Object {$_.IdentityReference.Value -eq $sid});"
        + "$rights=if($sid -eq 'S-1-5-32-545'){[int]([Security.AccessControl.FileSystemRights]::ReadAndExecute -bor [Security.AccessControl.FileSystemRights]::Synchronize)}else{[int][Security.AccessControl.FileSystemRights]::FullControl};"
        + "if($v.Count -ne 1 -or $v[0].IsInherited -or $v[0].AccessControlType -ne 'Allow' -or [int]$v[0].FileSystemRights -ne $rights -or [int]$v[0].InheritanceFlags -ne 3 -or [int]$v[0].PropagationFlags -ne 0){exit 1}};'trusted'");
    return r.length >= 2 && Number(r[1]) === 0 && String(r[0]).indexOf("trusted") >= 0;
}

function seathubJournalAclScript(dir, existing)
{
    return "$a=New-Object Security.AccessControl.DirectorySecurity;"
        + "$a.SetAccessRuleProtection($true,$false);"
        + "$a.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')));"
        + "foreach($sid in ('S-1-5-18','S-1-5-32-544','S-1-5-32-545')){"
        + "$s=New-Object Security.Principal.SecurityIdentifier($sid);"
        + "$rights=if($sid -eq 'S-1-5-32-545'){'ReadAndExecute'}else{'FullControl'};"
        + "$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($s,$rights,'ContainerInherit,ObjectInherit','None','Allow')))};"
        + (existing ? "(New-Object IO.DirectoryInfo(" + seathubPsQuote(dir) + ")).SetAccessControl($a)"
                    : "[IO.Directory]::CreateDirectory(" + seathubPsQuote(dir) + ",$a) | Out-Null");
}

function seathubSecureJournalDirectory(dir)
{
    var r = seathubPs("$p=" + seathubPsQuote(dir) + ";if(Test-Path -LiteralPath $p){"
        + "if((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){'reparse'}else{'directory'}}else{'missing'}");
    if (r.length < 2 || Number(r[1]) !== 0) return false;
    if (String(r[0]).indexOf("reparse") >= 0) {
        // Unlink without traversing; do not repair ACLs or delete anything in the destination.
        var detached = installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", dir], "");
        if (detached.length < 2 || Number(detached[1]) !== 0) return false;
    }
    if (seathubJournalOwnerTrusted(dir)) return true;
    // A folder that fails the trust check is repaired IN PLACE. It may hold the only record of a failed
    // attempt, so it is never deleted; if the repair is refused (no elevation) the journal stays untouched.
    var existing = installer.fileExists(dir);
    if (existing) seathubLog("journal folder failed the trust check; repairing in place");
    r = seathubPs(seathubJournalAclScript(dir, existing));
    if (!seathubIsOk(r)) {
        seathubLog("journal folder not trusted and not repairable, exit " + seathubExitCode(r));
        return false;
    }
    if (!seathubJournalOwnerTrusted(dir)) return false;
    var acl = installer.execute(seathubSystemTool("icacls.exe"), [dir], "");
    return acl.length >= 2 && Number(acl[1]) === 0;
}

function seathubPrepareJournal()
{
    if (seathubJournalDir) return seathubJournalDir;
    try {
        var root = seathubJoin(installer.environmentVariable("ProgramData"), "SeatHubSetup");
        var dir = seathubJoin(root, "install-journal");
        if (!seathubSecureJournalDirectory(root) || !seathubSecureJournalDirectory(dir)) return "";
        seathubJournalDir = dir;
        seathubPruneJournal(0);
        return dir;
    } catch (e) { seathubLog("journal prepare unavailable"); return ""; }
}

function seathubPruneJournal(reserve)
{
    // V35: reserve the next bounded file before writing, not after exceeding the file cap.
    return seathubPs("$p=" + seathubPsQuote(seathubJournalDir) + ";$files=Get-ChildItem -LiteralPath $p -File -Force;"
        + "foreach($f in $files){if($f.Name -notmatch '^attempt-[0-9a-f]{16}\\.(start|end)\\.json$' -or $f.Length -gt 4096 -or $f.LastWriteTimeUtc -lt [DateTime]::UtcNow.AddDays(-7)){Remove-Item -LiteralPath $f.FullName -Force}};"
        + "$files=Get-ChildItem -LiteralPath $p -File -Force | Sort-Object LastWriteTimeUtc -Descending;"
        + "$bytes=0;$count=0;foreach($f in $files){$bytes+=$f.Length;$count++;"
        + "if($count -gt " + (8 - reserve) + " -or $bytes -gt " + (32768 - reserve * 4096) + "){Remove-Item -LiteralPath $f.FullName -Force}}");
}

function seathubJournalVersion(value)
{
    value = String(value);
    return /^[0-9]{1,4}(\.[0-9]{1,4}){1,3}$/.test(value) ? value : "unknown";
}

function seathubJournalRecord(a, outcome, step, cls, end)
{
    function duration(n) { return Math.max(0, Math.min(3600000, Math.round(n))); }
    return {v:1, attempt:a.id, from:seathubJournalVersion(installer.value("SeatHubFromVersion")),
        to:seathubJournalVersion(installer.value("ProductVersion")), mode:"staged", started:a.started,
        ended:end ? new Date().toISOString() : "", outcome:outcome, step:step, "class":cls,
        code:Math.max(-2147483648, Math.min(2147483647, Math.round(a.code))),
        rollback:a.rollback, state:a.state,
        ms:{stage:duration(a.stageMs), swap:duration(a.swapMs), total:duration(Date.now() - a.clock)},
        elevated:installer.hasAdminRights()};
}

// Fallback when the IFW AppendFile operation is refused: create-new through the same (elevated) process path.
function seathubWriteFileViaPs(file, text)
{
    var r = seathubPs("$s=[IO.File]::Open(" + seathubPsQuote(file) + ",[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);"
        + "try{$b=[Text.Encoding]::UTF8.GetBytes(" + seathubPsQuote(text) + ");$s.Write($b,0,$b.Length)}finally{$s.Dispose()}");
    if (!seathubIsOk(r)) seathubLog("journal fallback write failed, exit " + seathubExitCode(r));
    return seathubIsOk(r);
}

function seathubJournalWrite(a, outcome, step, cls, end)
{
    var kind = end ? "end" : "start";
    // Reserve both new paths before start. Ready-page rejection must not enter
    // execute()'s nested event loop after Qt has posted last-window quit.
    var closingBeforeInstall = end && a.journalStarted
        && (a.step === "preflight" || a.step === "stop_client");
    var dir = closingBeforeInstall ? seathubJournalDir : seathubPrepareJournal();
    if (!dir) { seathubLog("journal " + kind + " unavailable: folder not prepared"); return false; }
    var text = JSON.stringify(seathubJournalRecord(a, outcome, step, cls, end));
    var file = seathubJoin(dir, "attempt-" + a.id + (end ? ".end.json" : ".start.json"));
    if (!closingBeforeInstall) {
        var prune = seathubPruneJournal(end ? 1 : 2);
        if (!seathubIsOk(prune)) {
            seathubLog("journal prune failed, exit " + seathubExitCode(prune));
            // The bound is reserved ahead of the end record, so losing the end record is the worse failure.
            if (!end) return false;
        }
    }
    if (text.length > 4096) { seathubLog("journal " + kind + " unavailable: record too large"); return false; }
    if (installer.fileExists(file)) { seathubLog("journal " + kind + " unavailable: already exists"); return false; }
    var wrote = installer.performOperation("AppendFile", [file, text]);
    if (!wrote) {
        seathubLog("journal " + kind + " AppendFile refused");
        wrote = seathubWriteFileViaPs(file, text);
    }
    if (!wrote || !installer.fileExists(file)) { seathubLog("journal " + kind + " unavailable: not written"); return false; }
    if (end) installer.performOperation("Delete", [seathubJoin(dir, "attempt-" + a.id + ".start.json")]);
    return true;
}

function seathubJournalStart()
{
    var a = seathubAttempt;
    if (!a || a.ended || a.journalStarted) return;
    a.journalStarted = seathubJournalWrite(a, "failed", "stage", "installer.unknown", false);
}

function seathubJournalRecovered(state)
{
    var a = {id:seathubRandomId(), started:new Date().toISOString(), clock:Date.now(),
        stageMs:0, swapMs:0, code:0, rollback:"ok", state:state};
    seathubJournalWrite(a, "recovered", "recover", "installer.interrupted", true);
}

function seathubJournalEnd(outcome, step, cls)
{
    if (!seathubAttempt || seathubAttempt.ended) return;
    seathubAttempt.ended = true;
    if (!seathubJournalWrite(seathubAttempt, outcome, step, cls, true)) seathubLog("journal end unavailable");
    seathubReleaseBoundary();
}

// ---------------------------------------------------------------------------------------- operations

function seathubPsQuote(s) { return "'" + String(s).replace(/'/g, "''") + "'"; }
function seathubPs(script)
{
    return installer.execute(seathubJoin(installer.environmentVariable("SystemRoot"),
        "System32/WindowsPowerShell/v1.0/powershell.exe"),
        ["-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference='Stop';" + script], "");
}

function seathubRename(source, destination)
{
    var last = NaN;
    for (var i = 0; i < SEATHUB_RENAME_TRIES; ++i) {
        var r = seathubPs(seathubMoveScript(source, destination));
        if (seathubIsOk(r)) { seathubLastExit = 0; seathubLog("rename ok " + source + " -> " + destination); return true; }
        last = seathubExitCode(r);
        seathubLog("rename failed " + (isNaN(last) ? "(no exit code)" : "code " + last));
        if (i + 1 < SEATHUB_RENAME_TRIES)
            for (var s = 0; s < SEATHUB_RENAME_WAIT_S; ++s) seathubSleepOneSecond();
    }
    seathubLastExit = last;
    seathubNoteCode(last);
    return false;
}

function seathubRemoveTree(path)
{
    // cmd rmdir removes a junction itself, not its destination. Never execute an uninstaller here.
    var r = installer.execute(seathubSystemTool("cmd.exe"), ["/c", "rmdir", "/s", "/q", path], "");
    return !installer.fileExists(path);
}

function seathubShortcutPath()
{
    return installer.toNativeSeparators(installer.value("AllUsersStartMenuProgramsPath") + "/SeatHub.lnk");
}

// IFW's CreateShortcut with a working directory goes through COM in the calling process, not through the
// elevated server, so after a rollback it can be refused. Read the result back and recreate it elevated.
function seathubEnsureShortcut(dir)
{
    var exe = seathubJoin(dir, SEATHUB_CLIENT);
    var link = seathubShortcutPath();
    if (!installer.fileExists(exe) || installer.fileExists(link)) return true;
    installer.performOperation("CreateShortcut", [exe, installer.value("AllUsersStartMenuProgramsPath") + "/SeatHub.lnk",
        "workingDirectory=" + dir, "iconPath=" + exe, "description=SeatHub"]);
    if (installer.fileExists(link)) return true;
    var r = seathubPs("$s=(New-Object -ComObject WScript.Shell).CreateShortcut(" + seathubPsQuote(link) + ");"
        + "$s.TargetPath=" + seathubPsQuote(exe) + ";$s.WorkingDirectory=" + seathubPsQuote(dir) + ";"
        + "$s.IconLocation=" + seathubPsQuote(exe + ",0") + ";$s.Description='SeatHub';$s.Save()");
    if (!seathubIsOk(r)) seathubLog("shortcut recreate failed, exit " + seathubExitCode(r));
    return installer.fileExists(link);
}

// ---------------------------------------------------------------------------------------- registration

// ProductUUID is stored in the maintenance tool's .ini as an escaped QVariant: \0 for NUL and \xHH for
// the characters that follow one. Decode it and read the braced UUID that follows the key name.
function seathubToolId(dir)
{
    var ini = seathubJoin(dir, SEATHUB_MAINTENANCE_TOOL.replace(/\.exe$/i, ".ini"));
    if (!installer.fileExists(ini)) return "";
    var r = seathubPs("$t=[IO.File]::ReadAllText(" + seathubPsQuote(ini) + ");"
        + "$t=[regex]::Replace($t,'\\x5cx([0-9a-fA-F]{1,2})',{param($m)[string][char][Convert]::ToInt32($m.Groups[1].Value,16)});"
        + "$t=$t.Replace('\\0','');"
        + "if($t -match 'ProductUUID[^{]{0,16}(\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\})'){$Matches[1]}else{exit 7}");
    if (!seathubIsOk(r)) return "";
    var id = String(r[0]).replace(/\s/g, "").toLowerCase();
    return /^\{[0-9a-f-]{36}\}$/.test(id) ? id : "";
}

// Every SeatHub uninstall row in every hive and view, with a flag that is false when any part of the
// query failed: a failed query is unknown, never "no rows".
function seathubQueryRows()
{
    var roots = SEATHUB_UNINSTALL_ROOTS.map(seathubPsQuote).join(",");
    var r = seathubPs("$o=@();$bad=0;foreach($root in @(" + roots + ")){"
        + "if(-not (Test-Path -LiteralPath $root)){continue};"
        + "try{$items=@(Get-ChildItem -LiteralPath $root -ErrorAction Stop)}catch{$bad=1;continue};"
        + "foreach($i in $items){try{$p=Get-ItemProperty -LiteralPath $i.PSPath -ErrorAction Stop}catch{$bad=1;continue};"
        + "if($p.DisplayName -eq 'SeatHub'){$o+=($i.Name+'|'+$p.Publisher+'|'+$p.InstallLocation+'|'+$p.UninstallString)}}};"
        + "$o;if($bad){exit 9}");
    if (!seathubIsOk(r)) return {ok:false, rows:[]};
    var rows = [];
    var lines = String(r[0]).split(/[\r\n]+/);
    for (var i = 0; i < lines.length; ++i) {
        var f = lines[i].split("|");
        if (f.length < 4 || f[0].indexOf("HKEY_") !== 0) continue;
        rows.push({key:f[0], id:f[0].substring(f[0].lastIndexOf("\\") + 1).toLowerCase(),
            publisher:f[1], location:f[2], uninstall:f.slice(3).join("|")});
    }
    return {ok:true, rows:rows};
}

function seathubNormalPath(p)
{
    return installer.toNativeSeparators(String(p)).replace(/^"|"$/g, "").replace(/[\\\/]+$/, "").toLowerCase();
}

// A row is OURS only when the publisher, an install location we manage and the maintenance tool path all
// agree. The display name alone never decides, and neither does a version.
function seathubOwnedRow(row, ctx)
{
    if (row.publisher !== SEATHUB_PUBLISHER) return false;
    var dirs = [ctx.final, ctx.stage, ctx.old].map(seathubNormalPath);
    var tools = [ctx.final, ctx.stage, ctx.old].map(function(d) { return seathubNormalPath(seathubJoin(d, SEATHUB_MAINTENANCE_TOOL)); });
    return dirs.indexOf(seathubNormalPath(row.location)) >= 0 && tools.indexOf(seathubNormalPath(row.uninstall)) >= 0;
}

function seathubRegFields(finalDir)
{
    var tool = seathubJoin(finalDir, SEATHUB_MAINTENANCE_TOOL);
    return [["InstallLocation", finalDir], ["UninstallString", '"' + tool + '"'],
        ["ModifyPath", '"' + tool + '"'], ["DisplayIcon", seathubJoin(finalDir, SEATHUB_CLIENT)]];
}

function seathubReadValues(key, fields)
{
    var script = "$k=Get-ItemProperty -LiteralPath " + seathubPsQuote("Registry::" + key) + ";";
    for (var i = 0; i < fields.length; ++i) script += "'" + fields[i][0] + "='+$k." + fields[i][0] + ";";
    var r = seathubPs(script);
    if (!seathubIsOk(r)) return null;
    var out = {};
    var lines = String(r[0]).split(/[\r\n]+/);
    for (var j = 0; j < lines.length; ++j) {
        var at = lines[j].indexOf("=");
        if (at > 0) out[lines[j].substring(0, at)] = lines[j].substring(at + 1);
    }
    return out;
}

// Write every field, then read every field back. A partial patch is left addressable by its key (the
// ProductUUID), so the next run finds it without relying on the half-patched InstallLocation.
function seathubPatchRow(key, finalDir)
{
    var fields = seathubRegFields(finalDir);
    for (var j = 0; j < fields.length; ++j) {
        var r = installer.execute(seathubSystemTool("reg.exe"), ["add", key, "/v", fields[j][0], "/t", "REG_SZ", "/d", fields[j][1], "/f"], "");
        if (!seathubIsOk(r)) { seathubNoteCode(seathubExitCode(r)); seathubLog("registry write failed " + fields[j][0]); return false; }
        seathubLog("registry write ok " + fields[j][0]);
    }
    var back = seathubReadValues(key, fields);
    if (!back) { seathubLog("registry readback unknown"); return false; }
    for (var k = 0; k < fields.length; ++k)
        if (back[fields[k][0]] !== fields[k][1]) { seathubLog("registry readback differs " + fields[k][0]); return false; }
    return true;
}

// Delete exactly these keys, then query again: a delete that was reported but did not stick is a failure.
function seathubDeleteKeys(keys)
{
    var ok = true;
    for (var i = 0; i < keys.length; ++i) {
        var r = installer.execute(seathubSystemTool("reg.exe"), ["delete", keys[i], "/f"], "");
        if (!seathubIsOk(r)) { seathubNoteCode(seathubExitCode(r)); seathubLog("registry delete failed"); ok = false; }
    }
    if (!keys.length) return true;
    var after = seathubQueryRows();
    if (!after.ok) return false;
    for (var j = 0; j < after.rows.length; ++j)
        if (keys.indexOf(after.rows[j].key) >= 0) { seathubLog("registry delete read back as still present"); ok = false; }
    return ok;
}

// Patch the new row, prove it is usable, and only then remove the rows that are provably dead: owned by
// us, and not the live maintenance tool's ProductUUID. Returns true only when no owned duplicate remains.
function seathubRepairRegistration(ctx)
{
    var q = seathubQueryRows();
    if (!q.ok) { seathubLog("registry query unknown; changing nothing"); return false; }
    var live = [];
    for (var i = 0; i < q.rows.length; ++i)
        if (ctx.newId && q.rows[i].id === ctx.newId && seathubOwnedRow(q.rows[i], ctx)) live.push(q.rows[i]);
    if (live.length !== 1) { seathubLog("registry: expected one row for the live ProductUUID, found " + live.length); return false; }
    var keep = live[0].key;
    if (!seathubPatchRow(keep, ctx.final)) return false;
    if (!installer.fileExists(seathubJoin(ctx.final, SEATHUB_MAINTENANCE_TOOL))) return false;
    var again = seathubQueryRows();
    if (!again.ok) return false;
    var dead = [];
    for (var j = 0; j < again.rows.length; ++j)
        if (again.rows[j].key !== keep && seathubOwnedRow(again.rows[j], ctx)) dead.push(again.rows[j].key);
    return seathubDeleteKeys(dead);
}

// An abandoned stage left its own row behind: owned, and registered at the stage folder.
function seathubDeleteRowsAt(location, ctx)
{
    var q = seathubQueryRows();
    if (!q.ok) { seathubLog("registry query unknown; leaving stage rows"); return false; }
    var dead = [];
    for (var i = 0; i < q.rows.length; ++i)
        if (seathubNormalPath(q.rows[i].location) === seathubNormalPath(location) && seathubOwnedRow(q.rows[i], ctx))
            dead.push(q.rows[i].key);
    return seathubDeleteKeys(dead);
}

function seathubAbandon()
{
    if (!seathubAttempt) return;
    var a = seathubAttempt;
    seathubDeleteRowsAt(a.stage, {final:a.final, stage:a.stage, old:a.old});
    if (!seathubRemoveTree(a.stage)) {
        // An extraction ACL denial can survive IFW's rollback. Repair only this attempt's
        // newly-created stage, never the live tree or arbitrary stale/locked siblings.
        installer.execute(seathubSystemTool("icacls.exe"), [a.stage,"/reset","/T","/C","/L"], "");
        seathubRemoveTree(a.stage);
    }
    // IFW's rollback undoes the stage's shortcut, which replaced the live one.
    seathubEnsureShortcut(a.final);
}

function seathubSiblingDirs(x)
{
    var r = seathubPs("$x=" + seathubPsQuote(x) + ";$parent=Split-Path $x;$leaf=Split-Path $x -Leaf;"
        + "$dirs=@(Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue | Where-Object {$_.Name -match ('^'+[regex]::Escape($leaf)+'\\.(stage|old)-.+$')} | Sort-Object LastWriteTimeUtc -Descending);"
        + "$dirs.FullName -join [Environment]::NewLine");
    if (!seathubIsOk(r)) return null;
    return String(r[0]).split(/[\r\n]+/).filter(function(p){return p !== "";});
}

function seathubRecoverInterruptedSwap(x, dirs)
{
    // ADR-0070 recovery table: newest old first; cleanup must never require an uninstaller.
    if (!dirs) dirs = seathubSiblingDirs(x);
    if (!dirs) return;
    if (!installer.fileExists(x)) {
        for (var i = 0; i < dirs.length; ++i)
            if (dirs[i].indexOf(x + ".old-") === 0 && seathubRename(dirs[i], x)) {
                seathubLog("recovered_after_interrupted_swap");
                seathubJournalRecovered("old_intact");
                break;
            }
    }
    for (var j = 0; j < dirs.length; ++j) {
        if (dirs[j].indexOf(x + ".old-") === 0 && installer.fileExists(x)) {
            // A kill after swap leaves the new GUID registered under its now-missing stage path, possibly
            // half patched. The row is found by the live tool's ProductUUID, not by InstallLocation.
            var stage = x + ".stage-" + dirs[j].substring((x + ".old-").length);
            var liveId = seathubToolId(x);
            if (!installer.fileExists(stage) && liveId
                    && seathubRepairRegistration({final:x, stage:stage, old:dirs[j], newId:liveId, oldId:seathubToolId(dirs[j])}))
                seathubJournalRecovered("new_live");
        }
        if (dirs[j].indexOf(x + ".stage-") === 0)
            seathubDeleteRowsAt(dirs[j], {final:x, stage:dirs[j], old:x + ".old-" + dirs[j].substring((x + ".stage-").length)});
        // Only-stage, missing-X is a fresh install. Locked stale folders are best-effort cleanup.
        seathubRemoveTree(dirs[j]);
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
        // fails cleanly at the rename step) exactly as it did before this elevated fallback
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
    var parts = String(path).split(/[\\\/]/);
    return parts[parts.length - 1];
}

function seathubParent(path)
{
    var parts = String(path).split(/[\\\/]/);
    parts.pop();
    return parts.join("\\");
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

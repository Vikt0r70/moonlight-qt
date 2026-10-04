'use strict';

// T022 / INTENT SC8 + SC9 / ADR-0077. Runs the fork's real installer/config/controlscript.qs in a VM with
// a faked Qt Installer Framework: no process, registry, installer, elevation or file is touched. Only the
// IFW operations are replaced, by a small model of Windows that knows
//   * which operations need the elevated token (the installer's admin rights are dropped by IFW before
//     installationFinished is emitted, packagemanagercore_p.cpp runInstaller),
//   * the uninstall rows of every hive and view, keyed by the ProductUUID a maintenance tool stores,
//   * a journal folder, a mutation boundary, and other processes.
// Usage: node --test tests/controlscript-registry.test.cjs
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const scriptPath = path.resolve(__dirname, '../installer/config/controlscript.qs');
const source = readFileSync(scriptPath, 'utf8');

const FINAL = 'C:\\Program Files\\SeatHub';
const NEWID = '{aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa}';
const OLDID = '{bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb}';
const UNINSTALL = 'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\';
const WOW_HKLM = 'HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\';
const JOURNAL = 'C:\\ProgramData\\SeatHubSetup\\install-journal';
const BOUNDARY = 'C:\\ProgramData\\SeatHubSetup\\mutation-boundary';
const LINK = 'C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\SeatHub.lnk';
const TOOL = 'SeatHubMaintenanceTool.exe';

const norm = (p) => String(p).replace(/\//g, '\\').replace(/\\+$/, '').toLowerCase();
const unq = (s) => s.replace(/''/g, "'");
const protectedPath = (p) => norm(p).startsWith('c:\\program files\\') || norm(p).startsWith('c:\\programdata\\');

// Builds the world, loads the script, and returns handles. `opts` select the failures to inject.
function world(opts = {}) {
    const w = {
        opts,
        elevated: opts.elevated !== undefined ? opts.elevated : true,
        prompts: 0,
        serverStarted: opts.serverStarted !== undefined ? opts.serverStarted : true,
        calls: [], moves: [], dialogs: [], logs: [],
        dirs: new Set(), files: new Map(), ids: new Map(), rows: new Map(),
        others: opts.others || [], cim: opts.cim === undefined ? 0 : opts.cim,
        deletes: [], writes: [],
    };
    w.names = new Map();
    const addDir = (d) => { w.dirs.add(norm(d)); w.names.set(norm(d), d); };
    w.addDir = addDir;
    w.install = (dir, id) => {
        addDir(dir);
        for (const f of [TOOL, 'SeatHub.exe', 'crashpad_handler.exe']) w.files.set(norm(dir + '\\' + f), '');
        w.files.set(norm(dir + '\\SeatHubMaintenanceTool.ini'), '');
        w.ids.set(norm(dir), id);
    };
    w.row = (id, location, extra = {}) => {
        const key = (extra.hive || UNINSTALL) + id;
        const uninstall = extra.uninstall || '"' + location + '\\' + TOOL + '"';
        w.rows.set(key, { id, publisher: extra.publisher || 'Seven Hills', InstallLocation: location,
            UninstallString: uninstall, ModifyPath: uninstall, DisplayIcon: location + '\\SeatHub.exe' });
        return key;
    };
    w.exists = (p) => w.dirs.has(norm(p)) || w.files.has(norm(p));
    w.move = (from, to) => {
        const a = norm(from), b = norm(to);
        for (const set of [w.dirs]) for (const d of [...set]) if (d === a || d.startsWith(a + '\\')) { set.delete(d); set.add(b + d.slice(a.length)); }
        for (const m of [w.files, w.ids]) for (const [k, v] of [...m]) if (k === a || k.startsWith(a + '\\')) { m.delete(k); m.set(b + k.slice(a.length), v); }
    };
    w.remove = (p) => {
        const a = norm(p);
        for (const d of [...w.dirs]) if (d === a || d.startsWith(a + '\\')) w.dirs.delete(d);
        for (const m of [w.files, w.ids]) for (const k of [...m.keys()]) if (k === a || k.startsWith(a + '\\')) m.delete(k);
    };
    const journalFiles = () => [...w.files.keys()].filter((k) => k.startsWith(norm(JOURNAL) + '\\'));
    w.journal = (kind) => {
        const key = journalFiles().find((k) => k.endsWith('.' + kind + '.json'));
        return key ? JSON.parse(w.files.get(key)) : null;
    };
    w.journalCount = () => journalFiles().length;
    w.rowKeys = () => [...w.rows.keys()];

    const values = { ProductVersion: '0.1.28', SeatHubFromVersion: '0.1.27', TargetDir: FINAL,
        AllUsersStartMenuProgramsPath: 'C:/ProgramData/Microsoft/Windows/Start Menu/Programs',
        InstallerFilePath: 'C:/Users/u/SeatHub-Setup-0.1.28.exe' };
    const env = { SystemRoot: 'C:\\Windows', ProgramFiles: 'C:\\Program Files', ProgramData: 'C:\\ProgramData', TEMP: 'C:\\Temp' };
    const page = { TargetDirectoryLineEdit: { text: FINAL, setText(t) { this.text = t; }, enabled: true }, MessageLabel: { setText() {} } };

    const rowText = ([key, r]) => [key, r.publisher, r.InstallLocation, r.UninstallString].join('|');

    const ps = (script) => {
        w.calls.push(['ps', script]);
        const move = script.match(/\[IO\.Directory\]::Move\('((?:[^']|'')*)','((?:[^']|'')*)'\)/);
        if (move) {
            const from = unq(move[1]), to = unq(move[2]);
            w.moves.push({ from, to, elevated: w.elevated });
            if (opts.renameExit && opts.renameExit(from, to)) return ['denied', opts.renameExit(from, to)];
            if (!w.elevated && (protectedPath(from) || protectedPath(to))) return ['', 5];
            if (!w.exists(from)) return ['', 3];
            if (w.exists(to)) return ['', 183];
            w.move(from, to);
            return ['', 0];
        }
        if (script.includes('NewGuid')) return [opts.guid || '0123456789abcdef', 0];
        if (script.includes('Get-CimInstance Win32_Process')) {
            if (opts.cimFail) return ['', 1];
            return [w.others.map((n, i) => (100 + i) + ' ' + n).join('\r\n'), 0];
        }
        if (script.includes('.seathub-write-')) return ['', opts.parentWritable === false ? 5 : 0];
        if (script.includes('ProductUUID')) {
            const ini = unq(script.match(/ReadAllText\('((?:[^']|'')*)'\)/)[1]);
            const id = w.ids.get(norm(path.win32.dirname(ini)));
            return id && !opts.iniUnreadable ? [id + '\r\n', 0] : ['', 7];
        }
        if (script.includes('$o=@();$bad=0')) {
            if (opts.queryFail && opts.queryFail()) return ['', 9];
            return [[...w.rows].map(rowText).join('\r\n'), 0];
        }
        if (script.includes("Get-ItemProperty -LiteralPath 'Registry::")) {
            const key = unq(script.match(/'Registry::((?:[^']|'')*)'/)[1]);
            const row = w.rows.get(key);
            if (!row) return ['', 1];
            const lines = [...script.matchAll(/'(\w+)='\+\$k\.\w+/g)].map((m) => m[1] + '=' + (opts.readbackWrong === m[1] ? 'stale' : row[m[1]]));
            return [lines.join('\r\n'), 0];
        }
        if (script.includes('$dirs.FullName')) {
            const x = unq(script.match(/\$x='((?:[^']|'')*)'/)[1]);
            const found = [...w.dirs].filter((d) => d.startsWith(norm(x) + '.stage-') || d.startsWith(norm(x) + '.old-')).map((d) => w.names.get(d));
            return [found.join('\r\n'), 0];
        }
        if (script.includes('$a.GetOwner')) return ['trusted', 0];
        if (script.includes('Test-Path -LiteralPath $p')) return ['directory', 0];
        if (script.includes('SetAccessControl') || script.includes('CreateDirectory(')) { w.aclRepaired = true; return ['', opts.aclRepairFails ? 5 : 0]; }
        if (script.includes('$files=Get-ChildItem')) return ['', opts.pruneFails ? 5 : 0];
        if (script.includes('WScript.Shell')) {
            if (!w.elevated) return ['', 5];
            w.files.set(norm(LINK), 'lnk');
            return ['', 0];
        }
        if (script.includes('[IO.File]::Open(')) {
            const file = unq(script.match(/Open\('((?:[^']|'')*)'/)[1]);
            if (!w.elevated && protectedPath(file)) return ['', 5];
            w.files.set(norm(file), unq(script.match(/GetBytes\('((?:[^']|'')*)'\)/)[1]));
            w.psFallbackWrite = true;
            return ['', 0];
        }
        if (script.includes('@(Get-ChildItem -LiteralPath') && script.includes('.Count')) return ['5', 0];
        throw new Error('Unhandled IFW execute seam: ' + script);
    };

    const installer = {
        value: (k) => values[k] || '',
        setValue: (k, v) => { values[k] = v; },
        status: 0,
        isInstaller: () => true,
        toNativeSeparators: (p) => String(p).replace(/\//g, '\\'),
        interrupt: () => {}, setCanceled: () => {},
        hasAdminRights: () => w.elevated,
        gainAdminRights: () => {
            w.calls.push(['gain']);
            if (opts.declineElevation) return false;
            if (!w.elevated && !w.serverStarted) { w.prompts += 1; w.serverStarted = true; }
            w.elevated = true;
            return true;
        },
        environmentVariable: (k) => env[k] || '',
        fileExists: (p) => w.exists(p),
        isProcessRunning: () => false, killProcess: () => true,
        performOperation: (op, args) => {
            w.calls.push([op, args]);
            if (op === 'AppendFile') {
                if (opts.appendRefused && norm(args[0]).startsWith(norm(JOURNAL))) return false;
                if (!w.elevated && protectedPath(args[0])) return false;
                w.files.set(norm(args[0]), args[1]);
                return true;
            }
            if (op === 'Delete') { w.files.delete(norm(args[0])); return true; }
            if (op === 'CreateShortcut') return false; // COM runs in the unelevated installer process
            return true;
        },
        execute: (tool, args) => {
            const t = norm(tool);
            w.calls.push([t, args]);
            if (t.endsWith('powershell.exe')) return ps(args[args.length - 1]);
            if (t.endsWith('reg.exe')) {
                if (!w.elevated) return ['access denied', 5];
                if (args[0] === 'delete') {
                    w.deletes.push(args[1]);
                    if (opts.deleteExit) return ['', opts.deleteExit];
                    if (!opts.deleteIsLie) w.rows.delete(args[1]);
                    return ['', 0];
                }
                w.writes.push(args[3]);
                if (opts.failField === args[3]) return ['access denied', 5];
                w.rows.get(args[1])[args[3]] = args[7];
                return ['', 0];
            }
            if (t.endsWith('cmd.exe')) {
                const target = args[args.length - 1];
                if (args[1] === 'mkdir') {
                    if (!w.elevated || opts.mkdirDenied) return ['', 5];
                    if (w.exists(target)) return ['', 1];
                    w.addDir(target); return ['', 0];
                }
                if (args[1] === 'rmdir') {
                    if (!w.elevated && protectedPath(target)) return ['', 5];
                    w.rmdirs = (w.rmdirs || []).concat(norm(target));
                    w.remove(target); return ['', 0];
                }
            }
            return ['', 0]; // ping, icacls, taskkill
        },
    };
    const gui = { currentPageWidget: () => page, reject: () => { w.rejected = true; }, setSilent() {}, rejectWithoutPrompt() {} };
    const context = { installer, gui, QInstaller: { Success: 0, Failure: 1, Canceled: 2 },
        QMessageBox: { critical: (id, title, text) => { w.dialogs.push(text); return 1; } },
        console: { log: (m) => w.logs.push(String(m)) } };
    vm.runInNewContext(source, context, { filename: scriptPath, timeout: 5000 });
    w.context = context;
    w.controller = Object.create(context.Controller.prototype);
    w.page = page;
    w.values = values;
    w.attempt = () => vm.runInContext('seathubAttempt', context);
    return w;
}

// A machine with the live 0.1.27 install registered under OLDID, as IFW left it.
function liveMachine(opts = {}) {
    const w = world(opts);
    w.install(FINAL, OLDID);
    w.row(OLDID, FINAL);
    if (!opts.noLink) w.files.set(norm(LINK), 'lnk');
    w.dirs.add(norm('C:\\ProgramData\\SeatHubSetup'));
    w.addDir(JOURNAL);
    return w;
}

// What IFW does between the Target page and installationFinished: extract the stage and register its row
// under the new ProductUUID, then (unless told otherwise) drop the internal elevation.
function stageAndFinish(w, { drop = true } = {}) {
    const a = w.attempt();
    assert.ok(a, 'the target page callback must have created an attempt');
    w.install(a.stage, NEWID);
    w.row(NEWID, a.stage);
    w.controller.seathubStarted();
    if (drop) w.elevated = false;
    w.controller.seathubFinished();
    return a;
}

function update(opts = {}) {
    const w = liveMachine(opts);
    w.controller.TargetDirectoryPageCallback();
    return w;
}

const finalRows = (w) => w.rowKeys().map((k) => k.slice(k.lastIndexOf('\\') + 1));

test('IFW drops its elevation before installationFinished: the swap-aside rename is re-elevated and succeeds', () => {
    const w = update({ serverStarted: true });
    stageAndFinish(w, { drop: true });
    const end = w.journal('end');
    assert.equal(end.outcome, 'success');
    assert.equal(end.code, 0);
    assert.equal(end.elevated, true, 'the end record is written with the same elevation as the swap');
    assert.ok(w.moves.length >= 2 && w.moves.every((m) => m.elevated), 'every rename ran elevated');
    assert.equal(w.prompts, 0, 'regaining reuses the started elevated server: no second UAC prompt');
    assert.equal(w.journal('start'), null, 'the start record is removed once the end record exists');
    assert.deepEqual(finalRows(w), [NEWID], 'exactly one SeatHub uninstaller remains');
    assert.ok(w.exists(LINK), 'the Start Menu shortcut is kept');
});

test('without the re-elevation the unelevated handler loses the rename AND the end record (the T005 symptom)', () => {
    const w = update();
    w.context.seathubRegainElevation = () => true; // reproduce the pre-repair behaviour
    const a = stageAndFinish(w, { drop: true });
    assert.equal(w.journal('end'), null, 'the journal folder is read-only for the unelevated token');
    assert.ok(w.logs.some((l) => l.includes('journal end unavailable')));
    assert.ok(w.moves.every((m) => !m.elevated));
    assert.ok(w.exists(FINAL + '\\SeatHub.exe'), 'the old app stays launchable');
    assert.ok(a.code === 5, 'the real number is held in the attempt');
});

test('an access-denied rename is access_denied with code 5 in the journal, after three tries', () => {
    const w = update({ renameExit: (from) => (norm(from) === norm(FINAL) ? 5 : 0) });
    stageAndFinish(w, { drop: false });
    const end = w.journal('end');
    assert.equal(end.outcome, 'failed');
    assert.equal(end.step, 'swap_aside');
    assert.equal(end.code, 5, 'ERROR_ACCESS_DENIED survives into the journal');
    assert.equal(end.class, 'installer.access_denied', 'access denied is not mislabelled locked_file');
    assert.equal(end.state, 'old_intact');
    assert.equal(w.moves.filter((m) => norm(m.from) === norm(FINAL)).length, 3, 'V36: three tries');
    assert.ok(w.exists(FINAL + '\\SeatHub.exe'), 'the old app stays launchable');
});

test('a sharing violation is locked_file with its own number; an unclassified number stays unknown', () => {
    const shared = update({ renameExit: (from) => (norm(from) === norm(FINAL) ? 32 : 0) });
    stageAndFinish(shared, { drop: false });
    assert.deepEqual([shared.journal('end').code, shared.journal('end').class], [32, 'installer.locked_file']);
    const odd = update({ renameExit: (from) => (norm(from) === norm(FINAL) ? 183 : 0) });
    stageAndFinish(odd, { drop: false });
    assert.deepEqual([odd.journal('end').code, odd.journal('end').class], [183, 'installer.unknown']);
});

test('the first failing operation keeps its number through rollback', () => {
    // swap_in fails with 5, then the rollback rename fails with 183: the journal keeps 5.
    const w = update({ renameExit: (from, to) => (norm(from).includes('.stage-') && norm(to) === norm(FINAL) ? 5
        : norm(from).includes('.old-') ? 183 : 0) });
    stageAndFinish(w, { drop: false });
    const end = w.journal('end');
    assert.equal(end.step, 'swap_in');
    assert.equal(end.code, 5);
    assert.equal(end.rollback, 'failed');
    assert.equal(end.class, 'installer.rollback_failed');
});

test('a failed old-row deletion cannot report success and still keeps one usable uninstaller', () => {
    const w = update({ deleteExit: 5 });
    stageAndFinish(w, { drop: false });
    const end = w.journal('end');
    assert.notEqual(end.outcome, 'success');
    assert.equal(end.step, 'registry');
    assert.equal(end.code, 5);
    assert.equal(end.state, 'new_live');
    assert.equal(w.rows.size, 2, 'the rejected delete leaves the old row, reported honestly');
    assert.equal(w.rows.get(UNINSTALL + NEWID).UninstallString, '"' + FINAL + '\\' + TOOL + '"', 'the live row is usable');
});

test('a delete that was reported but did not stick is read back as a failure', () => {
    const w = update({ deleteIsLie: true });
    stageAndFinish(w, { drop: false });
    assert.equal(w.journal('end').outcome, 'failed');
    assert.equal(w.journal('end').step, 'registry');
});

test('a failed registry query is unknown: nothing is written or deleted, and it is not reported as success', () => {
    const w = update({ queryFail: () => true });
    stageAndFinish(w, { drop: false });
    assert.deepEqual(w.writes, []);
    assert.deepEqual(w.deletes, []);
    assert.equal(w.journal('end').outcome, 'failed');
    assert.equal(w.journal('end').step, 'registry');
});

test('every registry write is read back: a stale readback keeps the old row and fails the registry step', () => {
    const w = update({ readbackWrong: 'UninstallString' });
    stageAndFinish(w, { drop: false });
    assert.deepEqual(w.deletes, [], 'nothing is removed before the new row is proven usable');
    assert.equal(w.journal('end').outcome, 'failed');
    assert.equal(w.rows.size, 2);
});

test('restart after a partial patch finds the live row by ProductUUID and finishes it (original harness failure 2)', () => {
    const w = liveMachine({ failField: 'UninstallString' });
    const stage = FINAL + '.stage-0123456789abcdef', old = FINAL + '.old-0123456789abcdef';
    // killed after the swap: new tree is live, old tree set aside, stage gone, row half patched.
    w.remove(FINAL); w.install(FINAL, NEWID); w.install(old, OLDID);
    w.rows.delete(UNINSTALL + OLDID); w.row(OLDID, FINAL);
    w.row(NEWID, stage);
    w.context.seathubAttempt = null;
    assert.equal(w.context.seathubRepairRegistration({ final: FINAL, stage, old, newId: NEWID, oldId: OLDID }), false);
    assert.equal(w.rows.get(UNINSTALL + NEWID).InstallLocation, FINAL, 'the first field was patched');
    assert.equal(w.rows.get(UNINSTALL + NEWID).UninstallString, '"' + stage + '\\' + TOOL + '"', 'the second was not');
    // next run, nothing injected: recovery must not need the half patched InstallLocation.
    w.opts.failField = null;
    w.context.seathubRecoverInterruptedSwap(FINAL);
    const row = w.rows.get(UNINSTALL + NEWID);
    assert.equal(row.UninstallString, '"' + FINAL + '\\' + TOOL + '"');
    assert.equal(row.ModifyPath, '"' + FINAL + '\\' + TOOL + '"');
    assert.equal(row.DisplayIcon, FINAL + '\\SeatHub.exe');
    assert.deepEqual(finalRows(w), [NEWID]);
    assert.equal(w.journal('end').outcome, 'recovered');
});

test('only proven-dead owned rows go: lookalikes, other locations, other publishers and leaked test rows survive', () => {
    const w = liveMachine();
    const lookalike = w.row('{cccccccc-3333-4333-8333-cccccccccccc}', FINAL, { publisher: 'Someone Else' });
    const scratch = w.row('{dddddddd-4444-4444-8444-dddddddddddd}', 'C:\\Users\\u\\scratch\\SeatHub');
    const scratchOwnedLooking = w.row('{eeeeeeee-5555-4555-8555-eeeeeeeeeeee}', FINAL, { uninstall: '"C:\\Elsewhere\\' + TOOL + '"' });
    const legacyWow = w.row('{ffffffff-6666-4666-8666-ffffffffffff}', FINAL, { hive: WOW_HKLM });
    w.controller.TargetDirectoryPageCallback();
    stageAndFinish(w, { drop: true });
    assert.equal(w.journal('end').outcome, 'success');
    const left = w.rowKeys();
    assert.ok(left.includes(UNINSTALL + NEWID));
    for (const kept of [lookalike, scratch, scratchOwnedLooking]) assert.ok(left.includes(kept), 'untouched: ' + kept);
    assert.ok(!left.includes(UNINSTALL + OLDID), 'the replaced row is removed');
    assert.ok(!left.includes(legacyWow), 'an owned dead row in another view is removed by its exact key');
    assert.deepEqual(w.deletes.filter((k) => k === lookalike || k === scratch || k === scratchOwnedLooking), []);
});

test('a legacy stale row of this install (not the live ProductUUID) is removed, leaving exactly one uninstaller', () => {
    const w = liveMachine();
    w.row('{12121212-7777-4777-8777-121212121212}', FINAL); // the 0.1.0 row the earlier update left behind
    w.controller.TargetDirectoryPageCallback();
    stageAndFinish(w, { drop: true });
    assert.deepEqual(finalRows(w), [NEWID]);
});

test('the usable uninstaller is verified before anything is removed', () => {
    const w = update();
    const a = w.attempt();
    w.install(a.stage, NEWID);
    w.row(NEWID, a.stage);
    w.controller.seathubStarted();
    w.context.seathubRename = (from, to) => { w.move(from, to); if (norm(to) === norm(FINAL)) w.files.delete(norm(FINAL + '\\' + TOOL)); return true; };
    w.controller.seathubFinished();
    assert.deepEqual(w.deletes, [], 'no tool at the final folder, so no row is deleted');
    assert.equal(w.journal('end').outcome, 'failed');
});

test('an unreadable ProductUUID is unknown ownership: no row is guessed or removed', () => {
    const w = update({ iniUnreadable: true });
    stageAndFinish(w, { drop: false });
    assert.deepEqual(w.deletes, []);
    assert.equal(w.journal('end').outcome, 'failed');
    assert.equal(w.journal('end').step, 'registry');
});

test('the Start Menu shortcut lost by IFW rollback is recreated elevated and read back', () => {
    const w = update({ renameExit: (from) => (norm(from) === norm(FINAL) ? 5 : 0), noLink: true });
    w.files.delete(norm(LINK));
    stageAndFinish(w, { drop: true });
    assert.ok(w.exists(LINK), 'the shortcut exists after the failed swap');
    assert.equal(w.journal('end').outcome, 'failed');
});

test('the stage folder and its row are removed on a failed swap, and the old row stays', () => {
    const w = update({ renameExit: (from) => (norm(from) === norm(FINAL) ? 5 : 0) });
    const a = stageAndFinish(w, { drop: true });
    assert.ok(!w.exists(a.stage), 'no stage residue');
    assert.deepEqual(finalRows(w), [OLDID]);
    assert.equal(w.journal('end').code, 5);
});

test('the end record is written even when the prune step fails, and a refused AppendFile falls back', () => {
    const w = update({ pruneFails: true, appendRefused: true });
    stageAndFinish(w, { drop: true });
    assert.equal(w.journal('end').outcome, 'success');
    assert.ok(w.psFallbackWrite, 'the same elevated process path wrote the record');
    assert.ok(!w.logs.some((l) => l.includes('journal end unavailable')));
});

test('an untrusted journal folder is repaired in place and never deleted', () => {
    const w = liveMachine();
    w.files.set(norm(JOURNAL + '\\attempt-aaaaaaaaaaaaaaaa.end.json'), '{}');
    w.context.seathubJournalOwnerTrusted = () => false;
    w.controller.TargetDirectoryPageCallback();
    assert.ok(w.aclRepaired, 'ACL repaired in place');
    assert.ok(!(w.rmdirs || []).includes(norm(JOURNAL)) && !(w.rmdirs || []).includes(norm('C:\\ProgramData\\SeatHubSetup')),
        'neither the journal nor its parent is removed');
    assert.ok(w.files.has(norm(JOURNAL + '\\attempt-aaaaaaaaaaaaaaaa.end.json')), 'existing records survive');
});

test('declined elevation changes nothing: the old app stays and the wizard closes with the failure text', () => {
    const w = liveMachine({ elevated: false, serverStarted: false, declineElevation: true });
    w.controller.TargetDirectoryPageCallback();
    assert.equal(w.moves.length, 0);
    assert.deepEqual(w.rowKeys(), [UNINSTALL + OLDID]);
    assert.ok(w.exists(FINAL + '\\SeatHub.exe'));
    assert.equal(w.prompts, 0);
    assert.equal(w.dialogs.length, 1);
    assert.ok(w.rejected, 'the wizard is closed');
    assert.ok(!w.exists(BOUNDARY), 'no boundary was taken');
});

// ----------------------------------------------------------------------------------- ADR-0077 item 7

test('quiescence: another live setup blocks the mutation; nothing is renamed, deleted or killed', () => {
    const w = liveMachine({ others: ['SeatHub-Setup-0.1.27.exe'] });
    w.addDir(FINAL + '.old-feedfeedfeedfeed'); // would be recovered or removed by a mutating run
    w.controller.TargetDirectoryPageCallback();
    assert.equal(w.moves.length, 0);
    assert.deepEqual(w.rmdirs || [], []);
    assert.ok(w.dirs.has(norm(FINAL + '.old-feedfeedfeedfeed')), 'blocked, not abandoned');
    assert.equal(w.journal('end').step, 'preflight');
    assert.equal(w.journal('end').class, 'installer.locked_file');
    assert.equal(w.journal('end').outcome, 'cancelled', 'a block is not a failure: the client spends no attempt on it');
    assert.equal(w.journal('end').state, 'old_intact');
    assert.ok(!w.exists(BOUNDARY));
});

test('quiescence: an unreadable process list is unknown, which is blocked', () => {
    const w = liveMachine({ cimFail: true });
    w.controller.TargetDirectoryPageCallback();
    assert.equal(w.moves.length, 0);
    assert.equal(w.journal('end').step, 'preflight');
    assert.equal(w.journal('end').outcome, 'cancelled');
});

test('quiescence: the mutation boundary is exclusive, held across the update and released at the end', () => {
    const w = update();
    assert.ok(w.exists(BOUNDARY), 'acquired before any mutation');
    stageAndFinish(w, { drop: true });
    assert.ok(!w.exists(BOUNDARY), 'released after the end record');
});

test('quiescence: a stale boundary is reclaimed only after no live setup is proven, never by age', () => {
    const w = liveMachine();
    w.addDir(BOUNDARY);
    w.controller.TargetDirectoryPageCallback();
    assert.ok(w.exists(BOUNDARY), 'reclaimed and held by this setup');
    assert.equal(w.journal('end'), null, 'the update proceeded');
    const live = liveMachine({ others: ['SeatHubMaintenanceTool.exe'] });
    live.addDir(BOUNDARY);
    live.controller.TargetDirectoryPageCallback();
    assert.ok(live.exists(BOUNDARY), 'a boundary with a live holder is left alone');
    assert.deepEqual(live.rmdirs || [], []);
    assert.equal(live.journal('end').step, 'preflight');
    assert.equal(live.journal('end').outcome, 'cancelled');
});

test('quiescence: an interrupted swap is recovered only after elevation and the boundary', () => {
    const w = liveMachine({ elevated: false, serverStarted: false });
    w.remove(FINAL); w.install(FINAL + '.old-0123456789abcdef', OLDID);
    w.controller.TargetDirectoryPageCallback();
    const order = w.calls.map((c) => (c[0] === 'gain' ? 'gain' : c[0].endsWith('cmd.exe') && c[1][1] === 'mkdir' ? 'boundary'
        : c[0] === 'ps' && c[1].includes('[IO.Directory]::Move') ? 'move' : null)).filter(Boolean);
    assert.deepEqual(order.slice(0, 3), ['gain', 'boundary', 'move']);
    assert.ok(w.exists(FINAL + '\\SeatHub.exe'), 'the old app is back');
});

test('no duration is encoded anywhere in the quiescence rule (V38 is open) and the 1800 s journal rule is not reused', () => {
    assert.ok(!/\b1800\b/.test(source), 'no 1800');
    const w = world();
    for (const fn of ['seathubOtherSetupState', 'seathubAcquireBoundary', 'seathubReleaseBoundary']) {
        const text = w.context[fn].toString();
        assert.ok(!/Date|clock|Math|setTimeout|setInterval|seathubSleepOneSecond|Ms\b|Seconds|timeout/i.test(text), fn + ' has no time quantity');
        assert.ok(!/\b[3-9]\d*\b/.test(text.replace(/"[^"]*"|'[^']*'/g, '')), fn + ' has no numeric bound beyond its two passes');
    }
    assert.ok(!/SEATHUB_QUIESCENCE|PRE_JOURNAL/i.test(source), 'no quiescence constant');
});

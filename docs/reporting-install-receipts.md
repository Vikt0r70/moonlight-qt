# Install-time reporting choice: route proof and receipts (T021, ADR-0107)

Evidence for the setup-window route and the sixteen receipts the task names. Every receipt line says
where its evidence came from, because the sources differ in strength:

- **box** = the packaged RentaPC candidate (pack ID `RentaPC`, built by `scripts/pack-seathub.ps1`,
  version 0.1.30, plus a second pack of the same files as 0.1.31 for the update) run inside the
  Sandboxie box `SeatHubCustomer`, installed into a private folder (`C:\RentaPCBoxTest\...`). The box
  hides the host's `HKCU\Software\Seven Hills`, so the owner's install and registry were not touched.
  The window was driven with a scratch Win32 program that reads the controls and clicks them.
- **suite** = a Qt test in this repo (`tests/tst_reporting_consent.cpp`, `tst_log_shipper.cpp`,
  `tst_facade_wiring.cpp`, `tst_ui_screens.cpp`), run through `tests\run-all-suites.cmd`.
- **probe** = a throwaway Velopack package (`RouteProof`, not RentaPC) used only to see which Velopack
  hooks fire and what the installed app reads.
- **source** = read from the code, not run.

Nothing here claims code signing or SignPath approval. The candidate is unsigned.

## Route proof

BOOTSTRAPPER-ROUTE: proven setup window (`installer/bootstrapper`, plain Win32, no Qt runtime, no new dependency) runs first, stores the choice outside the versioned app folder, then runs the unchanged Velopack Setup per-user with no administrator; the installed app reads the choice (probe: `--veloapp-install` hook run read `recorded=1 send=1 ver=1`; box: installed app launched with Choice=2 and did not ask)

How the route works and what was proven:

1. `RentaPC-Setup.exe` (the outer program) carries the notice text (resource 101, from
   `installer/PRIVACY-NOTICE.txt`) and the raw Velopack Setup (resource 102).
2. With no choice recorded and no silent switch it shows the window. Install writes
   `HKCU\Software\Seven Hills\SeatHub\Reporting` (`Choice`, `NoticeVersion`) and then runs the inner
   Setup with the customer's own arguments. Cancel or close returns 1223 and does nothing else.
3. Where the choice lives: `HKCU\Software\Seven Hills\SeatHub\Reporting`. It is outside the versioned
   app folder (`%LOCALAPPDATA%\RentaPC\current` is replaced on each update) and no Velopack hook
   touches it on install, reinstall or update. Only `--veloapp-uninstall` reaches
   `VelopackUpdater::beforeUninstall`, which deletes the whole `Software\Seven Hills\SeatHub` tree, so
   an uninstall removes the choice with the app (ADR-0107: uninstall removes it).
4. Probe result (the point attempt 1 left unproven): a Setup run over an existing install, silent, with
   `Choice=2` recorded, fires only `--veloapp-install` in the installed app. No `--veloapp-uninstall`
   ran and `Choice` stayed 2. The same held for a 0.1.1 update package over a 1.0.0 install.
5. No reporting step runs inside a Velopack hook. `velopack_updater.cpp` is unchanged by this task.

Observations a reviewer should know:

- An interactive Setup over an existing install, run in the box, did not finish within 150 s. This
  happened for the raw, unchanged Velopack Setup on its own as well, so it is not caused by the
  setup window. The silent update and reinstall (the update path the app uses) finished with exit 0.
- The default install folder `%LOCALAPPDATA%\RentaPC` exists on the owner's PC, so every box run used
  `--installto` a private folder inside the box.

## Receipts

RECEIPT-NOTICE-IN-BOOTSTRAPPER: pass box: the window opened with title "RentaPC setup / <Arabic>", a read-only notice of 3952 characters that starts with the ADR notice sentence and names Sentry, and the pack carries `installer/PRIVACY-NOTICE.txt` as resource 101 (Verify also checks `bootstrapper.rc` names it)

RECEIPT-INTERACTIVE-CHOICE: pass box: left ticked then Install recorded `Choice=1 NoticeVersion=1`; unticked then Install recorded `Choice=2 NoticeVersion=1` (both read back with `reg query`)

RECEIPT-DEFAULT-TICKED: pass box: `CHECK_STATE_INITIAL=1` for the label "send crash reports, logs and session evidence" on every first open, and the in-app ask for a user with no recorded choice also opened ticked

RECEIPT-RED-WARNING: pass box: both warning lines hidden at open; unticking made them visible with the text "We can't give proper support for errors with this turned off."; re-ticking hid them again. Colour is source: `WM_CTLCOLORSTATIC` uses RGB(0xEF,0x44,0x44), which is the `--destructive` token `#EF4444` in `docs/spec/ui.md`

RECEIPT-NOTICE-LINK: pass box + source: the window shows the link "What we collect / <Arabic>" (id 105). Its target is the constant `kUrl` = `https://rentapc.online/data-collection` in `reporting_consent_store.h`, opened with `ShellExecute`; the click was not executed (no network use in this task), and `PRIVACY-NOTICE.txt` also ends with the same URL

RECEIPT-CANCEL-INSTALLS-NOTHING: pass box: Cancel returned exit 1223; afterwards `HKCU\Software\Seven Hills\SeatHub\Reporting` did not exist and `C:\RentaPCBoxTest\app` was not created. Untick, re-tick, then Cancel also returned 1223 with nothing recorded. Closing the window, Escape and Cancel give the same `answered=false` result in `reporting_consent_store.h` (source; only Cancel was driven)

RECEIPT-SILENT-DEFAULT: pass box: with the key cleared, `RentaPC-Setup.exe --silent --installto ...` exited 0 and recorded `Choice=1 NoticeVersion=1`, with no window

RECEIPT-SILENT-REINSTALL-PRESERVES: pass box: with `Choice=2` recorded, a silent reinstall of the same 0.1.30 pack over the install exited 0 and left `Choice=2 NoticeVersion=1`; the reinstalled app then launched without asking. Probe evidence for the hooks is in the route proof. Covered upload suppression after reinstall is the suite evidence under UNTICKED-STOPS-UPLOADS (`aNewProcessOverTheKeptValueKeepsOff`)

RECEIPT-INNER-SETUP-UNCHANGED: pass box: the inner Setup embedded in the candidate outer `RentaPC-Setup.exe` has sha256 `f09acf3ce76629c1a2a2e263f93b676753acb87e0859530fc06a0dab820c92af` (57,859,998 bytes), the raw `RentaPC-win-Setup.exe` that vpk produced and `pack-seathub.ps1` moved to `build\bootstrapper-stage\inner-setup.bin`; a byte search found those exact bytes inside the outer exe (sha256 `939bf25162530156d91a9972e805ae72519f177a4c506ff732c32957bd51303b`) at offset 50136. The same byte-for-byte match was shown for the probe package. The release folder holds no inner Setup

RECEIPT-NO-CHOICE-NO-UPLOAD: pass suite + box: `noRecordedChoiceSendsNothingCoveredAndAsks` (no recorded choice: `coveredUploadsAllowed()` is false and the app asks) and `sentryIsNotInitialisedWithoutARecordedSend`; in the box the installed app started with the key cleared opened the ask window before anything else and recorded only after OK. Request counts come from the suites, not from the box (the box has no sign-in or Sentry DSN)

RECEIPT-UNTICKED-STOPS-UPLOADS: pass suite: `aRecordedDoNotSendBlocksCoveredUploads`, `linesStaySpooledWhileReportingIsOffAndShipWhenItIsSwitchedOn` (tst_log_shipper), `aCapsuleStaysPendingWhileReportingIsOffAndIsSentWhenItIsSwitchedOn` and `theQualityOutboxSendsNothingWhileReportingIsOffAndKeepsItsFiles` (tst_facade_wiring): zero covered requests when off, an item queued while on and then switched off is never sent, queued files stay on disk, and functional calls are unaffected. Box: an unticked install recorded `Choice=2` and the installed app started normally without asking

RECEIPT-UPDATE-PRESERVES: pass box: with `Choice=2`, a silent 0.1.31 Setup over the 0.1.30 install exited 0 and left `Choice=2 NoticeVersion=1`; `packages` changed from `RentaPC-0.1.30-full.nupkg` to `RentaPC-0.1.31-full.nupkg`; the updated app launched without asking and did not flip the choice. An update never re-enables an unticked choice

RECEIPT-UPDATE-IDENTITY: pass box + source: the 0.1.31 update replaced the 0.1.30 install in place (Uninstall key `RentaPC` `DisplayVersion` 0.1.31, same `InstallLocation`), and `releases.win.json` of the real pack lists `PackageId` `RentaPC`. `pack-seathub.ps1` still passes pack ID `RentaPC` and adds no channel or feed option; only the outer `RentaPC-Setup.exe` is published

RECEIPT-NOTICE-VERSION-REASK: pass box + suite: with `Choice=2 NoticeVersion=0` the installed app opened the ask window at start with the previous choice pre-selected (box unticked, warning visible) and the old choice stayed in force until OK; OK recorded `Choice=2 NoticeVersion=1`. Suite: `aHigherNoticeVersionAsksAgainWithThePreviousChoiceInForce`, `theSameNoticeVersionNeverAsks`, `aNoticeVersionAboveThePackageNeverAsks`. The "What changed" list is empty today: notice version 1 has no earlier text to compare, so the app passes no list yet

RECEIPT-LATER-CHANGE: pass suite: `theSettingsPageHasOneReportingRowThatShowsTheRecordedChoice` and `changingTheReportingRowRecordsTheChoiceAndStatesWhenItTakesEffect` (tst_ui_screens): the one Settings row shows the recorded choice and a change records it at the current notice version; `onlyAnExplicitChoiceChangesTheSwitch` (tst_reporting_consent) shows a server payload cannot enable it

RECEIPT-EFFECTIVE-TIME: pass suite + source: the Settings row states "Logs, stream quality numbers and session evidence stop or resume at once. Crash reporting stops or starts at the next launch of RentaPC." (checked in tst_ui_screens). Per-send senders re-read the switch on each send (tests above). Sentry is started only at launch from the stored choice: `sentryIsNotInitialisedWithoutARecordedSend` and `aNewProcessOverTheKeptValueKeepsOff` use the fake Sentry seam across a simulated relaunch. Limit: counting `crashpad_handler.exe` in the box did not separate on from off (0 processes in both, since the box app has no sign-in and no DSN), so the box gives no Crashpad evidence either way

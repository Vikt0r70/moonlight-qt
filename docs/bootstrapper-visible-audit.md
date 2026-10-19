# RentaPC setup file: every user-visible string (T083 audit)

The customer downloads one file, `RentaPC-Setup.exe`. It is a small Win32 program
(`installer/bootstrapper/`, ADR-0107) that asks the reporting question and then starts the unchanged
Velopack Setup it carries inside. This page lists every string a person can see from that outer file,
where it comes from and which approved text it must equal.

**Why this exists.** Windows shows the *File description* of a downloaded program in its
properties dialog and in the SmartScreen / "Open file - Security Warning" prompt. The file said
`RentaPC setup` while ADR-0092 inventory row 53 (`customer.setup.description`) approves `RentaPC`.
The repair changed only that one resource line; the visible window title stays `RentaPC setup`
(ADR-0107, `docs/spec/copy.md` Setup window section).

## Authorities

| Short name | Where | What it fixes |
|---|---|---|
| A-0092 | `docs/decisions/ADR-0092-visible-inventory.tsv` row 53, `ADR-0092-rentapc-visible-names-and-domains.md` (primary checkout) | The five product-name metadata fields, including `customer.setup.description` = `RentaPC` |
| A-copy | `docs/spec/copy.md` section "Setup window (Velopack, 2026-10-08)" (primary checkout) | Window title, notice, box label, warning, link, Install / Cancel, EN and AR |
| A-0107 | `docs/decisions/ADR-0107-client-install-time-reporting-choice.md` (primary checkout) | What the window asks, the notice provenance (`installer/PRIVACY-NOTICE.txt` paragraphs 1 and 2), the silent default |
| A-T021 | `installer/bootstrapper/main.cpp` as built by T021 | The three error dialogs. `copy.md` has no catalog entry for them, so the source itself is the only authority; nothing is invented here |

## What the test proves, and what it does not

`tests/tst_reporting_consent.cpp`, test `bootstrapperVisibleStringsMatchApprovedInventoryAndCopy`,
reads the production sources as text and fails when:

- this page is missing, has a malformed row or a duplicate row id;
- any version-resource line of `bootstrapper.rc` is not in the matrix, or a matrix `line` row is not in the file;
- any string literal in `main.cpp` or `reporting_consent_store.h` is not in the matrix (a new or edited literal fails), or a matrix literal is not in its source;
- the set of `MessageBoxW` title/body pairs in `main.cpp` differs from the `messagebox` rows;
- the approved English copy (pinned inside the test, not only here) or the notice differs from `PRIVACY-NOTICE.txt`;
- `app/version.txt` and `SEATHUB_VERSION` disagree.

It does **not** read the built PE or the installed window. A source test is not a readback of the
published file; use the procedure at the end of this page for that (task T030). The Arabic strings are
checked matrix-against-source only, the test file itself stays ASCII.

## Matrix

Row kinds: `line` = a trimmed line of the resource script equals the expected text; `text` = the source
contains the expected text; `literal` = visible copy, the expected text is the C++ spelling between the
quotes with adjacent literals merged; `classified` = a literal that is not copy (class in the Key
column); `messagebox` = one `MessageBoxW` call (Key = title, Expected = body); `vendor` = text drawn by
Velopack's inner Setup or Windows, not by this program and not RentaPC copy.

### Version resource (`installer/bootstrapper/bootstrapper.rc`)

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B01 | line | installer/bootstrapper/bootstrapper.rc | resource 101 | `101 RCDATA "../PRIVACY-NOTICE.txt"` | A-0107: the notice the window shows |
| B02 | line | installer/bootstrapper/bootstrapper.rc | resource 102 | `102 RCDATA "inner-setup.bin"` | A-0107: unchanged Velopack Setup, not drawn by this program |
| B03 | line | installer/bootstrapper/bootstrapper.rc | FileVersion number | `FILEVERSION RENTAPC_VER_NUMBERS` | Packer rule B40: maj,min,patch,0 |
| B04 | line | installer/bootstrapper/bootstrapper.rc | ProductVersion number | `PRODUCTVERSION RENTAPC_VER_NUMBERS` | Packer rule B40: maj,min,patch,0 |
| B05 | line | installer/bootstrapper/bootstrapper.rc | string table | `BLOCK "StringFileInfo"` | Windows version-resource layout |
| B06 | line | installer/bootstrapper/bootstrapper.rc | language block | `BLOCK "040904b0"` | English (US), Unicode code page 1200; matches B15 |
| B07 | line | installer/bootstrapper/bootstrapper.rc | CompanyName | `VALUE "CompanyName", "RentaPC"` | A-0092 metadata |
| B08 | line | installer/bootstrapper/bootstrapper.rc | FileDescription | `VALUE "FileDescription", "RentaPC"` | A-0092 inventory row 53 `customer.setup.description` |
| B09 | line | installer/bootstrapper/bootstrapper.rc | FileVersion | `VALUE "FileVersion", RENTAPC_VER_TEXT` | Packer rule B41: the release version text |
| B10 | line | installer/bootstrapper/bootstrapper.rc | InternalName | `VALUE "InternalName", "RentaPC-Setup"` | A-0092 metadata (file name stem) |
| B11 | line | installer/bootstrapper/bootstrapper.rc | OriginalFilename | `VALUE "OriginalFilename", "RentaPC-Setup.exe"` | A-0092 metadata (file name) |
| B12 | line | installer/bootstrapper/bootstrapper.rc | ProductName | `VALUE "ProductName", "RentaPC"` | A-0092 metadata |
| B13 | line | installer/bootstrapper/bootstrapper.rc | ProductVersion | `VALUE "ProductVersion", RENTAPC_VER_TEXT` | Packer rule B41: the release version text |
| B14 | line | installer/bootstrapper/bootstrapper.rc | variable info | `BLOCK "VarFileInfo"` | Windows version-resource layout |
| B15 | line | installer/bootstrapper/bootstrapper.rc | Translation | `VALUE "Translation", 0x409, 1200` | Language table: English (US) 0x409, code page 1200; matches B06 |

### Version macros (`scripts/pack-seathub.ps1`)

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B40 | text | scripts/pack-seathub.ps1 | numeric version | `$versionNumbers = "$($Matches[1]),$($Matches[2]),$($Matches[3]),0"` | Packer derives maj,min,patch,0 from the single version file (A-90) |
| B41 | text | scripts/pack-seathub.ps1 | RENTAPC_VER_NUMBERS | `#define RENTAPC_VER_NUMBERS $versionNumbers` | Written into bootstrapper-version.rci |
| B42 | text | scripts/pack-seathub.ps1 | RENTAPC_VER_TEXT | `#define RENTAPC_VER_TEXT ""$Version"""` | Written into bootstrapper-version.rci; FileVersion and ProductVersion text equal the release version |

### Setup window copy (`app/seathub/reporting_consent_store.h`, window section)

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B20 | literal | app/seathub/reporting_consent_store.h | notice EN | `To fix problems, RentaPC sends crash reports and logs to Sentry, a third-party service we use to receive them, and sends stream quality numbers and session evidence to RentaPC. They never include your PIN, password, email or phone number. You can change this any time in RentaPC Settings.` | A-0107, A-copy; equals PRIVACY-NOTICE.txt paragraph 1 |
| B21 | literal | app/seathub/reporting_consent_store.h | notice AR | `لنتمكن من إصلاح المشكلات، يرسل RentaPC تقارير الأعطال والسجلات إلى Sentry، وهي خدمة خارجية نستخدمها لاستلامها، ويرسل أرقام جودة البث وأدلة الجلسات إلى RentaPC. لا تتضمن أبدًا رمز PIN أو كلمة المرور أو البريد الإلكتروني أو رقم الهاتف. يمكنك تغيير ذلك في أي وقت من إعدادات RentaPC.` | A-0107, A-copy; equals PRIVACY-NOTICE.txt paragraph 2 |
| B22 | literal | app/seathub/reporting_consent_store.h | link URL | `https://rentapc.online/data-collection` | A-copy, A-0092 domains |
| B23 | literal | app/seathub/reporting_consent_store.h | window title (EN / AR) | `RentaPC setup  /  إعداد RentaPC` | A-copy: title `RentaPC setup`, AR «إعداد RentaPC»; unchanged by T083 |
| B24 | literal | app/seathub/reporting_consent_store.h | box label EN | `send crash reports, logs and session evidence` | A-copy |
| B25 | literal | app/seathub/reporting_consent_store.h | box label AR | `إرسال تقارير الأعطال والسجلات وأدلة الجلسات` | A-copy |
| B26 | literal | app/seathub/reporting_consent_store.h | warning EN | `We can't give proper support for errors with this turned off.` | A-copy |
| B27 | literal | app/seathub/reporting_consent_store.h | warning AR | `لا يمكننا تقديم دعم مناسب للأخطاء عند إيقاف هذا الخيار.` | A-copy |
| B28 | literal | app/seathub/reporting_consent_store.h | link label EN | `What we collect` | A-copy |
| B29 | literal | app/seathub/reporting_consent_store.h | link label AR | `ما الذي نجمعه` | A-copy |
| B30 | literal | app/seathub/reporting_consent_store.h | re-ask heading EN | `What changed` | A-copy; drawn only by the in-app re-ask window, never by the setup file (no change list is passed) |
| B31 | literal | app/seathub/reporting_consent_store.h | re-ask heading AR | `ما الذي تغيّر` | A-copy; same note as B30 |

### Setup window buttons (`installer/bootstrapper/main.cpp`)

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B32 | literal | installer/bootstrapper/main.cpp | primary action EN | `Install` | A-copy |
| B33 | literal | installer/bootstrapper/main.cpp | primary action AR | `تثبيت` | A-copy |
| B34 | literal | installer/bootstrapper/main.cpp | cancel action EN | `Cancel` | A-copy |
| B35 | literal | installer/bootstrapper/main.cpp | cancel action AR | `إلغاء` | A-copy |

### Error dialogs (`installer/bootstrapper/main.cpp`)

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B50 | messagebox | installer/bootstrapper/main.cpp | RentaPC setup | RentaPC setup could not unpack its installer. | A-T021 (no copy.md entry); title equals the window title, ADR-0092 name |
| B51 | messagebox | installer/bootstrapper/main.cpp | RentaPC setup | RentaPC setup could not start its installer. | A-T021 (no copy.md entry) |
| B52 | messagebox | installer/bootstrapper/main.cpp | RentaPC setup | RentaPC setup could not save your choice, so it did not install. | A-T021 (no copy.md entry) |

### Literals that are not visible copy

| ID | Kind | Source | Key | Expected | Authority |
|---|---|---|---|---|---|
| B60 | classified | installer/bootstrapper/main.cpp | private-temp-path | `RentaPC-setup-` | Folder prefix of the private temp folder; the customer never reads it |
| B61 | classified | installer/bootstrapper/main.cpp | private-temp-path | `-` | Separator inside that folder name |
| B62 | classified | installer/bootstrapper/main.cpp | extracted-file-name | `\\RentaPC-Setup.exe` | File the inner Setup is unpacked to, inside the private folder; matches B11 |
| B63 | classified | installer/bootstrapper/main.cpp | command-line | `\"` | Quoting of the inner Setup path |
| B64 | classified | installer/bootstrapper/main.cpp | command-line | ` ` | Argument separator |
| B65 | classified | installer/bootstrapper/main.cpp | command-line | `-s` | Silent switch the Velopack Setup also takes |
| B66 | classified | installer/bootstrapper/main.cpp | command-line | `--silent` | Silent switch |
| B67 | classified | installer/bootstrapper/main.cpp | command-line | `/s` | Silent switch |
| B68 | classified | installer/bootstrapper/main.cpp | command-line | `--reporting-test-key` | Route-proof switch, stripped before the inner Setup; never a customer string |
| B69 | classified | installer/bootstrapper/main.cpp | separator | `\r\n` | Notice text line-break conversion |
| B70 | classified | app/seathub/reporting_consent_store.h | registry | `Software\\Seven Hills\\SeatHub\\Reporting` | HKCU key of the recorded choice (ADR-0107) |
| B71 | classified | app/seathub/reporting_consent_store.h | registry | `Choice` | Registry value name |
| B72 | classified | app/seathub/reporting_consent_store.h | registry | `NoticeVersion` | Registry value name |
| B73 | classified | app/seathub/reporting_consent_store.h | control-class | `STATIC` | Win32 window class name |
| B74 | classified | app/seathub/reporting_consent_store.h | control-class | `EDIT` | Win32 window class name |
| B75 | classified | app/seathub/reporting_consent_store.h | control-class | `BUTTON` | Win32 window class name |
| B76 | classified | app/seathub/reporting_consent_store.h | separator | `  /  ` | EN / AR pairing of title, link label and headings |
| B77 | classified | app/seathub/reporting_consent_store.h | separator | ` / ` | EN / AR pairing of the Install and Cancel buttons |
| B78 | classified | app/seathub/reporting_consent_store.h | separator | `• ` | Bullet of the re-ask change list (not drawn by the setup file) |
| B79 | classified | app/seathub/reporting_consent_store.h | separator | `\r\n` | Line break of the re-ask change list |
| B80 | classified | app/seathub/reporting_consent_store.h | shell-verb | `open` | ShellExecute verb behind the "What we collect" link |
| B81 | classified | app/seathub/reporting_consent_store.h | window-class | `RentaPCReportingNotice` | Window class name, not drawn |
| B90 | vendor | installer/bootstrapper/main.cpp | Velopack inner Setup | Velopack's own progress, shortcut and failure wording | Drawn by the unchanged inner Setup; `vpk pack --packTitle RentaPC`; explicitly not RentaPC copy (A-copy) |
| B91 | vendor | installer/bootstrapper/main.cpp | Windows shell | SmartScreen, UAC and file-properties chrome | Windows draws these; File description comes from B08 |

Empty literals (`L""` defaults of the window specification) draw nothing and are skipped by the census.

## Paths a person can meet

1. **First interactive install** (no recorded choice): the window opens with the B23 title, B20/B21
   notice, the B24/B25 box (ticked by default), the B26/B27 warning only while unticked, the B28/B29
   link to B22, and the B32/B33 and B34/B35 buttons. Install records the choice, then starts the inner
   Setup, whose own screens are vendor text (B90).
2. **Cancel, Esc or close**: exit code 1223, nothing is recorded, no dialog.
3. **Silent install** (`-s`, `--silent`, `/s`): no window and no dialog; the send default is recorded only
   if there is no choice yet (ADR-0107), then the inner Setup runs silently.
4. **Interactive install with a recorded choice** (reinstall): no window; straight to the inner Setup.
5. **Updates**: applied by the Velopack updater that is installed next to the app, not by this file; the
   setup file does not run and shows nothing.
6. **Failures**: B50 (cannot unpack), B51 (cannot start), B52 (cannot save the choice, nothing installed).
   These are the only dialogs this program draws besides the window.

## Readback procedure for the candidate and the published file (T030)

The T083 test is a source check. After the 0.1.32 packer run, T030 must read the built file back, never
assume it from this page. Receipts are recorded by T030, not here.

1. Fetch the candidate (then, after publication, the published asset `RentaPC-Setup.exe`) into its own new,
   empty folder and compute its SHA-256.
2. Read the version resource: `(Get-Item <file>).VersionInfo | Format-List *` (PowerShell
   `FileVersionInfo`). Expect exactly: `CompanyName` = `RentaPC`; `FileDescription` = `RentaPC`;
   `FileVersion` = `0.1.32`; `InternalName` = `RentaPC-Setup`; `OriginalFilename` = `RentaPC-Setup.exe`;
   `ProductName` = `RentaPC`; `ProductVersion` = `0.1.32`; numeric file and product version `0.1.32.0`
   (`FileMajorPart`, `FileMinorPart`, `FileBuildPart`, `FilePrivatePart` = 0, 1, 32, 0); language table
   English (US) / Unicode (`Language` = `English (United States)`).
3. On a Windows desktop with no recorded choice (use `--reporting-test-key <throwaway key>` so the real
   key is untouched), start the file and capture the native window: read the title, notice, box label,
   warning (untick the box), link label and both buttons, and compare each with B23 to B35. Press Cancel
   and confirm exit code 1223.
4. Note in the receipt that steps 2 and 3 observe the built file, and which asset hash they observed.

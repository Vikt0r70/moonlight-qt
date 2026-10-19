@echo off
rem ===========================================================================
rem Build and run every SeatHub suite in this tree, in order, and report a real
rem exit code. Run it from anywhere; it always works in its own directory.
rem
rem   tests\run-all-suites.cmd
rem
rem Why this file exists:
rem   * Every suite is a single-config qmake project. The recipe is
rem     `qmake <suite>.pro && jom` - bare `jom`, because `jom release` fails on
rem     a project with `CONFIG -= debug_and_release debug`.
rem   * qmake writes one `Makefile` into tests\, so the suites cannot be built in
rem     parallel.
rem   * An earlier audit hand-rolled this loop and captured exit codes with the
rem     ERRORLEVEL percent-variable inside a parenthesised block, which expands
rem     when the block is parsed rather than when it runs. Every suite reported
rem     exit 0, including two that had not run at all. This file uses
rem     `setlocal enabledelayedexpansion` with the exclamation-mark form for the
rem     exit code, and reads each suite's own `Totals:` line as well, so a suite
rem     that dies before printing one is reported as failed rather than skipped.
rem   * A second parse trap, hit while writing this file: echoing a variable whose
rem     value contains a parenthesis (`%VCVARS%` lives under `Program Files
rem     (x86)`) inside a parenthesised `if` block makes cmd read that `(` as the
rem     start of a nested block and abort with "Microsoft was unexpected at this
rem     time" before a single line of the file runs. No variable that can hold a
rem     parenthesis is echoed inside a block here.
rem   * `tst_hud_bitmap` and `tst_overlay_injection` need SDL2 on PATH and Qt's
rem     platform plugins, or they abort with 0xC0000135 before printing a
rem     result. Both are set below.
rem   * The deployed `build\build-x64-release\app\release` directory must NOT be
rem     on PATH: it shadows the real Qt install.
rem
rem There is deliberately no `for` loop over the suite list: a nested
rem if/else inside a parenthesised block is what cmd mis-parses here, and the
rem flat `call :suite` list below cannot have that problem. Add a suite by
rem adding one line.
rem
rem Build logs land in `<suite>-build-out.txt` / `<suite>-qmake-out.txt`, results
rem in `<suite>-out.txt`. All three name patterns are gitignored.
rem   * Each suite that passes its ", 0 failed" check and exits 0 prints
rem     `[SUITE OK] <name>`, which every 06.3 fork check greps by name and counts
rem     against the `call :suite` lines above (I11-02).
rem   * Named-suite mode (06.3.1-08): run this file with one or more suite names as
rem     arguments to build and run only those, with the same vcvars, Qt and PATH setup
rem     as the full run below. It prints NAMED_SUITES_PASSED when every named suite
rem     passed - a task's own check can use it for fast feedback instead of the full run,
rem     which stays each plan's last-task check.
rem ===========================================================================
setlocal enabledelayedexpansion

set "QT=C:\Qt\6.11.2\msvc2022_64"
set "FORK=%~dp0.."
set "VSWHERE=%FORK%\scripts\vswhere.exe"

rem Toolchain discovery, same order as Find-VcVars in scripts\build-seathub.ps1 so the build
rem step and this step choose the same compiler: the validated fixed installs first (the
rem owner PC's BuildTools is the first), then component-filtered vswhere. A root counts only
rem when its VC\Tools\MSVC directory exists; see :try_vcvars_root. Flat statements only, no
rem parenthesised block, because these paths hold a parenthesis.
set "VCVARS="
call :try_vcvars_root "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call :try_vcvars_root "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
call :try_vcvars_root "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
call :try_vcvars_root "C:\Program Files\Microsoft Visual Studio\2022\Professional"
call :try_vcvars_root "C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined VCVARS if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do call :try_vcvars_root "%%I"
if not defined VCVARS goto :vcvars_missing
echo VCVARS_SELECTED=%VCVARS%

pushd "%~dp0"

call "%VCVARS%" >nul
if errorlevel 1 goto :vcvars_failed

set "PATH=%QT%\bin;%FORK%\libs\windows\lib\x64;%FORK%\build\sentry-native-0.17.1\install\bin;%PATH%"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%QT%\plugins"

set "FAILED="

if not "%~1"=="" goto :named

call :suite tst_attempt_vocab
call :suite tst_control_plane
call :suite tst_engine_seam
call :suite tst_error_map
call :suite tst_hud_bitmap
call :suite tst_liveness
call :suite tst_log_shipper
call :suite tst_log_tee
call :suite tst_overlay_injection
call :suite tst_pairing
call :suite tst_session_websocket
call :suite tst_sse_client
call :suite tst_settings_bridge
call :suite tst_stream_stats
call :suite tst_teardown
call :suite tst_threading
call :suite tst_token_store
call :suite tst_ui_screens
call :suite tst_update_feed
call :suite tst_install_journal
call :suite tst_update_retry
call :suite tst_facade_wiring
call :suite tst_d28_boundary
call :suite tst_osd_render
call :suite tst_telemetry
call :suite tst_single_instance
call :suite tst_reporting_consent

if defined FAILED goto :failed

echo ALL_SUITES_PASSED
popd
exit /b 0

rem ---------------------------------------------------------------------------
rem Named-suite mode: one or more suite names as arguments. Every statement here
rem is a flat single-line statement, with no parenthesised block - the same parse
rem trap the file header describes for the full run above.
rem ---------------------------------------------------------------------------
:named
if "%~1"=="" goto :named_done
call :suite_named %~1
shift
goto :named

:named_done
if defined FAILED goto :failed
echo NAMED_SUITES_PASSED
popd
exit /b 0

:failed
echo SOME_SUITES_FAILED
popd
exit /b 1

rem ---------------------------------------------------------------------------
rem Toolchain failures. Both exit nonzero before qmake runs.
rem ---------------------------------------------------------------------------
:vcvars_missing
echo VCVARS_FAILED - no Visual Studio 2022 install with the x64 C++ toolset was found
exit /b 1

:vcvars_failed
echo VCVARS_FAILED - the selected Visual Studio x64 environment did not load
popd
exit /b 1

rem ---------------------------------------------------------------------------
rem One Visual Studio install root as %~1: becomes VCVARS only when VCVARS is still unset,
rem vcvars64.bat exists and the install carries a VC\Tools\MSVC toolset directory (a Visual
rem Studio Community install without the C++ workload has the .bat but no toolset).
rem ---------------------------------------------------------------------------
:try_vcvars_root
if defined VCVARS goto :eof
if not exist "%~1\VC\Tools\MSVC" goto :eof
if not exist "%~1\VC\Auxiliary\Build\vcvars64.bat" goto :eof
set "VCVARS=%~1\VC\Auxiliary\Build\vcvars64.bat"
goto :eof

rem ---------------------------------------------------------------------------
rem One suite: qmake, jom, run, and read the suite's own Totals line.
rem :suite_named is the named-suite mode's own entry point into :suite - its call
rem line never matches the ^call :suite  count the full-run check above compares
rem against the [SUITE OK] lines.
rem ---------------------------------------------------------------------------
:suite_named
goto :suite

:suite
if exist "%~1-out.txt" del /q "%~1-out.txt"

"%QT%\bin\qmake.exe" "%~1.pro" > "%~1-qmake-out.txt" 2>&1
if errorlevel 1 (
    echo [QMAKE FAILED] %~1 - see %~1-qmake-out.txt
    set "FAILED=1"
    goto :eof
)

"%FORK%\scripts\jom.exe" -j8 > "%~1-build-out.txt" 2>&1
if errorlevel 1 (
    echo [BUILD FAILED] %~1 - see %~1-build-out.txt
    set "FAILED=1"
    goto :eof
)

rem Path-explicit (".\"): the bare name is resolved through PATH/App Paths, and a
rem non-interactive shell answers 9009 (command not found) for it even though the file is
rem right here after pushd. See .planning/debug/resolved/client-inapp-update-hangs.md.
".\%~1.exe" -o "%~1-out.txt,txt" >nul 2>&1
set "RC=!ERRORLEVEL!"

findstr /r /c:"^Totals:.*, 0 failed" "%~1-out.txt" >nul 2>&1
if errorlevel 1 (
    echo [SUITE FAILED] %~1 exit=!RC! - no ", 0 failed" line in %~1-out.txt
    set "FAILED=1"
    goto :eof
)

findstr /r /c:"^Totals:" "%~1-out.txt"

if !RC! neq 0 (
    echo [NONZERO EXIT] %~1 exited !RC!
    set "FAILED=1"
    goto :eof
)

echo [SUITE OK] %~1
goto :eof

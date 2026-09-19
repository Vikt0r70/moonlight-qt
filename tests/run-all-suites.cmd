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
rem     parallel. Two earlier phase audits hand-rolled this loop; one of them
rem     captured exit codes with `%ERRORLEVEL%` inside a parenthesised block,
rem     which expands at parse time, so every suite reported exit 0 - including
rem     two that had not run at all. The `setlocal enabledelayedexpansion` and
rem     `!ERRORLEVEL!` below are the fix for exactly that trap, and each suite's
rem     own `Totals:` line is read as well as its exit code.
rem   * `tst_hud_bitmap` and `tst_overlay_injection` need SDL2 on PATH and Qt's
rem     platform plugins, or they abort with 0xC0000135 before printing a
rem     result. Both are set below.
rem   * The deployed `build\build-x64-release\app\release` directory must NOT be
rem     on PATH: it shadows the real Qt install.
rem
rem Build logs land in <suite>-build-out.txt / <suite>-qmake-out.txt and results
rem in <suite>-out.txt (all three are gitignored).
rem ===========================================================================
setlocal enabledelayedexpansion

set "QT=C:\Qt\6.11.2\msvc2022_64"
set "FORK=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

pushd "%~dp0"

call "%VCVARS%" >nul
if errorlevel 1 (
    echo VCVARS_FAILED: %VCVARS%
    popd
    exit /b 1
)

set "PATH=%QT%\bin;%FORK%\libs\windows\lib\x64;%PATH%"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%QT%\plugins"

set "SUITES=tst_control_plane tst_engine_seam tst_error_map tst_hud_bitmap tst_liveness tst_overlay_injection tst_pairing tst_session_websocket tst_settings_bridge tst_teardown tst_threading tst_token_store tst_ui_screens tst_update_feed tst_facade_wiring tst_d28_boundary"

set "FAILED="

for %%S in (%SUITES%) do (
    if exist "%%S-out.txt" del /q "%%S-out.txt"
    "%QT%\bin\qmake.exe" "%%S.pro" > "%%S-qmake-out.txt" 2>&1
    if errorlevel 1 (
        echo [QMAKE FAILED] %%S - see %%S-qmake-out.txt
        set "FAILED=1"
    ) else (
        "%FORK%\scripts\jom.exe" -j8 > "%%S-build-out.txt" 2>&1
        if errorlevel 1 (
            echo [BUILD FAILED] %%S - see %%S-build-out.txt
            set "FAILED=1"
        ) else (
            ".\%%S.exe" -o "%%S-out.txt,txt" >nul 2>&1
            set "RC=!ERRORLEVEL!"
            findstr /r /c:"^Totals:.*, 0 failed" "%%S-out.txt" >nul 2>&1
            if errorlevel 1 (
                echo [SUITE FAILED] %%S ^(exit !RC!^) - no ", 0 failed" in %%S-out.txt
                set "FAILED=1"
            ) else (
                findstr /r /c:"^Totals:" "%%S-out.txt"
            )
            if !RC! neq 0 (
                echo [NONZERO EXIT] %%S exited !RC!
                set "FAILED=1"
            )
        )
    )
)

if defined FAILED (
    echo SOME_SUITES_FAILED
    popd
    exit /b 1
)

echo ALL_SUITES_PASSED
popd
exit /b 0

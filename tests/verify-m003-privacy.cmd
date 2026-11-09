@echo off
rem ===============================================================================================
rem T020 / M003 verified check: no customer address survives RentaPC diagnostics or a retained copy.
rem
rem What this proves, and what it does not:
rem   - the four suites below exercise the public seams only (LogShipper::scrub, the diagnostic
rem     lane, LogSpool, the real Sentry envelope collector) and print one digest per sanitized
rem     fixture through SEATHUB_PRIVACY_DIGEST_FILE;
rem   - docs/evidence/m003/client-privacy.json must record exactly those digests, so the receipt
rem     cannot drift from a live run (a stale receipt fails here);
rem   - the rig's own local file log is out of scope by owner ruling C3 (docs/spec/agents.md:351,
rem     ADR-0071) and is not exercised. Raw minidumps and third-party SDK internals are likewise
rem     outside what these suites can see; the receipt names that limit rather than claiming a
rem     universal byte scrub.
rem
rem Prerequisites come from the member repo's own documented path (scripts/build-seathub.ps1 pins
rem them; the suites' .pro files point at the same directories). Nothing is downloaded here: this
rem script checks for them and fails with the exact missing path.
rem ===============================================================================================
setlocal enabledelayedexpansion

set "FORK=%~dp0.."
set "QT=C:\Qt\6.11.2\msvc2022_64"
set "SENTRY=%FORK%\build\sentry-native-0.17.1\install"
set "SDL=%FORK%\libs\windows\lib\x64"
set "EVIDENCE=%FORK%\docs\evidence\m003\client-privacy.json"
set "DIGESTS=%~dp0privacy-digests-out.txt"

echo VERIFY_T020 no customer address off the rig

call :need_file "%QT%\bin\qmake.exe" "Qt 6.11.2 (the toolchain scripts/build-seathub.ps1 pins)"
if errorlevel 1 goto :prereq_failed
call :need_file "%QT%\bin\Qt6Core.dll" "Qt 6.11.2 (the toolchain scripts/build-seathub.ps1 pins)"
if errorlevel 1 goto :prereq_failed
call :need_file "%SDL%\SDL2.lib" "the prebuilt SDL2 the test .pro files link (libs submodule at its pinned commit)"
if errorlevel 1 goto :prereq_failed
call :need_file "%SENTRY%\lib\sentry.lib" "sentry-native 0.17.1 (scripts/build-seathub.ps1, pinned by SHA-256)"
if errorlevel 1 goto :prereq_failed
call :need_file "%FORK%\scripts\jom.exe" "the pinned jom.exe in scripts\"
if errorlevel 1 goto :prereq_failed
call :need_file "%EVIDENCE%" "the T020 evidence receipt"
if errorlevel 1 goto :prereq_failed
call :need_vcvars
if errorlevel 1 goto :prereq_failed

del /q "%DIGESTS%" 2>nul
set "SEATHUB_PRIVACY_DIGEST_FILE=%DIGESTS%"

rem qmake writes ONE Makefile into tests\, so named suites run serially - never in parallel.
call "%~dp0run-all-suites.cmd" tst_log_shipper tst_log_tee tst_reporting_consent tst_telemetry
if errorlevel 1 goto :suites_failed
if not exist "%DIGESTS%" goto :no_digests

rem Exactly the four fixtures the receipt must carry: no extras, none missing, none a zero match.
set "COUNT=0"
for /f "usebackq tokens=1,2,3" %%A in ("%DIGESTS%") do (
    if not "%%A"=="PRIVACY_DIGEST" goto :bad_digest_line
    set /a COUNT+=1
    findstr /c:"%%C" "%EVIDENCE%" >nul 2>&1
    if errorlevel 1 goto :receipt_mismatch
    echo DIGEST_OK %%B %%C
)
if not "!COUNT!"=="4" goto :digest_count

echo VERIFY_T020 PASS - four fixture digests match docs\evidence\m003\client-privacy.json
endlocal
exit /b 0

:need_file
if exist "%~1" exit /b 0
echo PREREQ_MISSING %~1 - %~2
exit /b 1

:need_vcvars
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" exit /b 0
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" exit /b 0
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" exit /b 0
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" exit /b 0
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" exit /b 0
echo PREREQ_MISSING a VS 2022 x64 C++ toolset (VC\Auxiliary\Build\vcvars64.bat) - see scripts\build-seathub.ps1
exit /b 1

:prereq_failed
echo VERIFY_T020 FAIL - a required prerequisite is absent; nothing was run and nothing is claimed
endlocal
exit /b 1

:suites_failed
echo VERIFY_T020 FAIL - a named suite failed (see tests\<suite>-out.txt)
endlocal
exit /b 1

:no_digests
echo VERIFY_T020 FAIL - tst_log_shipper produced no privacy digest file, so no fixture was proven
endlocal
exit /b 1

:bad_digest_line
echo VERIFY_T020 FAIL - unexpected line in %DIGESTS%
endlocal
exit /b 1

:receipt_mismatch
echo VERIFY_T020 FAIL - %EVIDENCE% does not record a digest a live run just produced
endlocal
exit /b 1

:digest_count
echo VERIFY_T020 FAIL - expected 4 fixture digests, found !COUNT!
endlocal
exit /b 1

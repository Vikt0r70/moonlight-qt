# One customer installer candidate, built and checked the way the owner builds it today (plan 04-05,
# REQ-INST01; ROADMAP Phase 4 SC4).
#
# What it does, in order, from one clean commit:
#   1. refuses a dirty tree (a candidate must be reproducible from the recorded commit);
#   2. scripts\build-seathub.ps1 with no reuse: full compile, then deploy;
#   3. tests\run-all-suites.cmd (the full 30-suite run) and its ALL_SUITES_PASSED line, plus the two
#      checks it does not make: no suite skipped a test, and every suite ran at least one test
#      beyond initTestCase/cleanupTestCase;
#   4. the client CI's three fork-boundary steps verbatim, through Git's bash;
#   5. the deployed exe's FileVersion against both version files;
#   6. with -StopBeforePack: stop here. Otherwise scripts\pack-seathub.ps1, then re-hash the outer
#      RentaPC-Setup.exe against setup-sha256.txt;
#   7. build\customer-candidate.json: one manifest the later release steps read.
#
# Usage, from anywhere:
#     powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\release_candidate.ps1 -StopBeforePack
#     powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\release_candidate.ps1
#
# -StopBeforePack is the dry run: everything but the packer, which needs vpk and the .NET SDK.
# This script uploads nothing, publishes nothing and writes no release row.
#
# The boundary steps below are copied, never parsed, from .github/workflows/seathub-build-win.yml
# (:32-53, :55-72, :74-99). They lead with `run: |` in the workflow, so YAML hands bash the block
# with the workflow's common indentation stripped; that is the text copied here. A fourth step in
# that workflow (:101-114, unmarked lines added to session.cpp) is deliberately not copied: its own
# comment says the exact line set is pinned by tests/tst_d28_boundary.cpp, which runs inside the
# suites above.
#
# Git's bash is named explicitly. A bare `bash` on this PC is WSL's C:\WINDOWS\system32\bash.exe,
# which has no git on its PATH and would report the diff steps as failures for the wrong reason.
#
# build-seathub.ps1 and pack-seathub.ps1 are called as child processes under pwsh, the interpreter
# their own usage lines name. Windows PowerShell 5.1 cannot run build-seathub.ps1 at all: the banner
# cl.exe writes to stderr becomes a terminating NativeCommandError at build-seathub.ps1:98 because
# both scripts set $ErrorActionPreference = 'Stop'. This script itself stays runnable under either
# interpreter.

[CmdletBinding()]
param(
    [string]$QtDir = 'C:\Qt\6.11.2\msvc2022_64',
    [string]$VcVars = '',
    [switch]$StopBeforePack
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$GitBash = 'C:\Program Files\Git\bin\bash.exe'

# The interpreter for the two existing scripts (see the header). pwsh is what they are written for;
# powershell.exe is the fallback only so a missing pwsh fails inside the called script, not here.
function Get-ChildInterpreter {
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pwsh) { return $pwsh.Source }
    return 'powershell.exe'
}

# Runs one of the existing release scripts as its own process, so its own error preference, and the
# stderr the toolchain writes, cannot stop this script mid-way.
function Invoke-ChildScript([string]$ScriptPath, [string]$QtDir, [string]$VcVars) {
    $interpreter = Get-ChildInterpreter
    $childArgs = @('-NoProfile', '-File', $ScriptPath, '-QtDir', $QtDir)
    if ($VcVars) { $childArgs += @('-VcVars', $VcVars) }
    & $interpreter @childArgs
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$(Split-Path -Leaf $ScriptPath) failed with exit code $code" }
}

function Step([string]$text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }

# The full run's suites, in order: one `call :suite <name>` line each in run-all-suites.cmd. The
# named-suite entry point (`call :suite_named %~1`) does not match the pattern, by its own design.
function Get-FullRunSuites([string]$RunnerPath) {
    $names = @(Get-Content -LiteralPath $RunnerPath |
        Where-Object { $_ -match '^call :suite (\S+)\s*$' } |
        ForEach-Object { ($_ -replace '^call :suite ', '').Trim() })
    if ($names.Count -lt 1) { throw "no 'call :suite <name>' lines in $RunnerPath" }
    return $names
}

# What run-all-suites.cmd does not check, read from one suite's own QtTest report: no test skipped,
# and at least one PASS line beyond the two fixture hooks. Returns one string per problem, so an
# empty result means the suite's report is acceptable.
function Test-SuiteOutput([string]$Name, [AllowEmptyString()][string]$Text) {
    $problems = New-Object System.Collections.Generic.List[string]
    $totals = [regex]::Match($Text, '(?m)^Totals: (\d+) passed, (\d+) failed, (\d+) skipped, ')
    if (-not $totals.Success) {
        $problems.Add("${Name}: no 'Totals: ... passed, ... failed, ... skipped' line in its out file")
        return $problems.ToArray()
    }
    $skipped = [int]$totals.Groups[3].Value
    if ($skipped -ne 0) { $problems.Add("${Name}: $skipped test(s) skipped") }

    $realPass = $false
    foreach ($pass in [regex]::Matches($Text, '(?m)^PASS   : .*$')) {
        if ($pass.Value -notmatch '::(init|cleanup)TestCase\(\)\s*$') { $realPass = $true; break }
    }
    if (-not $realPass) {
        $problems.Add("${Name}: no PASS line beyond initTestCase/cleanupTestCase - no test actually ran")
    }
    return $problems.ToArray()
}

# The three fork-boundary steps, verbatim from .github/workflows/seathub-build-win.yml.
$BoundarySteps = @(
    @{
        Name = 'engine files unchanged except overlaymanager/session.cpp (:32-53)'
        Body = @'
set -euo pipefail
CHANGED=$(git diff --name-only v6.1.0..HEAD -- \
  'app/streaming/video/*' 'app/streaming/audio/*' 'app/streaming/input/*' \
  'app/streaming/*.cpp' 'app/streaming/*.h')
if [ -z "$CHANGED" ]; then
  echo "No app/streaming/ files changed — gate passes trivially."
  exit 0
fi
echo "Changed app/streaming/ files:"
echo "$CHANGED"
# The only two permitted exceptions (D-28's overlay compositor, ADR-0046's session.cpp
# title literal). This regex is the single source; see the header comment above.
DISALLOWED=$(echo "$CHANGED" | grep -Ev 'overlaymanager|session\.cpp' || true)
if [ -n "$DISALLOWED" ]; then
  echo "::error::Disallowed engine file(s) modified outside the FORK-CHANGES.md exception list:"
  echo "$DISALLOWED"
  exit 1
fi
echo "Only exception-listed files changed under app/streaming/ — gate passes."
'@
    },
    @{
        Name = 'each changed exception file has a FORK-CHANGES.md entry (:55-72)'
        Body = @'
set -euo pipefail
CHANGED=$(git diff --name-only v6.1.0..HEAD -- \
  'app/streaming/video/*' 'app/streaming/audio/*' 'app/streaming/input/*' \
  'app/streaming/*.cpp' 'app/streaming/*.h' | grep -E 'overlaymanager|session\.cpp' || true)
if [ -z "$CHANGED" ]; then
  exit 0
fi
MISSING=""
for f in $CHANGED; do
  grep -qF "$f" FORK-CHANGES.md || MISSING="$MISSING $f"
done
if [ -n "$MISSING" ]; then
  echo "::error::These exception-listed files changed but have no FORK-CHANGES.md entry:$MISSING"
  exit 1
fi
'@
    },
    @{
        Name = 'app/backend beyond ADR-0109''s one path (:74-99)'
        Body = @'
set -euo pipefail
# ADR-0109 grants exactly one backend path. The comparison is the whole path (-x -F), so
# a similar name such as nvpairingmanager.cpp.orig is refused. The exact added and
# removed lines are pinned by tests/tst_d28_boundary.cpp; this step refuses another
# path, an unmarked added line, and a missing FORK-CHANGES.md entry.
CHANGED=$(git diff --name-only v6.1.0..HEAD -- 'app/backend/*')
DISALLOWED=$(echo "$CHANGED" | grep -vxF 'app/backend/nvpairingmanager.cpp' | grep -v '^$' || true)
if [ -n "$DISALLOWED" ]; then
  echo "::error::Disallowed backend file(s) modified outside ADR-0109's one path:"
  echo "$DISALLOWED"
  exit 1
fi
if [ -n "$CHANGED" ]; then
  grep -qF 'app/backend/nvpairingmanager.cpp' FORK-CHANGES.md \
    || { echo "::error::app/backend/nvpairingmanager.cpp changed but has no FORK-CHANGES.md entry"; exit 1; }
  UNMARKED=$(git diff -U0 v6.1.0..HEAD -- app/backend/nvpairingmanager.cpp \
    | grep -E '^\+' | grep -Ev '^\+\+\+' | grep -v 'ADR-0109' || true)
  if [ -n "$UNMARKED" ]; then
    echo "::error::nvpairingmanager.cpp gained line(s) with no ADR-0109 marker:"
    echo "$UNMARKED"
    exit 1
  fi
fi
'@
    }
)

# Runs one copied boundary step in its own temp .sh through Git's bash, from the repo root. The body
# is written with LF endings: bash treats a stray CR as part of the command.
function Invoke-BoundaryStep([string]$Name, [string]$Body, [string]$RepoRoot) {
    $bash = $GitBash
    if (-not (Test-Path -LiteralPath $bash)) { throw "Git's bash not found: $bash" }
    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("seathub-boundary-{0}.sh" -f ([guid]::NewGuid().ToString('N')))
    try {
        $lf = $Body -replace "`r`n", "`n"
        [System.IO.File]::WriteAllText($tempFile, $lf + "`n", (New-Object System.Text.UTF8Encoding($false)))
        Push-Location $RepoRoot
        try {
            & $bash $tempFile
            $code = $LASTEXITCODE
        }
        finally { Pop-Location }
    }
    finally {
        if (Test-Path -LiteralPath $tempFile) { Remove-Item -LiteralPath $tempFile -Force }
    }
    if ($code -ne 0) { throw "boundary step failed (exit $code): $Name" }
    Write-Host "boundary step passed: $Name"
}

function Write-CandidateManifest(
    [string]$ManifestPath,
    [string]$AppVersion,
    [AllowNull()][string]$SetupVersion,
    [bool]$DryRun,
    [AllowNull()][string]$SetupSha256,
    [string]$ClientCommit,
    [string]$ClientTree,
    [string]$FileVersion,
    [int]$SuiteCount) {
    $manifest = [ordered]@{
        versions   = [ordered]@{
            app   = $AppVersion
            setup = $SetupVersion
        }
        dry_run     = $DryRun
        # Plan 04-05 T1: null while this is a dry run, because nothing has been packed and no hash
        # exists yet. An empty string would let a later reader treat "not computed" as a value.
        setup_sha256 = if ($SetupSha256) { $SetupSha256 } else { $null }
        published   = $false
        signed      = $false
        client      = [ordered]@{
            commit = $ClientCommit
            tree   = $ClientTree
        }
        file_version = $FileVersion
        suite_count  = $SuiteCount
        boundary     = [ordered]@{
            steps  = $BoundarySteps.Count
            passed = $true
        }
    }
    $json = $manifest | ConvertTo-Json -Depth 5
    Set-Content -LiteralPath $ManifestPath -Value $json -Encoding UTF8
    Write-Host "manifest : $ManifestPath"
}

function Invoke-ReleaseCandidate {
    param(
        [string]$QtDir,
        [string]$VcVars,
        [switch]$StopBeforePack
    )

    $repoRoot = Split-Path -Parent $PSScriptRoot
    $buildRoot = Join-Path $repoRoot 'build'
    $deployFolder = Join-Path $buildRoot 'deploy-x64-release'
    $manifestPath = Join-Path $buildRoot 'customer-candidate.json'

    Push-Location $repoRoot
    try {
        Step 'Clean tree'
        $dirty = @(& git status --porcelain)
        if ($LASTEXITCODE -ne 0) { throw 'git status failed; is this a git checkout?' }
        if ($dirty.Count -gt 0) {
            throw "the tree is dirty; a candidate must come from one clean commit:`n$($dirty -join "`n")"
        }
        $clientCommit = ([string](& git rev-parse HEAD)).Trim()
        $clientTree = ([string](& git rev-parse 'HEAD^{tree}')).Trim()
        if ($clientCommit -notmatch '^[0-9a-f]{40}$' -or $clientTree -notmatch '^[0-9a-f]{40}$') {
            throw "could not read the commit and tree: '$clientCommit' / '$clientTree'"
        }
        Write-Host "commit   : $clientCommit"
        Write-Host "tree     : $clientTree"

        Step 'Build (full compile, no reuse)'
        $buildScript = Join-Path $PSScriptRoot 'build-seathub.ps1'
        Invoke-ChildScript -ScriptPath $buildScript -QtDir $QtDir -VcVars $VcVars

        Step 'Full suite run'
        $runner = Join-Path $repoRoot 'tests\run-all-suites.cmd'
        if (-not (Test-Path -LiteralPath $runner)) { throw "suite runner not found: $runner" }
        $suiteNames = Get-FullRunSuites $runner
        Write-Host "suites   : $($suiteNames.Count)"

        # tst_osd_render's picture sheet is produced on request only, by design; a candidate run
        # requests it so no test is skipped, and the sheet lands in build\ with the rest of the
        # output.
        $env:SEATHUB_PICTURE_SHEET_DIR = Join-Path $buildRoot 'picture-sheet'
        # The suite runner's toolchain probe leaves harmless warnings on stderr (vcvars64.bat looks
        # for its own vswhere). Redirecting stderr while $ErrorActionPreference is 'Stop' would turn
        # the first of those into a terminating error, so it is relaxed for this call only; the
        # runner's own exit code and its Totals lines still decide the result.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $suiteOutput = @(& cmd.exe /d /c "`"$runner`"" 2>&1 | ForEach-Object { "$_" })
            $suiteExit = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $previousPreference }
        $suiteLog = Join-Path $buildRoot 'customer-candidate-suites.log'
        Set-Content -LiteralPath $suiteLog -Value $suiteOutput -Encoding UTF8
        $suiteOutput | ForEach-Object { Write-Host $_ }
        if ($suiteExit -ne 0) { throw "tests\run-all-suites.cmd exited $suiteExit (log: $suiteLog)" }
        if (-not ($suiteOutput -match '^ALL_SUITES_PASSED$')) {
            throw "tests\run-all-suites.cmd did not print ALL_SUITES_PASSED (log: $suiteLog)"
        }

        $okCount = @($suiteOutput | Where-Object { $_ -match '^\[SUITE OK\] ' }).Count
        if ($okCount -ne $suiteNames.Count) {
            throw "[SUITE OK] lines: $okCount, suites in the full run: $($suiteNames.Count)"
        }

        $problems = New-Object System.Collections.Generic.List[string]
        foreach ($name in $suiteNames) {
            $outFile = Join-Path $repoRoot "tests\$name-out.txt"
            if (-not (Test-Path -LiteralPath $outFile)) {
                $problems.Add("${name}: no out file at $outFile")
                continue
            }
            foreach ($problem in @(Test-SuiteOutput -Name $name -Text (Get-Content -LiteralPath $outFile -Raw))) {
                $problems.Add($problem)
            }
        }
        if ($problems.Count -gt 0) {
            throw "suite result check failed:`n$($problems -join "`n")"
        }
        Write-Host "suite result check passed: $($suiteNames.Count) suites, no skips, every suite ran a real test"

        Step 'Fork boundary (the client CI steps)'
        foreach ($boundaryStep in $BoundarySteps) {
            Invoke-BoundaryStep -Name $boundaryStep.Name -Body $boundaryStep.Body -RepoRoot $repoRoot
        }

        Step 'Versions'
        $versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'app\seathub\seathub_version.h') -Raw
        if ($versionHeader -notmatch 'SEATHUB_VERSION\s+"([^"]+)"') {
            throw 'SEATHUB_VERSION not found in app\seathub\seathub_version.h'
        }
        $appVersion = $Matches[1]
        $versionTxt = (Get-Content -LiteralPath (Join-Path $repoRoot 'app\version.txt') -Raw).Trim()
        if ($versionTxt -ne $appVersion) {
            throw "version files disagree: app\version.txt says '$versionTxt' and app\seathub\seathub_version.h says '$appVersion'"
        }
        $deployedExe = Join-Path $deployFolder 'SeatHub.exe'
        if (-not (Test-Path -LiteralPath $deployedExe)) { throw "the deployed client not found: $deployedExe" }
        $fileVersion = (Get-Item -LiteralPath $deployedExe).VersionInfo.FileVersion
        if ($fileVersion -notlike "$appVersion*") {
            throw "the deployed SeatHub.exe reports file version $fileVersion, expected $appVersion"
        }
        Write-Host "app      : $appVersion (both version files and the built exe)"

        if ($StopBeforePack) {
            Write-CandidateManifest -ManifestPath $manifestPath -AppVersion $appVersion -SetupVersion $null `
                -DryRun $true -SetupSha256 $null -ClientCommit $clientCommit -ClientTree $clientTree `
                -FileVersion $fileVersion -SuiteCount $suiteNames.Count
            Write-Host "`nDry run: stopped before the packer as asked." -ForegroundColor Green
            return
        }

        Step 'Pack'
        $packScript = Join-Path $PSScriptRoot 'pack-seathub.ps1'
        Invoke-ChildScript -ScriptPath $packScript -QtDir $QtDir -VcVars $VcVars

        $releaseDir = Join-Path $buildRoot 'velopack-x64-release'
        $setup = Join-Path $releaseDir 'RentaPC-Setup.exe'
        $shaFile = Join-Path $releaseDir 'setup-sha256.txt'
        if (-not (Test-Path -LiteralPath $setup)) { throw "the packed installer not found: $setup" }
        if (-not (Test-Path -LiteralPath $shaFile)) { throw "setup-sha256.txt not found: $shaFile" }
        $recorded = (Get-Content -LiteralPath $shaFile -Raw).Trim().ToLower()
        $actual = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
        if ($recorded -ne $actual) {
            throw "RentaPC-Setup.exe hashes to $actual but setup-sha256.txt records $recorded"
        }
        $setupVersion = (Get-Item -LiteralPath $setup).VersionInfo.FileVersion
        Write-Host "setup    : $setupVersion  sha256 $actual"

        Write-CandidateManifest -ManifestPath $manifestPath -AppVersion $appVersion -SetupVersion $setupVersion `
            -DryRun $false -SetupSha256 $actual -ClientCommit $clientCommit -ClientTree $clientTree `
            -FileVersion $fileVersion -SuiteCount $suiteNames.Count
        Write-Host "`nCandidate ready. Nothing was uploaded or published." -ForegroundColor Green
    }
    finally {
        Pop-Location
    }
}

# Dot-sourced by tests\test_release_candidate.ps1, which only wants the functions above.
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-ReleaseCandidate -QtDir $QtDir -VcVars $VcVars -StopBeforePack:$StopBeforePack
}

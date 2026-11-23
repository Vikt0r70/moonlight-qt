# Tests the suite-result check that scripts\release_candidate.ps1 runs over each suite out file
# (plan 04-05, REQ-INST01).
#
# Why this file exists: run-all-suites.cmd already fails on a missing ", 0 failed" Totals line and on
# a nonzero exit, but it never reads the skipped count and it would accept a suite whose only PASS
# lines are initTestCase/cleanupTestCase. release_candidate.ps1 closes those two gaps, and this file
# proves the check fails on each of them before the script is trusted with a candidate run.
#
# Usage, from anywhere:
#     powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\test_release_candidate.ps1
#
# The check is dot-sourced out of the script, so this file never builds, runs a suite or writes a
# manifest.

$ErrorActionPreference = 'Stop'

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\release_candidate.ps1'
if (-not (Test-Path -LiteralPath $scriptPath)) { throw "scripts\release_candidate.ps1 not found: $scriptPath" }
. $scriptPath

if (-not (Get-Command Test-SuiteOutput -ErrorAction SilentlyContinue)) {
    throw 'scripts\release_candidate.ps1 does not define Test-SuiteOutput'
}

$failures = 0
function Check([string]$label, [bool]$passed) {
    if ($passed) {
        Write-Host "PASS: $label"
    }
    else {
        Write-Host "FAIL: $label"
        $script:failures++
    }
}

# A real passing suite as QtTest's -o <file>,txt writes it: a few named tests between the fixture
# hooks.
$goodOut = @'
********* Start testing of TstExample *********
Config: Using QtTest library 6.11.2, Qt 6.11.2
PASS   : TstExample::initTestCase()
PASS   : TstExample::doesTheThing()
PASS   : TstExample::doesItTwice()
PASS   : TstExample::cleanupTestCase()
Totals: 4 passed, 0 failed, 0 skipped, 0 blacklisted, 5ms
********* Finished testing of TstExample *********
'@

# The same suite with one test skipped: run-all-suites.cmd accepts this, the candidate run must not.
$skippedOut = @'
********* Start testing of TstExample *********
PASS   : TstExample::initTestCase()
SKIP   : TstExample::onRequest() - produced on request only
PASS   : TstExample::doesTheThing()
PASS   : TstExample::cleanupTestCase()
Totals: 3 passed, 0 failed, 1 skipped, 0 blacklisted, 5ms
********* Finished testing of TstExample *********
'@

# No test ran at all: only the two fixture hooks printed PASS.
$hooksOnlyOut = @'
********* Start testing of TstExample *********
PASS   : TstExample::initTestCase()
PASS   : TstExample::cleanupTestCase()
Totals: 2 passed, 0 failed, 0 skipped, 0 blacklisted, 0ms
********* Finished testing of TstExample *********
'@

$goodProblems = @(Test-SuiteOutput -Name 'tst_example' -Text $goodOut)
Check 'a fully passing suite reports no problem' ($goodProblems.Count -eq 0)

$skippedProblems = @(Test-SuiteOutput -Name 'tst_example' -Text $skippedOut)
Check 'a skipped test is reported' ($skippedProblems.Count -gt 0)
Check 'the skip report names the suite and the count' (
    ($skippedProblems -join ' ') -match 'tst_example' -and ($skippedProblems -join ' ') -match '1')

$hooksOnlyProblems = @(Test-SuiteOutput -Name 'tst_example' -Text $hooksOnlyOut)
Check 'a suite whose only PASS lines are the fixture hooks is reported' ($hooksOnlyProblems.Count -gt 0)
Check 'the hooks-only report says no real PASS line ran' (
    ($hooksOnlyProblems -join ' ') -match 'initTestCase')

# The manifest is the one artefact 04-06 reads (plan 04-05 T1's key list). A dry run has not packed,
# so setup_sha256 must be JSON null rather than an empty string: a later step that tests it for a
# hash has to see "not computed yet", not "computed as nothing".
$manifestPath = Join-Path ([IO.Path]::GetTempPath()) ('candidate-manifest-' + [guid]::NewGuid().ToString('n') + '.json')
try {
    Write-CandidateManifest -ManifestPath $manifestPath -AppVersion '0.1.38' -SetupVersion $null -DryRun $true `
        -SetupSha256 $null -ClientCommit ('a' * 40) -ClientTree ('b' * 40) -FileVersion '0.1.38.0' `
        -SuiteCount 30 | Out-Null
    $dryManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $dryKeys = @($dryManifest.PSObject.Properties.Name)
    Check 'the dry-run manifest carries the plan''s keys' (
        @(@('versions', 'dry_run', 'setup_sha256', 'published', 'signed', 'client', 'file_version',
            'suite_count', 'boundary') | Where-Object { $dryKeys -notcontains $_ }).Count -eq 0)
    Check 'a dry-run manifest leaves setup_sha256 null' ($null -eq $dryManifest.setup_sha256)
    Check 'a dry-run manifest is flagged and not published' (
        $dryManifest.dry_run -eq $true -and $dryManifest.published -eq $false -and $dryManifest.signed -eq $false)
    Check 'the manifest records the app version and both client revisions' (
        $dryManifest.versions.app -eq '0.1.38' -and $dryManifest.client.commit -eq ('a' * 40) -and $dryManifest.client.tree -eq ('b' * 40))

    Write-CandidateManifest -ManifestPath $manifestPath -AppVersion '0.1.38' -SetupVersion '0.1.38' -DryRun $false `
        -SetupSha256 ('c' * 64) -ClientCommit ('a' * 40) -ClientTree ('b' * 40) -FileVersion '0.1.38.0' `
        -SuiteCount 30 | Out-Null
    $packedManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Check 'a packed manifest carries the setup hash it measured' (
        $packedManifest.setup_sha256 -eq ('c' * 64) -and $packedManifest.dry_run -eq $false)
    Check 'a packed manifest still records a passing boundary' (
        $packedManifest.boundary.passed -eq $true -and $packedManifest.boundary.steps -eq 3)
}
finally {
    Remove-Item -LiteralPath $manifestPath -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "TEST_RELEASE_CANDIDATE_FAILED ($failures check(s))"
    exit 1
}

Write-Host 'TEST_RELEASE_CANDIDATE_PASSED'
exit 0

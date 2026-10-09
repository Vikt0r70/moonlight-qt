# Packs the SeatHub deploy folder into the Velopack release (ADR-0086, I-SEATHUB).
#
# Input is the folder scripts\build-seathub.ps1 deploys. Output, in build\velopack-x64-release\:
#     <packId>-win-Setup.exe             the one-click, per-user, no-administrator installer. It shows one
#                                        small window with Velopack's splash and progress bar and opens
#                                        SeatHub when it finishes. Its SHA-256 is the release row's `sha256`
#                                        (D-43) and is also written to setup-sha256.txt.
#     <packId>-<version>-full.nupkg      the full package - Velopack's update payload, uploaded whole,
#                                        never repacked
#     releases.win.json                  Velopack's manifest for the `win` channel (the folder that holds
#                                        Setup.exe also holds this and the package: the `url-folder`
#                                        update source of docs/evidence/m001/velopack-proof.md)
# (vpk also writes a portable zip and a legacy RELEASES file; neither is published.)
#
# Usage, from anywhere:
#     pwsh -File scripts\pack-seathub.ps1
#     pwsh -File scripts\pack-seathub.ps1 -SplashImage path\to\splash.png
#
# vpk is the pinned Velopack CLI (the .NET tool, version below). Install it once with
#     dotnet tool install --global vpk --version 1.2.161
# which needs the .NET SDK (8.0.425 is what T082 proved the pack with). This script refuses any other vpk
# version rather than pack with an unproven one. Nothing here signs anything: the release is unsigned
# (D-43), the code-signing certificate stays parked, and "No signing parameters provided" from vpk is
# expected.
#
# Branding is exactly the pack title, the icon, the splash image and the progress-bar colour. The bar
# colour is the design system's own primary token (app\gui\Tokens.qml `primaryDefault`), never a colour
# invented here. No splash image is checked into the fork; pass one with -SplashImage, otherwise Velopack's
# default splash is used. Velopack's own dialog wording is the vendor's, not SevenHills copy.

[CmdletBinding()]
param(
    [string]$DeployFolder = '',
    [string]$OutputDir = '',
    [string]$SplashImage = '',
    [string]$ProgressColor = '',
    [string]$Vpk = 'vpk'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# What docs/evidence/m001/velopack-proof.md recorded (VP-VERDICT: use exactly these). The packId is never
# `SeatHub`: that is the retired Tauri client's own %LOCALAPPDATA% folder.
$PackId = 'SeatHubVelopackProof'
$VpkVersion = '1.2.161'
$PackTitle = 'SeatHub'
$PackAuthors = 'Seven Hills'
$MainExe = 'SeatHub.exe'
$Runtime = 'win-x64'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $RepoRoot 'build'
$DefaultOutput = Join-Path $BuildRoot 'velopack-x64-release'
if (-not $DeployFolder) { $DeployFolder = Join-Path $BuildRoot 'deploy-x64-release' }
if (-not $OutputDir) { $OutputDir = $DefaultOutput }

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Require($path, $what) {
    if (-not (Test-Path -LiteralPath $path)) { throw "$what not found: $path" }
}

# ---------------------------------------------------------------- inputs
Step 'Inputs'
Require (Join-Path $DeployFolder $MainExe) "deployed client ($MainExe) - run scripts\build-seathub.ps1 first"
$iconPath = Join-Path $RepoRoot 'app\seathub.ico'
Require $iconPath 'SeatHub icon'
if ($SplashImage) { Require $SplashImage 'splash image' }

# One place names the version: the header the client compiles in (D-46), which app\version.txt must equal.
$versionHeader = Get-Content -LiteralPath (Join-Path $RepoRoot 'app\seathub\seathub_version.h') -Raw
if ($versionHeader -notmatch 'SEATHUB_VERSION\s+"([^"]+)"') {
    throw 'SEATHUB_VERSION not found in app\seathub\seathub_version.h'
}
$Version = $Matches[1]
$VersionTxt = (Get-Content -LiteralPath (Join-Path $RepoRoot 'app\version.txt') -Raw).Trim()
if ($VersionTxt -ne $Version) {
    throw "version files disagree: app\version.txt says '$VersionTxt' but app\seathub\seathub_version.h says '$Version'. Bump both together in one commit (A-90)."
}
$deployedVersion = (Get-Item -LiteralPath (Join-Path $DeployFolder $MainExe)).VersionInfo.FileVersion
if ($deployedVersion -notlike "$Version*") {
    throw "the deployed $MainExe reports file version $deployedVersion, expected $Version - rebuild before packing"
}

if (-not $ProgressColor) {
    $tokens = Get-Content -LiteralPath (Join-Path $RepoRoot 'app\gui\Tokens.qml') -Raw
    if ($tokens -notmatch 'primaryDefault:\s*"(#[0-9A-Fa-f]{6})"') {
        throw 'primaryDefault colour not found in app\gui\Tokens.qml; pass -ProgressColor'
    }
    $ProgressColor = $Matches[1]
}
Write-Host "packId   : $PackId"
Write-Host "version  : $Version"
Write-Host "title    : $PackTitle"
Write-Host "bar      : $ProgressColor"
Write-Host ("splash   : " + $(if ($SplashImage) { $SplashImage } else { '(Velopack default)' }))

# ---------------------------------------------------------------- the pinned vpk
Step "vpk $VpkVersion"
if (-not (Get-Command $Vpk -ErrorAction SilentlyContinue)) {
    throw "vpk not found. Install the pinned CLI: dotnet tool install --global vpk --version $VpkVersion (needs the .NET SDK)"
}
$vpkHelp = (& $Vpk -h 2>&1 | Out-String)
if ($vpkHelp -notmatch 'Velopack CLI\s+([0-9][^\s,]*)') {
    throw "could not read the vpk version from 'vpk -h'"
}
$vpkSeen = $Matches[1]
if ($vpkSeen -ne $VpkVersion) {
    throw "vpk $vpkSeen is installed but T082 proved $VpkVersion; install it with: dotnet tool update --global vpk --version $VpkVersion"
}
Write-Host "vpk $vpkSeen (the pinned version)"

# ---------------------------------------------------------------- pack
Step 'Pack'
if (Test-Path -LiteralPath $OutputDir) {
    if ([System.IO.Path]::GetFullPath($OutputDir) -ne [System.IO.Path]::GetFullPath($DefaultOutput)) {
        if (Get-ChildItem -LiteralPath $OutputDir -Force) { throw "$OutputDir is not empty; pass an empty folder so no stale package enters the release" }
    }
    else {
        # The default folder is this script's own output: clear it so a stale package never rides along.
        Remove-Item -LiteralPath $OutputDir -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$packArgs = @(
    'pack',
    '--packId', $PackId,
    '--packVersion', $Version,
    '--packDir', $DeployFolder,
    '--mainExe', $MainExe,
    '--runtime', $Runtime,
    '--outputDir', $OutputDir,
    '--packTitle', $PackTitle,
    '--packAuthors', $PackAuthors,
    '--icon', $iconPath,
    '--splashProgressColor', $ProgressColor
)
if ($SplashImage) { $packArgs += @('--splashImage', $SplashImage) }
& $Vpk @packArgs
if ($LASTEXITCODE -ne 0) { throw "vpk pack failed with exit code $LASTEXITCODE" }

# ---------------------------------------------------------------- verify the release
Step 'Verify the release'
$setup = Join-Path $OutputDir "$PackId-win-Setup.exe"
$full = Join-Path $OutputDir "$PackId-$Version-full.nupkg"
$manifest = Join-Path $OutputDir 'releases.win.json'
Require $setup 'Setup.exe'
Require $full 'the full package'
Require $manifest 'releases.win.json'
$manifestJson = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
$listed = @($manifestJson.Assets | Where-Object { $_.Type -eq 'Full' -and $_.Version -eq $Version })
if ($listed.Count -ne 1) { throw "releases.win.json does not list exactly one Full asset for $Version" }
if ($listed[0].PackageId -ne $PackId) { throw "releases.win.json names package '$($listed[0].PackageId)', expected '$PackId'" }

$setupHash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLower()
Set-Content -LiteralPath (Join-Path $OutputDir 'setup-sha256.txt') -Value $setupHash -Encoding ASCII
Write-Host ("{0}  {1:N1} MB" -f (Split-Path -Leaf $setup), ((Get-Item -LiteralPath $setup).Length / 1MB))
Write-Host ("{0}  {1:N1} MB" -f (Split-Path -Leaf $full), ((Get-Item -LiteralPath $full).Length / 1MB))
Write-Host "setup sha256  $setupHash"

Write-Host "`nDone." -ForegroundColor Green
Write-Host "  release  : $OutputDir"
Write-Host "`nPublish Setup.exe, the full package and releases.win.json into one folder, then put the row"
Write-Host "(url = that Setup.exe, sha256 = the digest above) in the release feed (docs/runbooks/client-release.md)."
Write-Host "The install is per user under %LOCALAPPDATA%\$PackId with no administrator prompt; the release is"
Write-Host "unsigned, so Windows may show its unknown-publisher warning for a browser download (D-43)."

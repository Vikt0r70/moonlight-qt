# Builds, deploys and packages the SeatHub client (D-15, D-16, D-42, D-43).
#
# This is the recipe the fork was missing: the binary built, but nothing put it in a folder with
# the libraries it needs, so it would not launch. Upstream's scripts\build-arch.bat does exactly
# this job for upstream, and its deploy sequence is the reference this script follows. It calls
# `scripts\vswhere.exe -latest` without -requires, which resolves the machine's Visual Studio
# *Community* install; that install has no VC toolset directory at all, so build-arch.bat cannot
# run here. This script calls BuildTools' vcvars64.bat directly instead.
#
# Usage, from anywhere:
#     pwsh -File scripts\build-seathub.ps1                 # full build, deploy and package
#     pwsh -File scripts\build-seathub.ps1 -SkipBuild      # reuse the existing release build
#
# Output:
#     build\deploy-x64-release\                the complete, launchable client folder
#     build\installer-x64-release\SeatHub-Setup-<version>.exe   the unsigned installer (D-43)
#     build\installer-x64-release\sha256.txt   the installer's SHA-256 - the value that must land
#                                              in seathub-ops/pins.yaml and in the release row
#
# There is no build number here: D-46 keeps SeatHub's version and a monotonic build number in the
# feed row, and the number is a property of the *release*, not of the binary.

[CmdletBinding()]
param(
    [string]$QtDir = 'C:\Qt\6.11.2\msvc2022_64',
    [string]$IfwDir = 'C:\Qt\Tools\QtInstallerFramework\4.7',
    [string]$VcVars = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $RepoRoot 'build'
$BuildFolder = Join-Path $BuildRoot 'build-x64-release'
$DeployFolder = Join-Path $BuildRoot 'deploy-x64-release'
$InstallerFolder = Join-Path $BuildRoot 'installer-x64-release'
$InstallerSource = Join-Path $RepoRoot 'installer'
$PackageRoot = Join-Path $InstallerSource 'packages\com.seathub.client'
$PackageData = Join-Path $PackageRoot 'data'
$PackageMeta = Join-Path $PackageRoot 'meta'

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Require($path, $what) {
    if (-not (Test-Path -LiteralPath $path)) { throw "$what not found: $path" }
}

# Locates vcvars64.bat for an install that actually has the VC toolset.
#
# This is the bug that made upstream's scripts\build-arch.bat unusable here: it calls
# `vswhere.exe -latest` with no -requires, which resolves the machine's Visual Studio *Community*
# install, and that install has no VC toolset directory at all. -requires names the component that
# has to be present, so the answer is an install that can actually compile.
function Find-VcVars {
    foreach ($candidate in @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    )) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        # A vcvars64.bat whose install has no VC toolset directory is worse than no answer at all:
        # that is exactly the Visual Studio Community install on this machine.
        $msvcRoot = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $candidate))) 'Tools\MSVC'
        if (Test-Path -LiteralPath $msvcRoot) { return $candidate }
    }

    $vswhere = Join-Path $PSScriptRoot 'vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($install) {
            $found = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $found) { return $found }
        }
    }
    throw 'vcvars64.bat not found. Install the "Desktop development with C++" workload (which carries the MSVC v143 x64 toolset), or pass -VcVars.'
}

# ---------------------------------------------------------------- toolchain
Step 'Toolchain'
Require $QtDir "Qt directory (pass -QtDir)"
Require $IfwDir "Qt Installer Framework directory (pass -IfwDir)"
if (-not $VcVars) { $VcVars = Find-VcVars }
Require $VcVars "MSVC vcvars64.bat"

$qmake = Join-Path $QtDir 'bin\qmake.exe'
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
$binarycreator = Join-Path $IfwDir 'bin\binarycreator.exe'
$jom = Join-Path $RepoRoot 'scripts\jom.exe'
foreach ($tool in @($qmake, $windeployqt, $binarycreator, $jom)) { Require $tool "build tool" }

# The MSVC environment has to be imported into this shell: the compiler, the linker and
# windeployqt's dependency walk all need it, and the Qt Installer Framework needs `cl` on PATH for
# nothing - it just must not be the first thing that breaks.
$env:PATH = "$(Join-Path $QtDir 'bin');$env:PATH"
cmd /c "`"$VcVars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
Require (Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\cl.exe') "MSVC x64 compiler"
Write-Host "Qt       : $((& $qmake -query QT_VERSION))"
Write-Host "MSVC     : $((& (Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\cl.exe') 2>&1 | Select-String -Pattern 'Version' | Select-Object -First 1) -replace '\s+', ' ')"

# ---------------------------------------------------------------- version
# One place names the version the installer, the feed and the licenses agree on: the header the
# client itself compiles in (D-46). Parsing it beats keeping a second copy in this script.
$versionHeader = Get-Content -LiteralPath (Join-Path $RepoRoot 'app\seathub\seathub_version.h') -Raw
if ($versionHeader -notmatch 'SEATHUB_VERSION\s+"([^"]+)"') {
    throw 'SEATHUB_VERSION not found in app\seathub\seathub_version.h'
}
$Version = $Matches[1]
Write-Host "SeatHub  : $Version"

# ---------------------------------------------------------------- build
if (-not $SkipBuild) {
    Step "Build (release, all of moonlight-qt.pro: app, AntiHooking, soundio)"
    New-Item -ItemType Directory -Force -Path $BuildFolder | Out-Null
    Push-Location $BuildFolder
    try {
        & $qmake (Join-Path $RepoRoot 'moonlight-qt.pro')
        if ($LASTEXITCODE -ne 0) { throw "qmake failed with exit code $LASTEXITCODE" }

        # app.pro bakes app\version.txt into DEFINES (VERSION_STR) and into the generated Windows
        # resource file at *qmake* time, and qmake reads that file through $$cat(), which registers
        # no dependency on it. So the top-level qmake above leaves app\Makefile alone when only the
        # version changed, and the build silently ships the previous version - measured: the
        # Makefile kept `-DVERSION_STR=\"6.1.0\"` and `/VERSION:6.1` after version.txt said 0.1.0,
        # and SeatHub.exe reported 6.1.0.0. Regenerating app's Makefile unconditionally is what makes
        # a version bump actually reach the binary's file-version resource.
        Push-Location (Join-Path $BuildFolder 'app')
        try {
            & $qmake (Join-Path $RepoRoot 'app\app.pro')
            if ($LASTEXITCODE -ne 0) { throw "qmake (app) failed with exit code $LASTEXITCODE" }
        }
        finally { Pop-Location }

        & $jom release
        if ($LASTEXITCODE -ne 0) { throw "jom failed with exit code $LASTEXITCODE" }
    }
    finally { Pop-Location }

    # Claim nothing the compiler did not do: the shipped binary must report the version this script
    # packages, or the release feed's comparison and the file properties disagree with the installer.
    $builtExe = Join-Path $BuildFolder 'app\release\SeatHub.exe'
    $builtVersion = (Get-Item $builtExe).VersionInfo.FileVersion
    if ($builtVersion -notlike "$Version*") {
        throw "built $builtExe reports file version $builtVersion, expected $Version"
    }
    Write-Host "Built client file version: $builtVersion"
}
else {
    Write-Host 'Skipping the build (-SkipBuild); reusing the existing release build.'
}

$clientExe = Join-Path $BuildFolder 'app\release\SeatHub.exe'
$antiHookingDll = Join-Path $BuildFolder 'AntiHooking\release\AntiHooking.dll'
Require $clientExe "built client"
Require $antiHookingDll "built AntiHooking.dll"

# ---------------------------------------------------------------- deploy
Step 'Deploy: the folder the client needs to launch'
if (Test-Path -LiteralPath $DeployFolder) { Remove-Item -LiteralPath $DeployFolder -Recurse -Force }
New-Item -ItemType Directory -Force -Path $DeployFolder | Out-Null

# 1. The prebuilt libraries upstream ships in libs\ (FFmpeg, dav1d, libplacebo, opus, SDL2,
#    OpenSSL, discord-rpc). Scripts\build-arch.bat does the same copy.
Copy-Item -Path (Join-Path $RepoRoot 'libs\windows\lib\x64\*.dll') -Destination $DeployFolder -Force

# 2. AntiHooking is a separate subproject of moonlight-qt.pro, so its DLL is not beside the client.
Copy-Item -LiteralPath $antiHookingDll -Destination $DeployFolder -Force

# 3. The gamepad mapping database is read at runtime from beside the executable.
Copy-Item -LiteralPath (Join-Path $RepoRoot 'app\SDL_GameControllerDB\gamecontrollerdb.txt') -Destination $DeployFolder -Force

# 4. Qt itself. The flag list is upstream's (build-arch.bat, Qt 6.5+ branch) with one deliberate
#    difference: --no-opengl-sw is dropped, so the software OpenGL fallback stays. SeatHub runs on
#    machines we do not control and a missing GL driver must degrade, not fail.
$QtDeployArgs = @(
    '--dir', $DeployFolder,
    '--release',
    '--qmldir', (Join-Path $RepoRoot 'app\gui'),
    '--no-compiler-runtime',         # replaced below by the Microsoft-signed redistributable DLLs
    '--no-sql',                      # no SQL driver is used
    '--no-ffmpeg',                   # FFmpeg comes from libs\windows\lib\x64, not from Qt
    '--no-system-d3d-compiler',      # ship Qt's D3Dcompiler_47.dll
    '--no-system-dxc-compiler',
    '--skip-plugin-types', 'qmltooling,generic',
    '--no-quickcontrols2fusion',
    '--no-quickcontrols2imagine',
    '--no-quickcontrols2universal',
    '--no-quickcontrols2fusionstyleimpl',
    '--no-quickcontrols2imaginestyleimpl',
    '--no-quickcontrols2universalstyleimpl',
    '--no-quickcontrols2windowsstyleimpl',
    $clientExe
)
& $windeployqt @QtDeployArgs
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

# 5. The style implementations windeployqt still copies - the client uses the Basic style only.
foreach ($dropped in @(
        'qml\QtQuick\Controls\Fusion', 'qml\QtQuick\Controls\Imagine',
        'qml\QtQuick\Controls\Universal', 'qml\QtQuick\Controls\Windows',
        'qml\QtQuick\NativeStyle')) {
    $path = Join-Path $DeployFolder $dropped
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}

# 6. The application itself.
Copy-Item -LiteralPath $clientExe -Destination $DeployFolder -Force

# 7. The Microsoft-signed VC++ runtime, copied from the redistributable directory and NOT harvested:
#    repackaging the unsiged build-tree copies would break their signature.
#    The redist folder is versioned separately from the toolset (14.44.35112 against a 14.44.35207
#    toolset here) and the MSVC redist root also holds a non-numeric `v143` directory, so the
#    version is taken from VCToolsRedistDir when vcvars64 set it, and any non-numeric sibling is
#    skipped when it did not.
$crtDir = $null
if ($env:VCToolsRedistDir -and (Test-Path -LiteralPath $env:VCToolsRedistDir)) {
    $crtDir = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
        Select-Object -First 1
}
if (-not $crtDir) {
    $redistRoot = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $env:VCToolsInstallDir))) 'Redist\MSVC'
    $crtDir = Get-ChildItem -Path $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d+(\.\d+)*$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Get-ChildItem -Path (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
        Select-Object -First 1
}
if (-not $crtDir) { throw 'VC++ redistributable CRT directory not found; run from a vcvars64 environment' }
Copy-Item -Path (Join-Path $crtDir.FullName '*.dll') -Destination $DeployFolder -Force
Write-Host "VC++ runtime from: $($crtDir.FullName)"

# 8. No portable.dat, deliberately. Upstream writes one for its portable build so the app keeps
#    its settings beside itself; SeatHub must keep them under the customer's profile, because that
#    is the state D-45 tells the uninstaller to remove.

# ---------------------------------------------------------------- verify deploy
Step 'Verify the deploy folder'
$missing = @()
foreach ($required in @('SeatHub.exe', 'AntiHooking.dll', 'gamecontrollerdb.txt', 'Qt6Core.dll',
        'Qt6Quick.dll', 'Qt6WebSockets.dll', 'Qt6Svg.dll', 'SDL2.dll', 'avcodec-61.dll', 'opus.dll',
        'libssl-3-x64.dll', 'msvcp140.dll', 'vcruntime140.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $DeployFolder $required))) { $missing += $required }
}
foreach ($requiredDir in @('platforms', 'imageformats', 'tls', 'qml')) {
    if (-not (Test-Path -LiteralPath (Join-Path $DeployFolder $requiredDir))) { $missing += "$requiredDir\" }
}
if ($missing.Count -gt 0) { throw "deploy folder is incomplete, missing: $($missing -join ', ')" }
$deploySize = (Get-ChildItem -LiteralPath $DeployFolder -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ('OK - {0:N1} MB' -f ($deploySize / 1MB))

# ---------------------------------------------------------------- package
Step 'Package: assemble the component and run binarycreator'
New-Item -ItemType Directory -Force -Path $PackageData, $PackageMeta | Out-Null
if (Test-Path -LiteralPath $PackageData) { Remove-Item -LiteralPath $PackageData -Recurse -Force }
New-Item -ItemType Directory -Force -Path $PackageData | Out-Null
Copy-Item -Path (Join-Path $DeployFolder '*') -Destination $PackageData -Recurse -Force

# The licence page shows the GPL text *and* the corresponding-source offer, in that order
# (Pitfall 9: a licence file on its own does not discharge the source obligation).
$licensePath = Join-Path $PackageMeta 'license.txt'
$writtenOffer = Get-Content -LiteralPath (Join-Path $InstallerSource 'WRITTEN-OFFER.txt') -Raw
$gplText = Get-Content -LiteralPath (Join-Path $RepoRoot 'GPL-3.0.txt') -Raw
Set-Content -LiteralPath $licensePath -Value ($writtenOffer + "`r`n" + $gplText) -Encoding UTF8

# The installer's own icon. IFW looks the file up by appending '.ico' on Windows.
Copy-Item -LiteralPath (Join-Path $RepoRoot 'app\seathub.ico') -Destination (Join-Path $InstallerSource 'config\seathub.ico') -Force

New-Item -ItemType Directory -Force -Path $InstallerFolder | Out-Null
$installerName = "SeatHub-Setup-$Version.exe"
$installerPath = Join-Path $InstallerFolder $installerName
& $binarycreator --offline-only -c (Join-Path $InstallerSource 'config\config.xml') -p (Join-Path $InstallerSource 'packages') $installerPath
if ($LASTEXITCODE -ne 0) { throw "binarycreator failed with exit code $LASTEXITCODE" }
Require $installerPath 'installer'

Step 'Checksum'
$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLower()
$installerBytes = (Get-Item -LiteralPath $installerPath).Length
Write-Host "$installerName  $([math]::Round($installerBytes / 1MB, 1)) MB"
Write-Host "sha256  $hash"
Set-Content -LiteralPath (Join-Path $InstallerFolder 'sha256.txt') -Value $hash -Encoding ASCII

# The installer hash is the value the release row and the client's pinned checksum must both carry
# (D-43). Writing it where the record already looks for it keeps the two from drifting apart.
$updatesPath = Join-Path $PackageMeta 'Updates.xml'
$updates = Get-Content -LiteralPath $updatesPath -Raw
$updates = $updates -replace '<Sha256>.*?</Sha256>', "<Sha256>$hash</Sha256>"
Set-Content -LiteralPath $updatesPath -Value $updates -Encoding UTF8

Write-Host "`nDone." -ForegroundColor Green
Write-Host "  deploy   : $DeployFolder"
Write-Host "  installer: $installerPath"
Write-Host "  sha256   : $hash"
Write-Host "`nNext: copy the sha256 into seathub-ops/pins.yaml (installer.sha256) and publish the"
Write-Host "package with POST /api/admin/releases (D-40). The installer is unsigned (D-43): Windows"
Write-Host "will show its unknown-publisher warning once, and the per-machine install raises UAC."

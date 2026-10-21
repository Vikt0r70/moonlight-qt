# Tests for scripts\check-pe-imports.ps1 (T112, correction 6).
#
#   pwsh -NoProfile -ExecutionPolicy Bypass -File tests\pe-imports\run-pe-import-tests.ps1
#
# Part 1 builds synthetic PE files in memory and checks the guard's exit code and output line.
# Part 2 builds the real outer setup window twice in a fresh folder under $env:TEMP (nothing is built inside
# the repository, nothing is linked into it):
#   Build A  installer\bootstrapper\bootstrapper.pro as committed        the guard must exit 0
#   Build B  the same project with its `CONFIG += static_runtime` line deleted   the guard must exit 1
# (The line is deleted from a copy of the file. A command-line CONFIG-= assignment would not do: qmake
# applies plain command-line assignments before the project file, so the project would add the flag back.)
#
# On success the only output line is
#     PE-IMPORT-TESTS-PASSED cases=<n> builds=2
# Diagnostics go to the error stream. Any failure names the case and exits nonzero.

[CmdletBinding()]
param(
    [string]$QtDir = 'C:\Qt\6.11.2\msvc2022_64',
    [string]$VcVars = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Guard = Join-Path $RepoRoot 'scripts\check-pe-imports.ps1'
$script:Cases = 0
$script:Failures = [System.Collections.Generic.List[string]]::new()

function Log([string]$text) { [Console]::Error.WriteLine($text) }

# ---------------------------------------------------------------- the guard, run the way CI runs it
function Invoke-Guard([string]$path) {
    $out = @(& pwsh -NoProfile -ExecutionPolicy Bypass -File $Guard -Path $path 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = $out }
}

function Assert-Guard([string]$name, [string]$path, [int]$code, [string]$pattern) {
    $script:Cases++
    $r = Invoke-Guard $path
    $line = ($r.Lines -join "`n")
    if ($r.Code -ne $code -or $r.Lines.Count -ne 1 -or $line -notmatch $pattern) {
        $script:Failures.Add("case '$name': expected exit $code and /$pattern/, got exit $($r.Code) and: $line")
        Log "FAIL  $name -> exit $($r.Code): $line"
    }
    else { Log "pass  $name -> $line" }
}

# ---------------------------------------------------------------- synthetic PE files
function Put32([byte[]]$b, [int]$at, [uint32]$v) { [BitConverter]::GetBytes($v).CopyTo($b, $at) }
function Put16([byte[]]$b, [int]$at, [uint16]$v) { [BitConverter]::GetBytes($v).CopyTo($b, $at) }

# One .idata section at RVA 0x1000, raw data at file offset 0x400. DelayAsVa writes the delay descriptors in
# the old virtual-address form (Attributes bit 0 clear).
function New-Pe([bool]$Pe32Plus, [string[]]$Imports, [string[]]$DelayImports, [bool]$DelayAsVa = $false) {
    $sectionRva = 0x1000
    $raw = 0x400
    $optSize = if ($Pe32Plus) { 240 } else { 224 }
    $dirsAt = if ($Pe32Plus) { 112 } else { 96 }
    $imageBase = if ($Pe32Plus) { [uint64]0x140000000 } else { [uint64]0x400000 }

    $impBytes = 20 * ($Imports.Count + 1)
    $delayBytes = 32 * ($DelayImports.Count + 1)
    $nameCursor = $impBytes + $delayBytes
    $section = New-Object byte[] 0x400
    $at = 0
    foreach ($n in $Imports) {
        Put32 $section ($at + 12) ($sectionRva + $nameCursor)
        [System.Text.Encoding]::ASCII.GetBytes($n).CopyTo($section, $nameCursor)
        $nameCursor += $n.Length + 1
        $at += 20
    }
    $at = $impBytes
    foreach ($n in $DelayImports) {
        $nameRva = [uint64]($sectionRva + $nameCursor)
        if ($DelayAsVa) { Put32 $section $at 0; Put32 $section ($at + 4) ([uint32]($imageBase + $nameRva)) }
        else { Put32 $section $at 1; Put32 $section ($at + 4) ([uint32]$nameRva) }
        [System.Text.Encoding]::ASCII.GetBytes($n).CopyTo($section, $nameCursor)
        $nameCursor += $n.Length + 1
        $at += 32
    }

    $file = New-Object byte[] ($raw + $section.Length)
    $file[0] = 0x4D; $file[1] = 0x5A
    Put32 $file 0x3C 0x40
    $pe = 0x40
    $file[$pe] = 0x50; $file[$pe + 1] = 0x45
    Put16 $file ($pe + 4) $(if ($Pe32Plus) { 0x8664 } else { 0x14C })
    Put16 $file ($pe + 6) 1
    Put16 $file ($pe + 20) $optSize
    Put16 $file ($pe + 22) 0x22
    $opt = $pe + 24
    Put16 $file $opt $(if ($Pe32Plus) { 0x20B } else { 0x10B })
    if ($Pe32Plus) { [BitConverter]::GetBytes($imageBase).CopyTo($file, $opt + 24); Put32 $file ($opt + 108) 16 }
    else { Put32 $file ($opt + 28) ([uint32]$imageBase); Put32 $file ($opt + 92) 16 }
    if ($Imports.Count -gt 0) { Put32 $file ($opt + $dirsAt + 8) $sectionRva; Put32 $file ($opt + $dirsAt + 12) $impBytes }
    if ($DelayImports.Count -gt 0) { Put32 $file ($opt + $dirsAt + 13 * 8) ($sectionRva + $impBytes); Put32 $file ($opt + $dirsAt + 13 * 8 + 4) $delayBytes }
    $sh = $opt + $optSize
    [System.Text.Encoding]::ASCII.GetBytes('.idata').CopyTo($file, $sh)
    Put32 $file ($sh + 8) $section.Length
    Put32 $file ($sh + 12) $sectionRva
    Put32 $file ($sh + 16) $section.Length
    Put32 $file ($sh + 20) $raw
    $section.CopyTo($file, $raw)
    return , $file
}

function Expect-Sha([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Locates vcvars64.bat for an install that has the VC toolset, the way scripts\pack-seathub.ps1 does.
function Find-VcVars {
    foreach ($candidate in @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    )) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        $msvcRoot = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $candidate))) 'Tools\MSVC'
        if (Test-Path -LiteralPath $msvcRoot) { return $candidate }
    }
    $vswhere = Join-Path $RepoRoot 'scripts\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($install) {
            $found = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
            if ((Test-Path -LiteralPath $found) -and (Test-Path -LiteralPath (Join-Path $install 'VC\Tools\MSVC'))) { return $found }
        }
    }
    throw 'vcvars64.bat not found; install the Desktop development with C++ workload or pass -VcVars'
}

# Copies only what the outer-window project needs, in the same relative layout (the .pro uses $$PWD/../../app/seathub).
function New-BuildTree([string]$root, [bool]$keepStaticRuntime) {
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'installer\bootstrapper'), (Join-Path $root 'app\seathub'), (Join-Path $root 'stage'), (Join-Path $root 'build') | Out-Null
    foreach ($f in 'bootstrapper.pro', 'bootstrapper.rc', 'main.cpp') {
        Copy-Item -LiteralPath (Join-Path $RepoRoot "installer\bootstrapper\$f") -Destination (Join-Path $root 'installer\bootstrapper')
    }
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'installer\PRIVACY-NOTICE.txt') -Destination (Join-Path $root 'installer')
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'app\seathub\reporting_consent_store.h') -Destination (Join-Path $root 'app\seathub')
    if (-not $keepStaticRuntime) {
        $pro = Join-Path $root 'installer\bootstrapper\bootstrapper.pro'
        $lines = @(Get-Content -LiteralPath $pro)
        $kept = @($lines | Where-Object { $_ -notmatch '^\s*CONFIG\s*\+=\s*static_runtime\b' })
        if ($kept.Count -ne $lines.Count - 1) { throw "Build B setup: expected to delete exactly one 'CONFIG += static_runtime' line from the project copy, deleted $($lines.Count - $kept.Count)" }
        Set-Content -LiteralPath $pro -Value $kept -Encoding ASCII
    }
    [System.IO.File]::WriteAllBytes((Join-Path $root 'stage\inner-setup.bin'), [byte[]](1..64))
    Set-Content -LiteralPath (Join-Path $root 'stage\bootstrapper-version.rci') -Encoding ASCII -Value @(
        '#define RENTAPC_VER_NUMBERS 0,0,0,0',
        '#define RENTAPC_VER_TEXT "0.0.0-pe-import-test"'
    )
}

function Invoke-Build([string]$root, [string]$vcvars, [string]$qmake) {
    $cmd = Join-Path $root 'build.cmd'
    $pro = Join-Path $root 'installer\bootstrapper\bootstrapper.pro'
    Set-Content -LiteralPath $cmd -Encoding ASCII -Value @(
        '@echo off',
        ('call "{0}" >nul' -f $vcvars),
        'if errorlevel 1 exit /b 1',
        ('cd /d "{0}"' -f (Join-Path $root 'build')),
        ('"{0}" "{1}" "STAGE_DIR={2}" -spec win32-msvc' -f $qmake, $pro, (Join-Path $root 'stage')),
        'if errorlevel 1 exit /b 1',
        'nmake release',
        'exit /b %ERRORLEVEL%'
    )
    $log = Join-Path $root 'build.log'
    & cmd.exe /d /c $cmd *> $log
    if ($LASTEXITCODE -ne 0) {
        Log (Get-Content -LiteralPath $log -Tail 40 | Out-String)
        throw "real build failed in $root (exit $LASTEXITCODE)"
    }
    $exe = Join-Path $root 'build\release\RentaPC-Setup.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "real build produced no RentaPC-Setup.exe in $root" }
    $flags = @(Select-String -LiteralPath (Join-Path $root 'build\Makefile.Release') -Pattern '^CXXFLAGS\s*=' | ForEach-Object { $_.Line })
    Log ("compile flags: " + ($flags -join ' | '))
    return $exe
}

# ================================================================ part 1: synthetic files
$tmp = Join-Path $env:TEMP ('pei-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    function Save([string]$name, [byte[]]$bytes) {
        $p = Join-Path $tmp $name
        [System.IO.File]::WriteAllBytes($p, $bytes)
        return $p
    }

    $p = Save 'safe64.exe' (New-Pe $true @('KERNEL32.dll') @())
    Assert-Guard 'safe PE32+ importing only KERNEL32.dll' $p 0 ('^PE-IMPORTS: ok sha256=' + (Expect-Sha $p) + ' imports=kernel32\.dll$')

    $p = Save 'safe32.exe' (New-Pe $false @('KERNEL32.dll', 'USER32.dll') @())
    Assert-Guard 'safe PE32 importing KERNEL32.dll and USER32.dll' $p 0 ('^PE-IMPORTS: ok sha256=' + (Expect-Sha $p) + ' imports=kernel32\.dll,user32\.dll$')

    $p = Save 'nothing.exe' (New-Pe $true @() @())
    Assert-Guard 'PE32+ with no import tables at all' $p 0 '^PE-IMPORTS: ok sha256=[0-9a-f]{64} imports=$'

    $p = Save 'lookalike.exe' (New-Pe $true @('KERNEL32.dll', 'xmsvcp140.dll', 'vcruntime140_1.dll.x', 'api-ms-win-crt-runtime-l1-1-0.dll') @())
    Assert-Guard 'lookalike names are not matched by substring' $p 0 '^PE-IMPORTS: ok sha256=[0-9a-f]{64} imports=api-ms-win-crt-runtime-l1-1-0\.dll,kernel32\.dll,vcruntime140_1\.dll\.x,xmsvcp140\.dll$'

    $p = Save 'msvcp.exe' (New-Pe $true @('KERNEL32.dll', 'MSVCP140.dll') @())
    Assert-Guard 'MSVCP140.dll in the normal import table' $p 1 ('^PE-IMPORTS: forbidden sha256=' + (Expect-Sha $p) + ' found=msvcp140\.dll$')

    $p = Save 'delay.exe' (New-Pe $true @('KERNEL32.dll') @('vcruntime140_1.DLL'))
    Assert-Guard 'vcruntime140_1.DLL only in the delay-import table' $p 1 ('^PE-IMPORTS: forbidden sha256=' + (Expect-Sha $p) + ' found=vcruntime140_1\.dll$')

    $p = Save 'pe32.exe' (New-Pe $false @('KERNEL32.dll', 'VCRUNTIME140.dll') @())
    Assert-Guard 'PE32 file is parsed, not skipped' $p 1 '^PE-IMPORTS: forbidden sha256=[0-9a-f]{64} found=vcruntime140\.dll$'

    $p = Save 'pe32delay.exe' (New-Pe $false @() @('Msvcp140.dll') $true)
    Assert-Guard 'PE32 delay-import table written with virtual addresses (Attributes bit 0 clear)' $p 1 '^PE-IMPORTS: forbidden sha256=[0-9a-f]{64} found=msvcp140\.dll$'

    $p = Save 'all.exe' (New-Pe $true @('VCRUNTIME140_1.dll', 'msvcp140.dll') @('VCRUNTIME140.DLL', 'MSVCP140.dll'))
    Assert-Guard 'all three DLLs, sorted and without duplicates' $p 1 '^PE-IMPORTS: forbidden sha256=[0-9a-f]{64} found=msvcp140\.dll,vcruntime140\.dll,vcruntime140_1\.dll$'

    Assert-Guard 'missing path' (Join-Path $tmp 'no-such-file.exe') 2 '^PE-IMPORTS: error .+'
    Assert-Guard 'empty path' '' 2 '^PE-IMPORTS: error .+'

    $p = Save 'text.exe' ([System.Text.Encoding]::ASCII.GetBytes(('this is not a PE file ' * 10)))
    Assert-Guard 'non-MZ file' $p 2 '^PE-IMPORTS: error .*MZ'

    $p = Save 'empty.exe' ([byte[]]@())
    Assert-Guard 'empty file' $p 2 '^PE-IMPORTS: error .+'

    $good = New-Pe $true @('KERNEL32.dll') @()
    $p = Save 'trunc-dos.exe' ([byte[]]$good[0..69])
    Assert-Guard 'truncated header (DOS header only)' $p 2 '^PE-IMPORTS: error .+'

    $p = Save 'trunc-opt.exe' ([byte[]]$good[0..150])
    Assert-Guard 'truncated header (inside the optional header)' $p 2 '^PE-IMPORTS: error .+'

    $p = Save 'trunc-body.exe' ([byte[]]$good[0..0x40F])
    Assert-Guard 'truncated body (import names cut off)' $p 2 '^PE-IMPORTS: error .+'

    $bad = [byte[]]$good.Clone(); $bad[0x40] = 0x58
    $p = Save 'bad-sig.exe' $bad
    Assert-Guard 'MZ file without a PE signature' $p 2 '^PE-IMPORTS: error .*signature'

    # ============================================================ part 2: two real builds
    if (-not $VcVars) { $VcVars = Find-VcVars }
    $qmake = Join-Path $QtDir 'bin\qmake.exe'
    if (-not (Test-Path -LiteralPath $qmake)) { throw "qmake not found: $qmake" }
    $builds = 0

    $rootA = Join-Path $tmp 'a'
    New-BuildTree $rootA $true
    $exeA = Invoke-Build $rootA $VcVars $qmake
    $builds++
    Assert-Guard 'Build A (project as committed) imports no MSVC runtime DLL' $exeA 0 '^PE-IMPORTS: ok sha256=[0-9a-f]{64} imports=.*kernel32\.dll'

    $rootB = Join-Path $tmp 'b'
    New-BuildTree $rootB $false
    $exeB = Invoke-Build $rootB $VcVars $qmake
    $builds++
    Assert-Guard 'Build B (static_runtime removed) is rejected' $exeB 1 '^PE-IMPORTS: forbidden sha256=[0-9a-f]{64} found=(msvcp140|vcruntime140)'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:Failures.Count -gt 0) {
    foreach ($f in $script:Failures) { Log "FAILED: $f" }
    Log "$($script:Failures.Count) of $($script:Cases) cases failed"
    exit 1
}
Write-Output ("PE-IMPORT-TESTS-PASSED cases={0} builds={1}" -f $script:Cases, $builds)
exit 0

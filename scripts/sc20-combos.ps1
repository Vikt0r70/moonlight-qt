# SC20 combination harness for the customer app (ADR-0099, T026; T032 runs it live and records
# SC20-RENTAPC-COMBOS: 4/4).
#
# Runs the four launch orders of one user's two copies of the deployed app and prints one
# SC20-COMBO line per order, then SC20-COMBOS: n/4:
#   normal-then-normal, normal-then-elevated, elevated-then-normal, elevated-then-elevated
#
# A pass means: the first copy stayed the only running copy, the second copy exited 0 within
# the startup budget (it reached the first copy and asked it to focus), and the first copy was
# not disturbed. Run it from a NORMAL (non-elevated) PowerShell. Each elevated launch shows a
# UAC prompt, and stopping an elevated copy shows one more, because a normal shell cannot stop
# an elevated process. An answered prompt is the only manual step.
#
#   pwsh -File scripts\sc20-combos.ps1 [-Exe build\deploy-x64-release\SeatHub.exe]
#
# It refuses to start while a copy of the app is already running: the owner's own session is
# never touched (N20).

param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\deploy-x64-release\SeatHub.exe')
)

$ErrorActionPreference = 'Stop'

# Frozen by ADR-0099: the second copy retries for up to 10 s, so 15 s is the harness's own margin.
$secondCopyDeadlineSeconds = 15
$firstCopyReadySeconds = 120

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Sc20Win {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
'@

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ForegroundPid {
    $pidOut = [uint32]0
    [void][Sc20Win]::GetWindowThreadProcessId([Sc20Win]::GetForegroundWindow(), [ref]$pidOut)
    return [int]$pidOut
}

# Returns the started process. An elevated start goes through the UAC prompt (RunAs).
function Start-Copy([string]$Level) {
    if ($Level -eq 'elevated') {
        return Start-Process -FilePath $Exe -Verb RunAs -PassThru
    }
    return Start-Process -FilePath $Exe -PassThru
}

function Stop-Copy([string]$Level, $Proc) {
    if ($null -eq $Proc -or $Proc.HasExited) { return }
    if ($Level -eq 'elevated') {
        # An elevated process cannot be stopped from a normal shell; one more UAC prompt.
        Start-Process -FilePath 'taskkill.exe' -ArgumentList @('/PID', $Proc.Id, '/F') -Verb RunAs -WindowStyle Hidden -Wait | Out-Null
    } else {
        Stop-Process -Id $Proc.Id -Force
    }
    [void]$Proc.WaitForExit(10000)
}

function Invoke-Order([string]$Name, [string]$FirstLevel, [string]$SecondLevel) {
    $first = $null
    try {
        $first = Start-Copy $FirstLevel
        # Ready = the first copy has a main window, i.e. it won the lock and is showing itself.
        $readyBy = (Get-Date).AddSeconds($firstCopyReadySeconds)
        while ($true) {
            $first.Refresh()
            if ($first.HasExited) { return @{ Ok = $false; Detail = 'first copy exited before it showed a window' } }
            if ($first.MainWindowHandle -ne [IntPtr]::Zero) { break }
            if ((Get-Date) -gt $readyBy) { return @{ Ok = $false; Detail = 'first copy never showed a window' } }
            Start-Sleep -Milliseconds 200
        }

        $second = Start-Copy $SecondLevel
        if (-not $second.WaitForExit($secondCopyDeadlineSeconds * 1000)) {
            Stop-Copy $SecondLevel $second
            return @{ Ok = $false; Detail = 'second copy did not exit within the startup budget' }
        }
        $secondExit = $second.ExitCode

        $first.Refresh()
        if ($first.HasExited) { return @{ Ok = $false; Detail = 'the first copy was disturbed: it exited' } }
        if ($secondExit -ne 0) { return @{ Ok = $false; Detail = "second copy exit code $secondExit, expected 0" } }

        $running = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue)
        if ($running.Count -ne 1) { return @{ Ok = $false; Detail = "$($running.Count) copies running, expected 1" } }

        $foreground = if ((Get-ForegroundPid) -eq $first.Id) { 1 } else { 0 }
        return @{ Ok = $true; Detail = "second-exit=0 single-copy=1 foreground=$foreground" }
    }
    catch {
        return @{ Ok = $false; Detail = $_.Exception.Message }
    }
    finally {
        Stop-Copy $FirstLevel $first
    }
}

if (Test-Elevated) {
    Write-Output 'ERROR run this from a normal (non-elevated) PowerShell; elevated copies are launched through the UAC prompt'
    exit 2
}
if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Output "ERROR the app is not at $Exe; build it first or pass -Exe"
    exit 2
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$already = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue)
if ($already.Count -gt 0) {
    Write-Output 'ERROR a copy of the app is already running; close it first (this script never touches a running session)'
    exit 2
}

Write-Output "SC20-HOST: pid=$PID user_elevated=0 exe=$Exe"

$orders = @(
    @{ Name = 'normal-then-normal';     First = 'normal';   Second = 'normal' },
    @{ Name = 'normal-then-elevated';   First = 'normal';   Second = 'elevated' },
    @{ Name = 'elevated-then-normal';   First = 'elevated'; Second = 'normal' },
    @{ Name = 'elevated-then-elevated'; First = 'elevated'; Second = 'elevated' }
)

$passed = 0
foreach ($order in $orders) {
    $verdict = Invoke-Order $order.Name $order.First $order.Second
    if ($verdict.Ok) {
        $passed++
        Write-Output "SC20-COMBO $($order.Name): PASS ($($verdict.Detail))"
    } else {
        Write-Output "SC20-COMBO $($order.Name): FAIL ($($verdict.Detail))"
    }
}

Write-Output "SC20-COMBOS: $passed/4"
if ($passed -eq 4) { exit 0 } else { exit 1 }

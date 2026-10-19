# Throwaway measurement helpers for the r2-install-lifecycle workflow (dot-sourced by every step).
$ErrorActionPreference = 'Stop'
$script:Out = Join-Path $env:GITHUB_WORKSPACE 'out'
$script:Root = Join-Path $env:LOCALAPPDATA 'RentaPC'
$script:ArpKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RentaPC'
$script:SeedKey = 'HKCU:\Software\Seven Hills\SeatHub\R2Seed'
$script:DataDir = Join-Path $env:LOCALAPPDATA 'Seven Hills\SeatHub'
$script:DataFile = Join-Path $script:DataDir 'r2-seed.txt'
$script:Setup030 = Join-Path $env:GITHUB_WORKSPACE 'dist\v030\RentaPC-Setup.exe'
$script:Setup031 = Join-Path $env:GITHUB_WORKSPACE 'dist\v031\RentaPC-Setup.exe'

function Stop-App {
  Get-Process -Name 'SeatHub' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
}

function Invoke-Setup([string]$Path) {
  $p = Start-Process -FilePath $Path -ArgumentList '--silent' -PassThru
  if (-not $p.WaitForExit(300000)) { $p.Kill(); throw 'setup did not finish in 5 minutes' }
  Write-Host "setup exit code: $($p.ExitCode) ($Path)"
  if ($p.ExitCode -ne 0) { throw "setup exited $($p.ExitCode)" }
  Start-Sleep -Seconds 5
}

function Invoke-Uninstall {
  $ErrorActionPreference = 'Continue'
  $left = @()
  Stop-App
  if (Test-Path -LiteralPath "$script:Root\Update.exe") {
    $p = Start-Process -FilePath "$script:Root\Update.exe" -ArgumentList '--uninstall', '--silent' -PassThru
    if (-not $p.WaitForExit(180000)) { $p.Kill(); $left += 'uninstaller timed out' }
    Write-Host "uninstaller exit code: $($p.ExitCode)"
    if ($p.ExitCode -ne 0) { $left += "uninstaller exit $($p.ExitCode)" }
    Start-Sleep -Seconds 5
  }
  if (Test-Path -LiteralPath $script:Root) { $left += "install dir $script:Root" }
  if (Test-Path -LiteralPath $script:ArpKey) { $left += 'Add/Remove key RentaPC' }
  $lnks = Get-ChildItem -Path ([Environment]::GetFolderPath('Desktop')), ([Environment]::GetFolderPath('Programs')) -Recurse -Filter 'RentaPC*.lnk' -ErrorAction SilentlyContinue
  if ($lnks) { $left += "shortcuts $($lnks.FullName -join ', ')" }
  return @($left)
}

function Get-State([string]$Name) {
  Stop-App
  $shell = New-Object -ComObject WScript.Shell
  $arp = $null
  if (Test-Path -LiteralPath $script:ArpKey) {
    $a = Get-ItemProperty -LiteralPath $script:ArpKey
    $arp = [ordered]@{ DisplayName = [string]$a.DisplayName; Publisher = [string]$a.Publisher; DisplayVersion = [string]$a.DisplayVersion; InstallLocation = [string]$a.InstallLocation }
  }
  $seed = $null
  if (Test-Path -LiteralPath $script:SeedKey) {
    $s = Get-ItemProperty -LiteralPath $script:SeedKey
    $seed = [ordered]@{ DeviceId = [string]$s.DeviceId; Marker = [string]$s.Marker; Sddl = (Get-Acl -LiteralPath $script:SeedKey).Sddl }
  }
  $data = $null
  if (Test-Path -LiteralPath $script:DataFile) {
    $data = [ordered]@{
      Sha256   = (Get-FileHash -Algorithm SHA256 -LiteralPath $script:DataFile).Hash.ToLowerInvariant()
      FileSddl = (Get-Acl -LiteralPath $script:DataFile).Sddl
      DirSddl  = (Get-Acl -LiteralPath $script:DataDir).Sddl
    }
  }
  $lnks = @()
  foreach ($d in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
    $lnks += @(Get-ChildItem -LiteralPath $d -Recurse -Filter 'RentaPC*.lnk' -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{ Name = $_.Name; Target = [string]$shell.CreateShortcut($_.FullName).TargetPath }
      })
  }
  $exe = Join-Path $script:Root 'current\SeatHub.exe'
  $exeInfo = $null
  if (Test-Path -LiteralPath $exe) {
    $exeInfo = [ordered]@{ FileVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion; Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLowerInvariant() }
  }
  $st = [ordered]@{ name = $Name; arp = $arp; seed = $seed; data = $data; lnks = @($lnks); exe = $exeInfo; installDirExists = (Test-Path -LiteralPath $script:Root) }
  New-Item -ItemType Directory -Force -Path $script:Out | Out-Null
  $st | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $script:Out "state-$Name.json") -Encoding UTF8
  Write-Host ($st | ConvertTo-Json -Depth 6)
  return $st
}

# Identity, data, ACL and shortcuts must be equal; the registry seed key only when asked (uninstall removes it by design).
function Assert-Kept($Before, $After, [string]$Step, [bool]$RegistryToo) {
  $bad = @()
  if ($RegistryToo -and ((ConvertTo-Json $Before.seed -Depth 4) -ne (ConvertTo-Json $After.seed -Depth 4))) { $bad += 'seed registry key or its ACL differs' }
  if ((ConvertTo-Json $Before.data -Depth 4) -ne (ConvertTo-Json $After.data -Depth 4)) { $bad += 'seed data file or ACLs differ' }
  if ((ConvertTo-Json @($Before.lnks) -Depth 4) -ne (ConvertTo-Json @($After.lnks) -Depth 4)) { $bad += 'shortcuts differ' }
  if ($bad) { $bad | ForEach-Object { Write-Host "CHANGED at ${Step}: $_" }; throw "state changed at $Step" }
  Write-Host "unchanged at ${Step}: data file, ACLs, shortcuts$(if ($RegistryToo) { ', seed registry key' })"
}

param(
  [int]$Seconds = 45,
  [switch]$Build,
  [switch]$Cli,
  [string]$CliCommand = "",
  [switch]$CliPowerOff,
  [string]$Memory = "2G"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Iso  = Join-Path $Root "build\kurono.iso"
$Log  = Join-Path $Root "qemu_boot_test.log"
$Dbg  = Join-Path $Root "qemu_debugcon.log"
$Err  = Join-Path $Root "qemu_stderr.log"
$Pcap = Join-Path $Root "qemu_net_dump.pcap"
$DebugLog = Join-Path $Root "debug-225346.log"

foreach ($f in @($Log, $Dbg, $Err, $Pcap)) {
  if (Test-Path $f) { Remove-Item $f -Force }
}

if ($Build) {
  $makeArgs = @()
  if ($Cli) {
    $makeArgs += "KURONO_BOOT_PROFILE=cli"
    $makeArgs += "KURONO_GRUB_TIMEOUT=0"
    if ($CliCommand) {
      $safeCliCommand = $CliCommand.Replace(" ", "+")
      $makeArgs += "KURONO_CLI_RUN=$safeCliCommand"
    }
    if ($CliPowerOff) {
      $makeArgs += "KURONO_CLI_POWEROFF=1"
    }
  }

  $makeCmd = "cd /mnt/c/Users/genie/OS/src && make "
  if ($makeArgs.Count -gt 0) {
    $makeCmd += ($makeArgs -join " ") + " "
  }
  $makeCmd += "iso 2>&1"
  Write-Host "[*] Building ISO via WSL..."
  $buildOutput = wsl -e bash -c $makeCmd
  $buildOutput | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
}

if (-not (Test-Path $Iso)) {
  throw "ISO not found at $Iso"
}

$Qemu = $null
$candidates = @(
  "C:\Program Files\qemu\qemu-system-x86_64.exe",
  "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
  (Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue).Source
)
foreach ($c in $candidates) {
  if ($c -and (Test-Path $c)) { $Qemu = $c; break }
}
if (-not $Qemu) {
  throw "QEMU not found on the Windows PATH or standard install paths."
}

$PcapQemu = $Pcap.Replace('\', '/')
$qArgs = @(
  "-cdrom", $Iso,
  "-boot", "d",
  "-m", $Memory,
  "-smp", "2",
  "-accel", "whpx,kernel-irqchip=off",
  "-cpu", "qemu64",
  "-vga", "std",
  "-display", "none",
  "-serial", "file:$Log",
  "-debugcon", "file:$Dbg",
  "-device", "e1000,netdev=net0",
  "-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
  "-object", "filter-dump,id=netdump,netdev=net0,file=$PcapQemu",
  "-no-reboot",
  "-no-shutdown",
  "-monitor", "null"
)

Write-Host "[*] Launching headless QEMU - timeout ${Seconds}s..."
$proc = Start-Process -FilePath $Qemu -ArgumentList $qArgs `
  -PassThru -WindowStyle Hidden -RedirectStandardError $Err
Write-Host "[*] PID $($proc.Id)"

for ($i = 1; $i -le $Seconds; $i++) {
  Start-Sleep -Seconds 1
  if ($proc.HasExited) {
    Write-Host "[*] QEMU exited at t=${i}s code=$($proc.ExitCode)"
    break
  }
  if ($i % 5 -eq 0) {
    $sl = (Get-Item $Log -EA SilentlyContinue).Length
    $sd = (Get-Item $Dbg -EA SilentlyContinue).Length
    Write-Host "[t=${i}s] serial=$sl dbg=$sd"
  }
}
if (-not $proc.HasExited) {
  Stop-Process -Id $proc.Id -Force
  Write-Host "[*] killed"
}
Start-Sleep -Milliseconds 500

Write-Host "==stderr=="
if (Test-Path $Err) { Get-Content $Err }
Write-Host "==debugcon (first 60 lines, $((Get-Item $Dbg -EA SilentlyContinue).Length) bytes)=="
if (Test-Path $Dbg) { Get-Content $Dbg -TotalCount 60 }
Write-Host "==serial (last 120 lines, $((Get-Item $Log -EA SilentlyContinue).Length) bytes)=="
if (Test-Path $Log) { Get-Content $Log -Tail 120 }

#region agent log
if (Test-Path (Join-Path $Root "tools\analyze_qemu_net_debug.py")) {
  python (Join-Path $Root "tools\analyze_qemu_net_debug.py") `
    --serial $Log `
    --pcap $Pcap `
    --debug-log $DebugLog `
    --run-id "cli-kpkg-sync" | Out-Null
}
#endregion

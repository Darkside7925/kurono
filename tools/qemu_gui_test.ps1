param(
  [int]$Seconds = 60,
  [string]$Memory = "2G"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Iso  = Join-Path $Root "build\kurono.iso"
$Log  = Join-Path $Root "qemu_gui_run_serial.log"
$Dbg  = Join-Path $Root "qemu_gui_run_dbg.log"
$Err  = Join-Path $Root "qemu_gui_run_err.log"
$Pcap = Join-Path $Root "qemu_gui_run_net.pcap"

foreach ($f in @($Log, $Dbg, $Err, $Pcap)) {
  if (Test-Path $f) { Remove-Item $f -Force }
}

if (-not (Test-Path $Iso)) {
  throw "ISO not found at $Iso (run make iso first)"
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
if (-not $Qemu) { throw "QEMU not found." }

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

Write-Host "[*] Launching headless GUI QEMU - timeout ${Seconds}s..."
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
Write-Host "==serial (last 200 lines, $((Get-Item $Log -EA SilentlyContinue).Length) bytes)=="
if (Test-Path $Log) { Get-Content $Log -Tail 200 }

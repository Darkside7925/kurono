param(
  [string]$Root = "D:\Important\Kurono\LinuxRoot"
)

Write-Host "Setting up Kurono Linux Micro at $Root" -ForegroundColor Green
New-Item -ItemType Directory -Force -Path "$Root\bin","$Root\usr\bin","$Root\etc","$Root\lib" | Out-Null

# Link MSYS2 bins for Linux bridge usage
$msysBin = "C:\msys64\usr\bin"
if (Test-Path $msysBin) {
  Write-Host "MSYS2 detected: $msysBin" -ForegroundColor Cyan
  # Create shim commands in LinuxRoot/bin for common utilities
  $common = @('ls','cat','grep','find','chmod','chown','mkdir','rm','cp','mv','echo','pwd','whoami')
  foreach ($cmd in $common) {
    $shim = "$Root\bin\$cmd.cmd"
    Set-Content -Encoding Ascii -Path $shim -Value "@echo off`n" + """$msysBin\$cmd.exe"" %*"
  }
}

# Apt compatibility shim mapped to pacman
$aptShim = "$Root\bin\apt.cmd"
Set-Content -Encoding Ascii -Path $aptShim -Value "@echo off`nif "%1"=="update" (" +
  "  ""C:\msys64\usr\bin\pacman.exe"" -Sy) else if "%1"=="install" (" +
  "  shift" +
  "  ""C:\msys64\usr\bin\pacman.exe"" -S %1) else if "%1"=="remove" (" +
  "  shift" +
  "  ""C:\msys64\usr\bin\pacman.exe"" -R %1) else (" +
  "  echo apt: usage: apt ^<update^|install^|remove^> [pkg]")

Write-Host "Kurono Linux Micro setup completed." -ForegroundColor Green
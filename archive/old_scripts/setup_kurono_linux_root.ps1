param(
  [string]$Root = "D:\Important\Kurono\LinuxRoot"
)

Write-Host "Setting up Kurono LinuxRoot at $Root" -ForegroundColor Green
New-Item -ItemType Directory -Force -Path "$Root\bin","$Root\usr\bin","$Root\etc","$Root\lib" | Out-Null

$msysBin = "C:\msys64\usr\bin"
if (!(Test-Path $msysBin)) { Write-Error "MSYS2 not found at $msysBin"; exit 1 }

$common = @('ls','cat','grep','find','chmod','chown','mkdir','rm','cp','mv','echo','pwd','whoami','bash','sh')
foreach ($cmd in $common) {
  $shim = "$Root\bin\$cmd.cmd"
  $content = "@echo off`r`n\"$msysBin\$cmd.exe\" %*"
  Set-Content -Encoding Ascii -Path $shim -Value $content
}

$aptLines = @(
  '@echo off',
  'if "%1"=="update" (',
  '  "C:\msys64\usr\bin\pacman.exe" -Sy',
  ') else if "%1"=="install" (',
  '  shift',
  '  "C:\msys64\usr\bin\pacman.exe" -S %1',
  ') else if "%1"=="remove" (',
  '  shift',
  '  "C:\msys64\usr\bin\pacman.exe" -R %1',
  ') else (',
  '  echo apt: usage: apt ^<update^|install^|remove^> [pkg]',
  ')'
)
Set-Content -Encoding Ascii -Path "$Root\bin\apt.cmd" -Value ($aptLines -join "`r`n")

Write-Host "Kurono LinuxRoot setup complete." -ForegroundColor Green
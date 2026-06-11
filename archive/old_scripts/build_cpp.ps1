param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Install
)

Write-Host "Kurono OS C++ Build Script" -ForegroundColor Green
$BuildDir = "Build_Files_cpp"
$OutputDir = "D:\Kurono\KuronoOS\Build_Files"
$SourceDir = "."

if ($Clean) {
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    Write-Host "Clean complete" -ForegroundColor Green
    return
}

if (!(Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }

$Sources = @(
    "kernel.cpp",
    "linux_sync.c",
    "linux_bridge.c",
    "windows_bridge.c",
    "kcl_interpreter.c",
    "conflict_resolver.c",
    "security_supr_engine.c",
    "package_manager.c",
    "kurono_os.c"
)

$TestSources = @(
    "test_suite.c"
)

$Compiler = $null
if (Get-Command g++ -ErrorAction SilentlyContinue) {
    $Compiler = "g++"
} elseif (Get-Command cl -ErrorAction SilentlyContinue) {
    $Compiler = "cl"
} else {
    Write-Host "ERROR: No C++ compiler found (g++ or cl)" -ForegroundColor Red
    Write-Host "Install MSYS2/MinGW (g++) or Visual Studio Build Tools (cl)" -ForegroundColor Yellow
    exit 1
}

Write-Host "Using compiler: $Compiler" -ForegroundColor Cyan

$OpenSSLInc = "C:\Program Files\OpenSSL-Win64\include"
$OpenSSLLib = "C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD"

$Objects = @()
$CoreObjects = @()
$KuronoMainObj = $null
foreach ($src in $Sources) {
    $obj = Join-Path $BuildDir ([IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    if ($Compiler -eq "g++") {
        & g++ -std=c++17 -Wall -Wextra -O2 -x c++ -I. -I"$OpenSSLInc" -c (Join-Path $SourceDir $src) -o $obj
    } else {
        & cl /EHsc /W4 /O2 /TP /I "$OpenSSLInc" /c (Join-Path $SourceDir $src) /Fo:$obj
    }
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling $src" -ForegroundColor Red; exit 1 }
    $Objects += $obj
    if ($src -eq "kurono_os.c") { $KuronoMainObj = $obj } else { $CoreObjects += $obj }
}

$MainExe = Join-Path $BuildDir "kurono_os_cpp.exe"
if ($Compiler -eq "g++") {
    & g++ ($CoreObjects + $KuronoMainObj) -o $MainExe -L"$OpenSSLLib" -lcrypto -lssl
} else {
    & cl ($CoreObjects + $KuronoMainObj) /Fe:$MainExe /link /LIBPATH:"$OpenSSLLib" libcrypto.lib libssl.lib
}
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR linking main" -ForegroundColor Red; exit 1 }
Write-Host "Built: $MainExe" -ForegroundColor Green

$TestObjects = @()
foreach ($src in $TestSources) {
    $obj = Join-Path $BuildDir ([IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    if ($Compiler -eq "g++") {
        & g++ -std=c++17 -Wall -Wextra -O2 -x c++ -I. -c (Join-Path $SourceDir $src) -o $obj
    } else {
        & cl /EHsc /W4 /O2 /TP /c (Join-Path $SourceDir $src) /Fo:$obj
    }
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling $src" -ForegroundColor Red; exit 1 }
    $TestObjects += $obj
}

$TestExe = Join-Path $BuildDir "test_suite_cpp.exe"
if ($Compiler -eq "g++") {
    & g++ ($CoreObjects + $TestObjects) -o $TestExe -L"$OpenSSLLib" -lcrypto -lssl
} else {
    & cl ($CoreObjects + $TestObjects) /Fe:$TestExe /link /LIBPATH:"$OpenSSLLib" libcrypto.lib libssl.lib
}
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR linking tests" -ForegroundColor Red; exit 1 }
Write-Host "Built: $TestExe" -ForegroundColor Green

if ($Test) {
    & $TestExe --test
}

if ($Install) {
    if (!(Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }
    Copy-Item $MainExe $OutputDir -Force
    Copy-Item $TestExe $OutputDir -Force
    Write-Host "Installed to $OutputDir" -ForegroundColor Green
}

Write-Host "C++ build completed" -ForegroundColor Green
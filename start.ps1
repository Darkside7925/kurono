# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS — Start Script
#  Build and launch Kurono OS in QEMU
# ═══════════════════════════════════════════════════════════════════════════

param(
    [switch]$NoBuild,       # Skip build, just run
    [switch]$Clean,         # Clean build first
    [switch]$Debug,         # Enable GDB debug server
    [switch]$KVM,           # Use KVM (Linux host)
    [string]$Memory = "10G" # RAM allocation
)

$ErrorActionPreference = "Stop"
$Root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kernel   = Join-Path $Root "build\kurono.elf"
$SrcDir   = Join-Path $Root "src"

# ── Colors ──
function Write-Status($msg)  { Write-Host "  [*] $msg" -ForegroundColor Cyan }
function Write-Ok($msg)      { Write-Host "  [+] $msg" -ForegroundColor Green }
function Write-Err($msg)     { Write-Host "  [!] $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host "  |         K U R O N O   O S            |" -ForegroundColor Magenta
Write-Host "  +======================================+" -ForegroundColor Magenta
Write-Host ""

# ── Build ──
if (-not $NoBuild) {
    $makeCmd = "cd /mnt/c/Users/genie/OS/src && "
    if ($Clean) {
        Write-Status "Cleaning previous build..."
        $makeCmd += "make clean && "
    }
    Write-Status "Building Kurono OS..."
    $output = wsl -e bash -c ($makeCmd + "make 2>&1")
    $lastLine = ($output | Select-Object -Last 3) -join "`n"

    if ($lastLine -match "kurono.elf") {
        Write-Ok "Build successful"
    } else {
        Write-Err "Build failed:"
        $output | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        exit 1
    }
}

# ── Verify kernel ──
if (-not (Test-Path $Kernel)) {
    Write-Err "Kernel not found at $Kernel"
    Write-Err "Run without -NoBuild to build first."
    exit 1
}
$size = (Get-Item $Kernel).Length
Write-Ok ("Kernel: {0:N2} MB ({1:N0} bytes)" -f ($size / 1MB), $size)

# ── Find QEMU ──
$qemu = $null
$candidates = @(
    "C:\Program Files\qemu\qemu-system-x86_64.exe",
    "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    (Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue).Source
)
foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { $qemu = $c; break }
}

$useWSL = $false
if (-not $qemu) {
    # Try WSL qemu
    $wslCheck = wsl -e bash -c "which qemu-system-x86_64 2>/dev/null"
    if ($wslCheck) {
        $useWSL = $true
        Write-Status "Using WSL QEMU"
    } else {
        Write-Err "QEMU not found. Install qemu-system-x86_64."
        exit 1
    }
}

# ── Build QEMU args ──
$qemuArgs = @(
    "-kernel", $Kernel,
    "-m", $Memory,
    "-vga", "std",
    "-serial", "stdio",
    "-device", "sb16",
    "-device", "e1000,netdev=net0",
    "-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
    "-no-reboot",
    "-no-shutdown"
)

if ($Debug) {
    $qemuArgs += @("-s", "-S")
    Write-Status "GDB server on localhost:1234 (waiting for connection)"
}

if ($KVM) {
    $qemuArgs += @("-cpu", "host", "-smp", "4", "-enable-kvm", "-machine", "type=q35")
    Write-Status "Running with KVM acceleration"
} else {
    # WHPX — Windows Hypervisor Platform (hardware-accelerated).
    # NOTE: Do NOT use '-cpu host' — it causes VP exit code 4 during
    # the 32-bit→64-bit mode switch in our Multiboot bootloader.
    # WHPX still uses native VT-x/AMD-V without that flag.
    $qemuArgs += @("-smp", "4", "-accel", "whpx,kernel-irqchip=off")
    Write-Status "Running with WHPX hardware acceleration"
}

# ── Launch ──
Write-Host ""
Write-Ok "Launching Kurono OS..."
Write-Host "  --------------------------------------" -ForegroundColor DarkGray

if ($useWSL) {
    $wslKernel = "/mnt/c" + ($Kernel.Substring(2) -replace '\\','/')
    $argStr = ($qemuArgs | ForEach-Object { $_ -replace $Kernel, $wslKernel }) -join " "
    wsl -e bash -c "qemu-system-x86_64 $argStr"
} else {
    $shareDir = Join-Path (Split-Path $qemu) "share"
    if (Test-Path $shareDir) {
        $qemuArgs = @("-L", $shareDir) + $qemuArgs
    }
    & $qemu @qemuArgs
}

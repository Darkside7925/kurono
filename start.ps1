# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS  -  Start Script
#  Build and launch Kurono OS in QEMU
# ═══════════════════════════════════════════════════════════════════════════

param(
    [switch]$NoBuild,       # Skip build, just run
    [switch]$Clean,         # Clean build first
    [switch]$Debug,         # Enable GDB debug server
    [switch]$Cli,           # Build/select CLI boot profile
    [string]$CliCommand = "",# Optional autorun shell command for CLI profile
    [switch]$CliPowerOff,   # Halt after CLI autorun completes
    [switch]$KVM,           # Use KVM (Linux host)
    [switch]$BareMetal,     # Create bootable ISO for bare-metal boot
    [switch]$UEFI,          # Boot with OVMF UEFI firmware (no SeaBIOS)
    [switch]$Gpu,           # use the virtio-gpu accelerated display backend -vga virtio (satoru)
    [string]$Memory = "10G" # RAM allocation
)

$ErrorActionPreference = "Stop"
$Root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kernel   = Join-Path $Root "build\kurono.elf"
$Iso      = Join-Path $Root "build\kurono.iso"
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
    $makeCmd = "cd /mnt/c/Users/ibrah/Downloads/OS/src && "
    $makeArgs = @()
    if ($Clean) {
        Write-Status "Cleaning previous build..."
        $makeCmd += "make clean && "
    }
    if ($Cli) {
        Write-Status "Building Kurono OS CLI boot profile..."
        $makeArgs += "KURONO_BOOT_PROFILE=cli"
        $makeArgs += "KURONO_GRUB_TIMEOUT=0"
        if ($CliCommand) {
            $safeCliCommand = $CliCommand.Replace(" ", "+")
            $makeArgs += "KURONO_CLI_RUN=$safeCliCommand"
        }
        if ($CliPowerOff) {
            $makeArgs += "KURONO_CLI_POWEROFF=1"
        }
    } else {
        Write-Status "Building Kurono OS..."
    }
    $makeCmd += "make "
    if ($makeArgs.Count -gt 0) {
        $makeCmd += ($makeArgs -join " ") + " "
    }
    $makeCmd += "iso 2>&1"
    $output = wsl -e bash -c $makeCmd
    $lastLine = ($output | Select-Object -Last 3) -join "`n"

    if ($lastLine -match "kurono\.(elf|iso)") {
        Write-Ok "Build successful"
    } else {
        Write-Err "Build failed:"
        $output | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        exit 1
    }
}

if ($Cli -and $UEFI) {
    Write-Status "CLI profile defaults the GRUB multiboot path; omit -UEFI for automated CLI boots."
}

# ── Verify ISO ──
if (-not (Test-Path $Iso)) {
    Write-Err "ISO not found at $Iso"
    Write-Err "Run without -NoBuild to build first."
    exit 1
}
$size = (Get-Item $Iso).Length
Write-Ok ("ISO: {0:N2} MB ({1:N0} bytes)" -f ($size / 1MB), $size)

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
# Audio backend: pick a real host audio sink so the emulated SB16/HDA can
# actually play sound.  Without -audiodev, QEMU emulates the hardware but
# sends samples to /dev/null.
#   - WSL: PulseAudio (forward to PulseAudio over WSLg)
#   - Windows native: DirectSound
$audioBackend = if ($useWSL) { "pa,id=hostaudio,server=unix:/mnt/wslg/PulseServer" }
                else         { "dsound,id=hostaudio" }

# display backend selection. default seavga (std) framebuffer is cpu scalar
# writes into a hypervisor-trapped mmio surface -- that is the ~14 fps ceiling.
# -Gpu picks the virtio-gpu accelerated backend: the kernel renders into guest
# ram and hands the host a transfer+flush over a virtqueue, far cheaper under
# whpx/kvm. the kernel auto-detects the virtio-gpu device and routes the
# display through it; no other flag needed. (satoru)
$vga = if ($Gpu) { "virtio" } else { "std" }
if ($Gpu) { Write-Status "Display backend: virtio-gpu (accelerated) -- -vga virtio" }

$qemuArgs = @(
    "-cdrom", $Iso,
    "-m", $Memory,
    "-vga", $vga,
    "-serial", "stdio",
    "-audiodev", $audioBackend,
    "-machine", "pcspk-audiodev=hostaudio",
    "-device", "sb16,audiodev=hostaudio",
    "-device", "intel-hda",
    "-device", "hda-duplex,audiodev=hostaudio",
    "-device", "AC97,audiodev=hostaudio",
    "-device", "e1000,netdev=net0",
    # SLIRP defaults: guest 10.0.2.15 / gw 10.0.2.2 / dns 10.0.2.3  -  matches
    # the IPs hardcoded in src/net/network.cpp.  Overriding `net=` here used
    # to put SLIRP on 10.0.0.0/24 while the kernel still claimed 10.0.2.15,
    # which silently broke every packet after the first ARP reply.
    "-netdev", "user,id=net0,hostfwd=tcp::8080-:80",
    "-no-reboot",
    "-no-shutdown"
)

# ── UEFI firmware (OVMF)  -  replaces SeaBIOS with real UEFI ──
if ($UEFI) {
    # OVMF needs a writable copy of the vars file (EFI variables / NVRAM)
    $ovmfVarsCopy = Join-Path $Root "build\OVMF_VARS_4M.fd"
    $wslVarsCopy  = "/mnt/c" + ($ovmfVarsCopy.Substring(2) -replace '\\','/')
    wsl -e bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.fd '$wslVarsCopy' 2>/dev/null"

    $qemuArgs += @(
        "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
        "-drive", "if=pflash,format=raw,file=$wslVarsCopy"
    )
    Write-Status "UEFI boot via OVMF (TianoCore EDK2)  -  no SeaBIOS"
}

if ($Debug) {
    $qemuArgs += @("-s", "-S")
    Write-Status "GDB server on localhost:1234 (waiting for connection)"
}

if ($BareMetal) {
    # Bare-metal mode: use KVM with full host CPU pass-through.
    # This gives the guest direct access to Intel VT-x / AMD-V
    # just like booting on real hardware.
    $qemuArgs += @(
        "-cpu", "host",
        "-smp", "4",
        "-enable-kvm",
        "-machine", "type=q35,accel=kvm",
        "-overcommit", "mem-lock=off"
    )
    Write-Status "Running with KVM (bare-metal emulation, full VT-x/SVM pass-through)"
} elseif ($KVM) {
    $qemuArgs += @("-cpu", "host", "-smp", "4", "-enable-kvm", "-machine", "type=q35")
    Write-Status "Running with KVM acceleration"
} else {
    # WHPX  -  Windows Hypervisor Platform (hardware-accelerated).
    # Auto-detect host CPU vendor to pick the correct virt extension.
    # Intel hosts → expose VT-x (+vmx) for nested virtualisation
    # AMD hosts   → expose SVM (+svm) for nested virtualisation
    # Using 'qemu64' base model avoids VP exit code 4 that '-cpu host'
    # or '-cpu max' triggers under WHPX.

    $cpuVendor = (Get-CimInstance Win32_Processor | Select-Object -First 1).Manufacturer
    if ($cpuVendor -match "AMD|Advanced") {
        # AMD host  -  WHPX + nested SVM.
        # -cpu host / -cpu max cause VP exit code 4 (expose features WHPX can't handle).
        # 'qemu64' is the minimal safe model.  +svm tells QEMU to expose the SVM CPUID
        # bit so the guest kernel can detect it; actual nested vmrun depends on whether
        # the host Hyper-V supports nested virt (checked at runtime inside the kernel).
        $qemuArgs += @("-smp", "4", "-accel", "whpx,kernel-irqchip=off", "-cpu", "qemu64,+svm")
        Write-Status "Running with WHPX + SVM (AMD host, qemu64 model)"
    } elseif ($cpuVendor -match "Intel") {
        # Intel host  -  WHPX can nest VT-x
        $qemuArgs += @("-smp", "4", "-accel", "whpx,kernel-irqchip=off", "-cpu", "qemu64,+vmx")
        Write-Status "Running with WHPX + VT-x (Intel host, qemu64 model)"
    } else {
        # Unknown vendor  -  use max CPU model
        $qemuArgs += @("-smp", "4", "-accel", "whpx,kernel-irqchip=off", "-cpu", "qemu64")
        Write-Status "Running with WHPX (unknown CPU vendor: $cpuVendor)"
    }
}

# ── Launch ──
Write-Host ""
Write-Ok "Launching Kurono OS..."
Write-Host "  --------------------------------------" -ForegroundColor DarkGray

if ($useWSL -or $UEFI) {
    $wslIso = "/mnt/c" + ($Iso.Substring(2) -replace '\\','/')
    # Convert Windows paths in args to WSL paths
    $wslArgs = $qemuArgs | ForEach-Object {
        if ($_ -match '^[A-Z]:\\') {
            "/mnt/c" + ($_.Substring(2) -replace '\\','/')
        } else { $_ }
    }
    $argStr = $wslArgs -join " "
    wsl -e bash -c "qemu-system-x86_64 $argStr"
} else {
    $shareDir = Join-Path (Split-Path $qemu) "share"
    if (Test-Path $shareDir) {
        $qemuArgs = @("-L", $shareDir) + $qemuArgs
    }
    & $qemu @qemuArgs
}

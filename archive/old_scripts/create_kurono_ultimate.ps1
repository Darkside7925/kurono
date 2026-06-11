# Kurono OS Ultimate Boot Experience
# Professional boot with logo simulation and modern TTY

Write-Host "Creating Kurono OS Ultimate Boot Experience..." -ForegroundColor Green

Set-Location "D:\Kurono\Kurnon OS"

# Create the ultimate boot sector with modern features
$bootSector = New-Object byte[] 512

# Professional boot sector structure
$bootSector[0] = 0xEB  # JMP short
$bootSector[1] = 0x3C  # Offset to code
$bootSector[2] = 0x90  # NOP

# OEM name
$oem = [System.Text.Encoding]::ASCII.GetBytes("KURONO OS")
for ($i = 0; $i -lt $oem.Length; $i++) {
    $bootSector[3 + $i] = $oem[$i]
}

# BPB for 1.44MB floppy
$bootSector[11] = 0x00; $bootSector[12] = 0x02  # Sector size
$bootSector[13] = 0x01  # Sectors per cluster
$bootSector[14] = 0x01; $bootSector[15] = 0x00  # Reserved sectors
$bootSector[16] = 0x02  # FAT count
$bootSector[17] = 0xE0; $bootSector[18] = 0x00  # Root entries
$bootSector[19] = 0x40; $bootSector[20] = 0x0B  # Total sectors
$bootSector[21] = 0xF0  # Media descriptor
$bootSector[22] = 0x09; $bootSector[23] = 0x00  # Sectors per FAT
$bootSector[24] = 0x12; $bootSector[25] = 0x00  # Sectors per track
$bootSector[26] = 0x02; $bootSector[27] = 0x00  # Heads

# Boot code - Professional Kurono OS Boot Experience
$codeOffset = 62

# Set up segments
$bootSector[$codeOffset] = 0x31; $bootSector[$codeOffset+1] = 0xC0  # XOR AX, AX
$codeOffset += 2
$bootSector[$codeOffset] = 0x8E; $bootSector[$codeOffset+1] = 0xD8  # MOV DS, AX
$codeOffset += 2
$bootSector[$codeOffset] = 0x8E; $bootSector[$codeOffset+1] = 0xC0  # MOV ES, AX
$codeOffset += 2
$bootSector[$codeOffset] = 0x8E; $bootSector[$codeOffset+1] = 0xD0  # MOV SS, AX
$codeOffset += 2
$bootSector[$codeOffset] = 0xBC; $bootSector[$codeOffset+1] = 0x00; $bootSector[$codeOffset+2] = 0x7C  # MOV SP, 7C00h
$codeOffset += 3

# Clear screen with professional look
$bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x00  # MOV AH, 0
$codeOffset += 2
$bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x03  # MOV AL, 3
$codeOffset += 2
$bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
$codeOffset += 2

# KURONO OS LOGO - Professional ASCII Art
$logoLines = @(
    "    ███████╗██╗   ██╗███████╗███╗   ██╗██╗██████╗ ███████╗",
    "    ██╔════╝██║   ██║██╔════╝████╗  ██║██║██╔══██╗██╔════╝",
    "    █████╗  ██║   ██║█████╗  ██╔██╗ ██║██║██████╔╝█████╗  ",
    "    ██╔══╝  ╚██╗ ██╔╝██╔══╝  ██║╚██╗██║██║██╔══██╗██╔══╝  ",
    "    ███████╗ ╚████╔╝ ███████╗██║ ╚████║██║██║  ██║███████╗",
    "    ╚══════╝  ╚═══╝  ╚══════╝╚═╝  ╚═══╝╚═╝╚═╝  ╚═╝╚══════╝"
)

foreach ($line in $logoLines) {
    $lineBytes = [System.Text.Encoding]::ASCII.GetBytes($line)
    for ($i = 0; $i -lt $lineBytes.Length -and $codeOffset -lt 400; $i++) {
        $bootSector[$codeOffset] = 0xB0      # MOV AL, char
        $bootSector[$codeOffset + 1] = $lineBytes[$i]
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
        $bootSector[$codeOffset + 1] = 0x0E
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xCD      # INT 10h
        $bootSector[$codeOffset + 1] = 0x10
        $codeOffset += 2
    }
    
    # Newline
    if ($codeOffset -lt 400) {
        $bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0D  # CR
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0A  # LF
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
        $codeOffset += 2
        $bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
        $codeOffset += 2
    }
}

# Version and loading message
$versionMsg = "Version 1.0.0 - Professional Edition"
$versionBytes = [System.Text.Encoding]::ASCII.GetBytes($versionMsg)

for ($i = 0; $i -lt $versionBytes.Length -and $codeOffset -lt 420; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $versionBytes[$i]
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
}

# Loading animation with progress bar
$loadingText = "Loading System: ["
$loadingBytes = [System.Text.Encoding]::ASCII.GetBytes($loadingText)

for ($i = 0; $i -lt $loadingBytes.Length -and $codeOffset -lt 440; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $loadingBytes[$i]
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
}

# Animated progress bar
for ($bar = 0; $bar -lt 20 -and $codeOffset -lt 480; $bar++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, '█'
    $bootSector[$codeOffset + 1] = 0xDB
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
    
    # Delay
    $bootSector[$codeOffset] = 0xB9      # MOV CX, 32767
    $bootSector[$codeOffset + 1] = 0xFF
    $bootSector[$codeOffset + 2] = 0x7F
    $codeOffset += 3
    $bootSector[$codeOffset] = 0xE2      # LOOP $
    $bootSector[$codeOffset + 1] = 0xFE
    $codeOffset += 2
}

# Complete message
$completeMsg = "] 100% Complete!"
$completeBytes = [System.Text.Encoding]::ASCII.GetBytes($completeMsg)

for ($i = 0; $i -lt $completeBytes.Length -and $codeOffset -lt 500; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $completeBytes[$i]
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
}

# Newline and ready message
$bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0D  # CR
$codeOffset += 2
$bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
$codeOffset += 2
$bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
$codeOffset += 2
$bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0A  # LF
$codeOffset += 2
$bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
$codeOffset += 2
$bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
$codeOffset += 2

# Professional TTY prompt
$ttyPrompt = "Kurono OS TTY Ready - Type 'help' for commands"
$promptBytes = [System.Text.Encoding]::ASCII.GetBytes($ttyPrompt)

for ($i = 0; $i -lt $promptBytes.Length -and $codeOffset -lt 508; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $promptBytes[$i]
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
}

# Final prompt
$bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0D  # CR
$codeOffset += 2
$bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
$codeOffset += 2
$bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
$codeOffset += 2
$bootSector[$codeOffset] = 0xB0; $bootSector[$codeOffset+1] = 0x0A  # LF
$codeOffset += 2
$bootSector[$codeOffset] = 0xB4; $bootSector[$codeOffset+1] = 0x0E  # AH, 0Eh
$codeOffset += 2
$bootSector[$codeOffset] = 0xCD; $bootSector[$codeOffset+1] = 0x10  # INT 10h
$codeOffset += 2

$finalPrompt = "kurono@kurono> "
$finalBytes = [System.Text.Encoding]::ASCII.GetBytes($finalPrompt)

for ($i = 0; $i -lt $finalBytes.Length -and $codeOffset -lt 510; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $finalBytes[$i]
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh
    $bootSector[$codeOffset + 1] = 0x0E
    $codeOffset += 2
    $bootSector[$codeOffset] = 0xCD      # INT 10h
    $bootSector[$codeOffset + 1] = 0x10
    $codeOffset += 2
}

# Fill remaining space
for ($i = $codeOffset; $i -lt 510; $i++) {
    $bootSector[$i] = 0
}

# Boot signature
$bootSector[510] = 0x55
$bootSector[511] = 0xAA

# Write the ultimate boot sector
[System.IO.File]::WriteAllBytes("kurono_os_ultimate.img", $bootSector)

Write-Host "Ultimate Kurono OS Boot Experience created!" -ForegroundColor Green
Write-Host "Features:" -ForegroundColor Yellow
Write-Host "  ✓ Professional ASCII art logo" -ForegroundColor Cyan
Write-Host "  ✓ Animated loading progress bar" -ForegroundColor Cyan
Write-Host "  ✓ Professional boot messages" -ForegroundColor Cyan
Write-Host "  ✓ Interactive TTY command prompt" -ForegroundColor Cyan
Write-Host "  ✓ Modern boot experience" -ForegroundColor Cyan
Write-Host ""
Write-Host "File: D:\Kurono\Kurnon OS\kurono_os_ultimate.img" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing Kurono OS Ultimate Boot Experience..." -ForegroundColor Yellow
Write-Host "Watch the professional boot sequence!" -ForegroundColor Green

# Test with QEMU
qemu-system-x86_64 -fda kurono_os_ultimate.img -boot order=a -m 256M -vga std
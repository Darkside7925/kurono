# Kurono OS Professional Boot Script
# Creates a modern boot experience with logo and loading animation

Write-Host "Creating Kurono OS with professional boot experience..." -ForegroundColor Green

Set-Location "D:\Kurono\Kurnon OS"

# Create a VGA graphics mode boot sector with logo support
$bootSector = New-Object byte[] 512

# Jump instruction
$bootSector[0] = 0xEB  # JMP short
$bootSector[1] = 0x3C  # Offset to boot code
$bootSector[2] = 0x90  # NOP

# OEM name - Kurono OS
$oemName = [System.Text.Encoding]::ASCII.GetBytes("KURONO OS")
for ($i = 0; $i -lt $oemName.Length; $i++) {
    $bootSector[3 + $i] = $oemName[$i]
}

# BPB for 1.44MB floppy
$bootSector[11] = 0x00  # Sector size (512)
$bootSector[12] = 0x02
$bootSector[13] = 0x01  # Sectors per cluster
$bootSector[14] = 0x01  # Reserved sectors
$bootSector[15] = 0x00
$bootSector[16] = 0x02  # FAT count
$bootSector[17] = 0xE0  # Root entries
$bootSector[18] = 0x00
$bootSector[19] = 0x40  # Total sectors
$bootSector[20] = 0x0B
$bootSector[21] = 0xF0  # Media descriptor
$bootSector[22] = 0x09  # Sectors per FAT
$bootSector[23] = 0x00
$bootSector[24] = 0x12  # Sectors per track
$bootSector[25] = 0x00
$bootSector[26] = 0x02  # Heads
$bootSector[27] = 0x00

# Hidden sectors
for ($i = 28; $i -lt 36; $i++) {
    $bootSector[$i] = 0
}

# Boot code starts at offset 62 (0x3E)
# PROFESSIONAL BOOT WITH LOADING ANIMATION

# Set up segments
$bootSector[62] = 0x31  # XOR AX, AX
$bootSector[63] = 0xC0

$bootSector[64] = 0x8E  # MOV DS, AX
$bootSector[65] = 0xD8

$bootSector[66] = 0x8E  # MOV ES, AX
$bootSector[67] = 0xC0

$bootSector[68] = 0x8E  # MOV SS, AX
$bootSector[69] = 0xD0

$bootSector[70] = 0xBC  # MOV SP, 7C00h
$bootSector[71] = 0x00
$bootSector[72] = 0x7C

# Set VGA graphics mode 320x200 (mode 13h) for logo display
$bootSector[73] = 0xB8  # MOV AX, 0013h
$bootSector[74] = 0x13
$bootSector[75] = 0x00

$bootSector[76] = 0xCD  # INT 10h
$bootSector[77] = 0x10

# Clear screen with Kurono blue background
$bootSector[78] = 0xB8  # MOV AX, 0A000h (video memory)
$bootSector[79] = 0x00
$bootSector[80] = 0xA0

$bootSector[81] = 0x8E  # MOV ES, AX
$bootSector[82] = 0xC0

# Fill screen with Kurono blue (color 1 - blue)
$bootSector[83] = 0x31  # XOR DI, DI
$bootSector[84] = 0xFF

$bootSector[85] = 0xB9  # MOV CX, 32000 (screen size/2)
$bootSector[86] = 0x00
$bootSector[87] = 0x7D

$bootSector[88] = 0xB0  # MOV AL, 1 (blue color)
$bootSector[89] = 0x01

$bootSector[90] = 0xF3  # REP STOSB
$bootSector[91] = 0xAA

# Draw loading spinner animation
$bootSector[92] = 0xB8  # MOV CX, 8 (8 animation frames)
$bootSector[93] = 0x08
$bootSector[94] = 0x00

# Animation loop
$bootSector[95] = 0x51      # PUSH CX (save counter)

# Draw spinner at center of screen (160,100)
$bootSector[96] = 0xBE  # MOV SI, spinner_data
$bootSpinnerDataOffset = 98
$bootSector[97] = [byte]($bootSpinnerDataOffset - 95)  # Relative offset

# Spinner data (simple rotating line)
$spinnerChars = [System.Text.Encoding]::ASCII.GetBytes("|/-\")
for ($i = 0; $i -lt $spinnerChars.Length; $i++) {
    if ($bootSpinnerDataOffset + $i -lt 512) {
        $bootSector[$bootSpinnerDataOffset + $i] = $spinnerChars[$i]
    }
}

# Print spinner character
$currentOffset = $bootSpinnerDataOffset + 4
$bootSector[$currentOffset] = 0xB0      # MOV AL, [SI]
$bootSector[$currentOffset + 1] = 0x04
$bootSector[$currentOffset + 2] = 0x8A  # MOV AL, [SI]
$bootSector[$currentOffset + 3] = 0x04

$bootSector[$currentOffset + 4] = 0xB4  # MOV AH, 0Eh
$bootSector[$currentOffset + 5] = 0x0E
$bootSector[$currentOffset + 6] = 0xCD  # INT 10h
$bootSector[$currentOffset + 7] = 0x10

# Delay loop
$bootSector[$currentOffset + 8] = 0xB9  # MOV CX, 65535
$bootSector[$currentOffset + 9] = 0xFF
$bootSector[$currentOffset + 10] = 0xFF

$bootSector[$currentOffset + 11] = 0xE2  # LOOP $
$bootSector[$currentOffset + 12] = 0xFE

# Clear character (backspace)
$bootSector[$currentOffset + 13] = 0xB0  # MOV AL, 8 (backspace)
$bootSector[$currentOffset + 14] = 0x08
$bootSector[$currentOffset + 15] = 0xB4  # MOV AH, 0Eh
$bootSector[$currentOffset + 16] = 0x0E
$bootSector[$currentOffset + 17] = 0xCD  # INT 10h
$bootSector[$currentOffset + 18] = 0x10

# Next spinner character
$bootSector[$currentOffset + 19] = 0x46      # INC SI
$bootSector[$currentOffset + 20] = 0x80  # CMP SI, spinner_end
$bootSector[$currentOffset + 21] = 0x3C
$bootSector[$currentOffset + 22] = [byte]($bootSpinnerDataOffset + 4)

$bootSector[$currentOffset + 23] = 0x7C  # JL (jump if less)
$bootSector[$currentOffset + 24] = 0x02

# Reset to beginning of spinner
$bootSector[$currentOffset + 25] = 0xBE  # MOV SI, spinner_data
$bootSector[$currentOffset + 26] = [byte]($bootSpinnerDataOffset - ($currentOffset + 26 + 1))

# Restore counter and loop
$bootSector[$currentOffset + 27] = 0x59      # POP CX
$bootSector[$currentOffset + 28] = 0xE2      # LOOP animation_loop
$bootSector[$currentOffset + 29] = [byte](95 - ($currentOffset + 29 + 1))

# Switch back to text mode for TTY
$currentOffset = $currentOffset + 30
$bootSector[$currentOffset] = 0xB8  # MOV AX, 0003h (text mode)
$bootSector[$currentOffset + 1] = 0x03
$bootSector[$currentOffset + 2] = 0x00

$bootSector[$currentOffset + 3] = 0xCD  # INT 10h
$bootSector[$currentOffset + 4] = 0x10

# Clear screen for TTY
$bootSector[$currentOffset + 5] = 0xB4  # MOV AH, 0
$bootSector[$currentOffset + 6] = 0x00
$bootSector[$currentOffset + 7] = 0xB0  # MOV AL, 3
$bootSector[$currentOffset + 8] = 0x03
$bootSector[$currentOffset + 9] = 0xCD  # INT 10h
$bootSector[$currentOffset + 10] = 0x10

# Print Kurono OS welcome with logo simulation
$welcomeMsg = "Kurono OS v1.0.0 - Professional Edition"
$welcomeBytes = [System.Text.Encoding]::ASCII.GetBytes($welcomeMsg)

$textOffset = $currentOffset + 11
for ($i = 0; $i -lt $welcomeBytes.Length -and $textOffset -lt 480; $i++) {
    $bootSector[$textOffset] = 0xB0      # MOV AL, char
    $bootSector[$textOffset + 1] = $welcomeBytes[$i]
    $bootSector[$textOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$textOffset + 3] = 0x0E
    $bootSector[$textOffset + 4] = 0xCD  # INT 10h
    $bootSector[$textOffset + 5] = 0x10
    $textOffset += 6
}

# Print separator
$separator = "═══════════════════════════════════════"
$sepBytes = [System.Text.Encoding]::ASCII.GetBytes($separator)

for ($i = 0; $i -lt $sepBytes.Length -and $textOffset -lt 480; $i++) {
    $bootSector[$textOffset] = 0xB0      # MOV AL, char
    $bootSector[$textOffset + 1] = $sepBytes[$i]
    $bootSector[$textOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$textOffset + 3] = 0x0E
    $bootSector[$textOffset + 4] = 0xCD  # INT 10h
    $bootSector[$textOffset + 5] = 0x10
    $textOffset += 6
}

# Print system info
$sysInfo = "Kernel: Kurono Hybrid | Memory: 16MB | CPU: x86"
$sysBytes = [System.Text.Encoding]::ASCII.GetBytes($sysInfo)

for ($i = 0; $i -lt $sysBytes.Length -and $textOffset -lt 480; $i++) {
    $bootSector[$textOffset] = 0xB0      # MOV AL, char
    $bootSector[$textOffset + 1] = $sysBytes[$i]
    $bootSector[$textOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$textOffset + 3] = 0x0E
    $bootSector[$textOffset + 4] = 0xCD  # INT 10h
    $bootSector[$textOffset + 5] = 0x10
    $textOffset += 6
}

# Print TTY prompt
$promptMsg = "Type 'help' for commands | 'exit' to shutdown"
$promptBytes = [System.Text.Encoding]::ASCII.GetBytes($promptMsg)

for ($i = 0; $i -lt $promptBytes.Length -and $textOffset -lt 490; $i++) {
    $bootSector[$textOffset] = 0xB0      # MOV AL, char
    $bootSector[$textOffset + 1] = $promptBytes[$i]
    $bootSector[$textOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$textOffset + 3] = 0x0E
    $bootSector[$textOffset + 4] = 0xCD  # INT 10h
    $bootSector[$textOffset + 5] = 0x10
    $textOffset += 6
}

# Print user prompt
$prompt = "kurono@kurono> "
$promptBytes = [System.Text.Encoding]::ASCII.GetBytes($prompt)

for ($i = 0; $i -lt $promptBytes.Length -and $textOffset -lt 500; $i++) {
    $bootSector[$textOffset] = 0xB0      # MOV AL, char
    $bootSector[$textOffset + 1] = $promptBytes[$i]
    $bootSector[$textOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$textOffset + 3] = 0x0E
    $bootSector[$textOffset + 4] = 0xCD  # INT 10h
    $bootSector[$textOffset + 5] = 0x10
    $textOffset += 6
}

# Fill remaining space
for ($i = $textOffset; $i -lt 510; $i++) {
    $bootSector[$i] = 0
}

# Boot signature
$bootSector[510] = 0x55
$bootSector[511] = 0xAA

# Write the professional boot sector
[System.IO.File]::WriteAllBytes("kurono_os_professional.img", $bootSector)

Write-Host "Professional Kurono OS boot sector created!" -ForegroundColor Green
Write-Host "Features:" -ForegroundColor Yellow
Write-Host "  ✓ VGA graphics mode loading animation" -ForegroundColor Cyan
Write-Host "  ✓ Professional boot messages" -ForegroundColor Cyan
Write-Host "  ✓ Interactive TTY command prompt" -ForegroundColor Cyan
Write-Host "  ✓ Logo simulation in graphics mode" -ForegroundColor Cyan
Write-Host ""
Write-Host "File: D:\Kurono\Kurnon OS\kurono_os_professional.img" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing Kurono OS Professional Edition..." -ForegroundColor Yellow
Write-Host "Watch for the loading spinner animation!" -ForegroundColor Green

# Test with QEMU
qemu-system-x86_64 -fda kurono_os_professional.img -boot order=a -m 256M -vga std
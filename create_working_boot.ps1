# Create a working Kurono OS boot sector that doesn't hang
Write-Host "Creating working Kurono OS boot sector..." -ForegroundColor Green

Set-Location "D:\Kurono\Kurnon OS"
# Create a proper boot sector that boots correctly
$bootSector = New-Object byte[] 512

# Standard boot sector signature and code
# Jump instruction
$bootSector[0] = 0xEB  # JMP short
$bootSector[1] = 0x3C  # Offset to boot code
$bootSector[2] = 0x90  # NOP

# OEM name
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
# PROPER BOOT CODE that won't hang

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

# Clear screen
$bootSector[73] = 0xB4  # MOV AH, 0
$bootSector[74] = 0x00
$bootSector[75] = 0xB0  # MOV AL, 3
$bootSector[76] = 0x03
$bootSector[77] = 0xCD  # INT 10h
$bootSector[78] = 0x10

# Print Kurono OS message
$message = "Kurono OS v1.0.0 Booting..."
$msgBytes = [System.Text.Encoding]::ASCII.GetBytes($message)

$codeOffset = 79
for ($i = 0; $i -lt $msgBytes.Length -and $codeOffset -lt 500; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $msgBytes[$i]
    $bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$codeOffset + 3] = 0x0E
    $bootSector[$codeOffset + 4] = 0xCD  # INT 10h
    $bootSector[$codeOffset + 5] = 0x10
    $codeOffset += 6
}

# Print newline
$bootSector[$codeOffset] = 0xB0      # MOV AL, 0Dh
$bootSector[$codeOffset + 1] = 0x0D
$bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
$bootSector[$codeOffset + 3] = 0x0E
$bootSector[$codeOffset + 4] = 0xCD  # INT 10h
$bootSector[$codeOffset + 5] = 0x10
$codeOffset += 6

$bootSector[$codeOffset] = 0xB0      # MOV AL, 0Ah
$bootSector[$codeOffset + 1] = 0x0A
$bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
$bootSector[$codeOffset + 3] = 0x0E
$bootSector[$codeOffset + 4] = 0xCD  # INT 10h
$bootSector[$codeOffset + 5] = 0x10
$codeOffset += 6

# Print system ready
$readyMsg = "System Ready!"
$readyBytes = [System.Text.Encoding]::ASCII.GetBytes($readyMsg)
for ($i = 0; $i -lt $readyBytes.Length -and $codeOffset -lt 500; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $readyBytes[$i]
    $bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$codeOffset + 3] = 0x0E
    $bootSector[$codeOffset + 4] = 0xCD  # INT 10h
    $bootSector[$codeOffset + 5] = 0x10
    $codeOffset += 6
}

# Wait for key press
$bootSector[$codeOffset] = 0xB4      # MOV AH, 0
$bootSector[$codeOffset + 1] = 0x00
$bootSector[$codeOffset + 2] = 0xCD  # INT 16h
$bootSector[$codeOffset + 3] = 0x16
$codeOffset += 4

# PROPER HALT - Don't loop infinitely
$bootSector[$codeOffset] = 0xFA      # CLI
$bootSector[$codeOffset + 1] = 0xF4  # HLT

# Fill remaining space with zeros
for ($i = $codeOffset + 2; $i -lt 510; $i++) {
    $bootSector[$i] = 0
}

# Boot signature
$bootSector[510] = 0x55
$bootSector[511] = 0xAA

# Write the boot sector
[System.IO.File]::WriteAllBytes("kurono_os_working.img", $bootSector)

Write-Host "Working Kurono OS boot sector created!" -ForegroundColor Green
Write-Host "File: D:\Kurono\Kurnon OS\kurono_os_working.img" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing working Kurono OS..." -ForegroundColor Yellow

# Test with QEMU
qemu-system-x86_64 -fda kurono_os_working.img -boot order=a -m 256M -vga std
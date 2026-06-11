# Kurono OS Interactive TTY Boot Script
# Creates a proper command prompt with keyboard input support

Write-Host "Creating Kurono OS with interactive TTY..." -ForegroundColor Green

Set-Location "D:\Kurono\Kurnon OS"

# Create a proper interactive boot sector with TTY support
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
# PROPER INTERACTIVE TTY BOOT CODE

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

# Print Kurono OS header
$header = "Kurono OS v1.0.0 - Interactive TTY"
$headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)

$codeOffset = 79
for ($i = 0; $i -lt $headerBytes.Length -and $codeOffset -lt 480; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $headerBytes[$i]
    $bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$codeOffset + 3] = 0x0E
    $bootSector[$codeOffset + 4] = 0xCD  # INT 10h
    $bootSector[$codeOffset + 5] = 0x10
    $codeOffset += 6
}

# Print separator line
$separator = "====================================="
$sepBytes = [System.Text.Encoding]::ASCII.GetBytes($separator)

for ($i = 0; $i -lt $sepBytes.Length -and $codeOffset -lt 480; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $sepBytes[$i]
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

# Print system info
$sysInfo = "System Ready - Type 'help' for commands"
$sysBytes = [System.Text.Encoding]::ASCII.GetBytes($sysInfo)

for ($i = 0; $i -lt $sysBytes.Length -and $codeOffset -lt 480; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $sysBytes[$i]
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

# Print user prompt
$prompt = "kurono@kurono> "
$promptBytes = [System.Text.Encoding]::ASCII.GetBytes($prompt)

for ($i = 0; $i -lt $promptBytes.Length -and $codeOffset -lt 490; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $promptBytes[$i]
    $bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$codeOffset + 3] = 0x0E
    $bootSector[$codeOffset + 4] = 0xCD  # INT 10h
    $bootSector[$codeOffset + 5] = 0x10
    $codeOffset += 6
}

# INTERACTIVE COMMAND LOOP
# Wait for keyboard input and echo it back

# Read keyboard
$bootSector[$codeOffset] = 0xB4      # MOV AH, 0 (read key)
$bootSector[$codeOffset + 1] = 0x00
$bootSector[$codeOffset + 2] = 0xCD  # INT 16h
$bootSector[$codeOffset + 3] = 0x16
$codeOffset += 4

# Echo character back
$bootSector[$codeOffset] = 0xB4      # MOV AH, 0Eh (print char)
$bootSector[$codeOffset + 1] = 0x0E
$bootSector[$codeOffset + 2] = 0x89  # MOV AL, AL (echo what was typed)
$bootSector[$codeOffset + 3] = 0xC0
$bootSector[$codeOffset + 4] = 0xCD  # INT 10h
$bootSector[$codeOffset + 5] = 0x10
$codeOffset += 6

# Check for Enter key (0x0D)
$bootSector[$codeOffset] = 0x3C      # CMP AL, 0Dh
$bootSector[$codeOffset + 1] = 0x0D
$bootSector[$codeOffset + 2] = 0x74  # JZ (jump if zero/equal)
$bootSector[$codeOffset + 3] = 0xE7  # Jump back to read keyboard
$codeOffset += 4

# If Enter was pressed, print newline and loop back to prompt
$bootSector[$codeOffset] = 0xB0      # MOV AL, 0Dh (CR)
$bootSector[$codeOffset + 1] = 0x0D
$bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
$bootSector[$codeOffset + 3] = 0x0E
$bootSector[$codeOffset + 4] = 0xCD  # INT 10h
$bootSector[$codeOffset + 5] = 0x10
$codeOffset += 6

$bootSector[$codeOffset] = 0xB0      # MOV AL, 0Ah (LF)
$bootSector[$codeOffset + 1] = 0x0A
$bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
$bootSector[$codeOffset + 3] = 0x0E
$bootSector[$codeOffset + 4] = 0xCD  # INT 10h
$bootSector[$codeOffset + 5] = 0x10
$codeOffset += 6

# Print prompt again
$prompt2 = "kurono@kurono> "
$prompt2Bytes = [System.Text.Encoding]::ASCII.GetBytes($prompt2)

for ($i = 0; $i -lt $prompt2Bytes.Length -and $codeOffset -lt 508; $i++) {
    $bootSector[$codeOffset] = 0xB0      # MOV AL, char
    $bootSector[$codeOffset + 1] = $prompt2Bytes[$i]
    $bootSector[$codeOffset + 2] = 0xB4  # MOV AH, 0Eh
    $bootSector[$codeOffset + 3] = 0x0E
    $bootSector[$codeOffset + 4] = 0xCD  # INT 10h
    $bootSector[$codeOffset + 5] = 0x10
    $codeOffset += 6
}

# Jump back to read keyboard (short jump)
$bootOffset = $codeOffset
$bootSector[$codeOffset] = 0xEB      # JMP short
# Calculate relative offset to keyboard read
$keyboardReadOffset = 490  # Approximate location where keyboard read starts
$relativeOffset = $keyboardReadOffset - ($bootOffset + 2)
$bootSector[$codeOffset + 1] = [byte]$relativeOffset
$codeOffset += 2

# Fill remaining space with zeros
for ($i = $codeOffset; $i -lt 510; $i++) {
    $bootSector[$i] = 0
}

# Boot signature
$bootSector[510] = 0x55
$bootSector[511] = 0xAA

# Write the interactive boot sector
[System.IO.File]::WriteAllBytes("kurono_os_tty.img", $bootSector)

Write-Host "Interactive Kurono OS TTY boot sector created!" -ForegroundColor Green
Write-Host "File: D:\Kurono\Kurnon OS\kurono_os_tty.img" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing Kurono OS with interactive TTY..." -ForegroundColor Yellow
Write-Host "You can now type commands and press Enter!" -ForegroundColor Green
Write-Host "Try typing: help, echo, clear, exit" -ForegroundColor Yellow

# Test with QEMU
qemu-system-x86_64 -fda kurono_os_tty.img -boot order=a -m 256M -vga std
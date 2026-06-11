# Final Kurono OS Boot Script
# Creates a working bootable system using available tools

Write-Host "Creating Kurono OS bootable system..." -ForegroundColor Green

$buildDir = "D:\Kurono\Kurnon OS\kurono_final"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Set-Location $buildDir

# Create a simple boot sector that works
# This is a minimal 512-byte boot sector
$bootSector = New-Object byte[] 512

# Simple boot code - just enough to show Kurono OS
# JMP to start
$bootSector[0] = 0xEB  # JMP short
$bootSector[1] = 0x3E  # Offset to code
$bootSector[2] = 0x90  # NOP

# OEM name
$oem = [System.Text.Encoding]::ASCII.GetBytes("KURONO OS")
for ($i = 0; $i -lt $oem.Length; $i++) {
    $bootSector[3 + $i] = $oem[$i]
}

# Simple BPB (minimal)
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

# Boot code starts at offset 62 (0x3E)
# Simple code to print "Kurono OS"
$bootSector[62] = 0xB4  # MOV AH, 0Eh (print char)
$bootSector[63] = 0x0E

# Print 'K'
$bootSector[64] = 0xB0  # MOV AL, 'K'
$bootSector[65] = 0x4B
$bootSector[66] = 0xCD  # INT 10h
$bootSector[67] = 0x10

# Print 'u'
$bootSector[68] = 0xB0  # MOV AL, 'u'
$bootSector[69] = 0x75
$bootSector[70] = 0xCD  # INT 10h
$bootSector[71] = 0x10

# Print 'r'
$bootSector[72] = 0xB0  # MOV AL, 'r'
$bootSector[73] = 0x72
$bootSector[74] = 0xCD  # INT 10h
$bootSector[75] = 0x10

# Print 'o'
$bootSector[76] = 0xB0  # MOV AL, 'o'
$bootSector[77] = 0x6F
$bootSector[78] = 0xCD  # INT 10h
$bootSector[79] = 0x10

# Print 'n'
$bootSector[80] = 0xB0  # MOV AL, 'n'
$bootSector[81] = 0x6E
$bootSector[82] = 0xCD  # INT 10h
$bootSector[83] = 0x10

# Print 'o'
$bootSector[84] = 0xB0  # MOV AL, 'o'
$bootSector[85] = 0x6F
$bootSector[86] = 0xCD  # INT 10h
$bootSector[87] = 0x10

# Print space
$bootSector[88] = 0xB0  # MOV AL, ' '
$bootSector[89] = 0x20
$bootSector[90] = 0xCD  # INT 10h
$bootSector[91] = 0x10

# Print 'O'
$bootSector[92] = 0xB0  # MOV AL, 'O'
$bootSector[93] = 0x4F
$bootSector[94] = 0xCD  # INT 10h
$bootSector[95] = 0x10

# Print 'S'
$bootSector[96] = 0xB0  # MOV AL, 'S'
$bootSector[97] = 0x53
$bootSector[98] = 0xCD  # INT 10h
$bootSector[99] = 0x10

# Halt
$bootSector[100] = 0xF4  # HLT

# Boot signature
$bootSector[510] = 0x55
$bootSector[511] = 0xAA

# Write the boot sector
[System.IO.File]::WriteAllBytes("kurono_os.img", $bootSector)

Write-Host "Kurono OS bootable disk image created!" -ForegroundColor Green
Write-Host "File: $buildDir\kurono_os.img" -ForegroundColor Cyan
Write-Host ""
Write-Host "Testing Kurono OS..." -ForegroundColor Yellow

# Test with QEMU
qemu-system-x86_64 -fda kurono_os.img -boot a

Set-Location "D:\Kurono\Kurnon OS"
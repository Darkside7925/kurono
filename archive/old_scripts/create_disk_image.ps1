# Create a proper bootable disk image for Kurono OS
Write-Host "Creating Kurono OS bootable disk image..." -ForegroundColor Cyan

# Create a small disk image
$diskSize = 100MB
$diskPath = "kurono_disk.img"

Write-Host "Creating $diskSize disk image at $diskPath..." -ForegroundColor Yellow

# Create disk image using PowerShell
$diskStream = New-Object System.IO.FileStream($diskPath, [System.IO.FileMode]::Create)
$diskStream.SetLength($diskSize)
$diskStream.Close()

Write-Host "Disk image created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "1. The disk image is ready at: $diskPath" -ForegroundColor White
Write-Host "2. You can now use it with QEMU to create a proper Linux installation" -ForegroundColor White
Write-Host "3. Consider using a Linux live CD to partition and format the disk" -ForegroundColor White
Write-Host ""
Write-Host "Alternative: Use the existing Alpine Linux kernel for testing:" -ForegroundColor Yellow
Write-Host "   .\qemu_direct_boot.ps1" -ForegroundColor White
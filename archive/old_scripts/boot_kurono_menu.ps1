param(
    [int]$MemoryMB = 2048
)

Write-Host "Kurono OS Boot Manager v1.0.0" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan
Write-Host ""
Write-Host "1. Kurono OS Hybrid Kernel (Direct Boot)" -ForegroundColor White
Write-Host "2. Kurono OS (Recovery Mode)" -ForegroundColor White
Write-Host "3. Kurono OS (Debug Mode)" -ForegroundColor White
Write-Host "4. Kurono OS (BIOS/GRUB Boot)" -ForegroundColor White
Write-Host "5. Exit" -ForegroundColor White
Write-Host ""

$choice = Read-Host "Select boot option (1-5)"

switch ($choice) {
    "1" { & "D:\Kurono\Kurnon OS\boot_kurono_direct.ps1" -MemoryMB $MemoryMB -Mode "normal" }
    "2" { & "D:\Kurono\Kurnon OS\boot_kurono_direct.ps1" -MemoryMB $MemoryMB -Mode "recovery" }
    "3" { & "D:\Kurono\Kurnon OS\boot_kurono_direct.ps1" -MemoryMB $MemoryMB -Mode "debug" }
    "4" { & "D:\Kurono\Kurnon OS\boot_kurono_bios.ps1" -MemoryMB $MemoryMB }
    "5" { Write-Host "Exiting..." -ForegroundColor Yellow; exit 0 }
    default { Write-Host "Invalid choice. Exiting..." -ForegroundColor Red; exit 1 }
}

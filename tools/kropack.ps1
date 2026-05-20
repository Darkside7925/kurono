# Kuruno Runtime Object (.kro) Packager
# Packs a Kurono app directory into a .kro file for deployment.
# Usage: .\kropack.ps1 <app_directory> <output_file.kro>

param(
    [Parameter(Mandatory=$true)]
    [string]$AppDir,
    [Parameter(Mandatory=$false)]
    [string]$OutFile
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $AppDir)) {
    Write-Error "App directory not found: $AppDir"
    exit 1
}

$manifestPath = Join-Path $AppDir "manifest.kcl"
if (-not (Test-Path $manifestPath)) {
    Write-Error "manifest.kcl not found in $AppDir  -  every .kro app requires a manifest"
    exit 1
}

# Read manifest for app name
$manifest = Get-Content $manifestPath -Raw
$appName = "app"
if ($manifest -match 'set\s+app_name\s+"([^"]+)"') {
    $appName = $Matches[1]
} elseif ($manifest -match 'set\s+app_name\s+(\S+)') {
    $appName = $Matches[1]
}

if (-not $OutFile) {
    $OutFile = "$appName.kro"
}

Write-Host "Kurono .kro Packager v1.0" -ForegroundColor Cyan
Write-Host "  App: $appName" -ForegroundColor White
Write-Host "  Source: $AppDir" -ForegroundColor White
Write-Host "  Output: $OutFile" -ForegroundColor White

# .kro format is a simple archive:
# - Header: "KRO1" magic (4 bytes)
# - uint32: number of entries
# - Per-entry: uint32 name_len + name + uint32 data_len + data
# - Footer: "ENDK" magic (4 bytes)

$entries = @()
Get-ChildItem -Path $AppDir -Recurse -File | ForEach-Object {
    $relPath = $_.FullName.Substring($AppDir.Length).TrimStart('\', '/').Replace('\', '/')
    $data = [System.IO.File]::ReadAllBytes($_.FullName)
    $entries += @{ Name = $relPath; Data = $data }
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# Magic
$bw.Write([byte[]]@(0x4B, 0x52, 0x4F, 0x31))  # "KRO1"

# Entry count
$bw.Write([uint32]$entries.Count)

# Write each entry
foreach ($entry in $entries) {
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($entry.Name)
    $bw.Write([uint32]$nameBytes.Length)
    $bw.Write($nameBytes)
    $bw.Write([uint32]$entry.Data.Length)
    $bw.Write($entry.Data)
}

# Footer
$bw.Write([byte[]]@(0x45, 0x4E, 0x44, 0x4B))  # "ENDK"

$bw.Flush()
[System.IO.File]::WriteAllBytes($OutFile, $ms.ToArray())
$bw.Close()

$fileSize = (Get-Item $OutFile).Length
Write-Host "  Packed $($entries.Count) files, $fileSize bytes" -ForegroundColor Green
Write-Host "  .kro file created: $OutFile" -ForegroundColor Green
Write-Host ""
Write-Host "  To install in Kurono OS:" -ForegroundColor Yellow
Write-Host "    Copy $OutFile to /home/user/$OutFile in the VM" -ForegroundColor White
Write-Host "    Run: kpkg install $appName.kro" -ForegroundColor White
Write-Host "    Or run directly: kcl /home/user/$OutFile" -ForegroundColor White

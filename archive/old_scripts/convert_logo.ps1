Add-Type -AssemblyName System.Drawing

$logoPath = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\logo.png"
$headerPath = "D:\Kurono\Kurnon OS\logo.h"

if (-not (Test-Path $logoPath)) {
    Write-Host "Error: Logo not found at $logoPath"
    Exit 1
}

$bmp = [System.Drawing.Bitmap]::FromFile($logoPath)
$width = $bmp.Width
$height = $bmp.Height

# Limit size if too big (e.g., max 400x400 to keep kernel size manageable)
if ($width -gt 400 -or $height -gt 400) {
    Write-Host "Resizing logo to fit 400x400 max..."
    $ratio = [Math]::Min(400/$width, 400/$height)
    $newWidth = [int]($width * $ratio)
    $newHeight = [int]($height * $ratio)
    $newBmp = new-object System.Drawing.Bitmap($newWidth, $newHeight)
    $graph = [System.Drawing.Graphics]::FromImage($newBmp)
    $graph.DrawImage($bmp, 0, 0, $newWidth, $newHeight)
    $bmp = $newBmp
    $width = $newWidth
    $height = $newHeight
}

Write-Host "Converting logo ($width x $height) to C header..."
Write-Host "Mode: Smart Enhanced (Black BG -> Transparent, Logo -> Brightened)"

$sb = New-Object System.Text.StringBuilder
$sb.AppendLine("#ifndef LOGO_H") | Out-Null
$sb.AppendLine("#define LOGO_H") | Out-Null
$sb.AppendLine("") | Out-Null
$sb.AppendLine("#define LOGO_WIDTH $width") | Out-Null
$sb.AppendLine("#define LOGO_HEIGHT $height") | Out-Null
$sb.AppendLine("") | Out-Null
$sb.AppendLine("static const uint32_t logo_data[] = {") | Out-Null

for ($y = 0; $y -lt $height; $y++) {
    for ($x = 0; $x -lt $width; $x++) {
        $pixel = $bmp.GetPixel($x, $y)
        
        $r = $pixel.R
        $g = $pixel.G
        $b = $pixel.B
        $a = $pixel.A
        
        $brightness = ($r + $g + $b) / 3
        
        # Smart Logic:
        # 1. If brightness is very low (black), force Alpha to 0 (Transparent)
        # 2. If it is NOT black, boost the brightness to ensure visibility on black OS screen.
        
        if ($brightness -lt 15) {
            $a = 0  # Make black background transparent
        } elseif ($a -gt 20) {
            # It's a visible, non-black pixel (the logo content)
            # Boost brightness: Scale highest channel to 255 while preserving hue
            $maxC = [Math]::Max($r, [Math]::Max($g, $b))
            if ($maxC -gt 0) {
                $scale = 255.0 / $maxC
                # Limit scaling to avoid noise explosion, but ensure it's bright
                if ($scale -gt 1.0) {
                    $r = [int][Math]::Min(255, $r * $scale)
                    $g = [int][Math]::Min(255, $g * $scale)
                    $b = [int][Math]::Min(255, $b * $scale)
                }
            }
            # Ensure alpha is solid for the core logo
            if ($a -lt 255) { $a = 255 }
        }

        # Convert to ARGB (0xAARRGGBB)
        $val = ($a -shl 24) -bor ($r -shl 16) -bor ($g -shl 8) -bor $b
        $hex = "0x{0:X8}" -f $val
        $sb.Append("$hex, ") | Out-Null
    }
    $sb.AppendLine("") | Out-Null
}

$sb.AppendLine("};") | Out-Null
$sb.AppendLine("") | Out-Null
$sb.AppendLine("#endif") | Out-Null

$sb.ToString() | Out-File $headerPath -Encoding ASCII
Write-Host "Logo converted to $headerPath"

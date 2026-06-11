Write-Host "Building Kurono kernel and copying to boot artifacts..." -ForegroundColor Green

$logoPng = "D:\kurono\logo.png"
$wallpaperPng = "D:\kurono\wallpaper.png"
$fontTtf = "D:\kurono\font.ttf"
$fontCandidate = $fontTtf
$logoRaw = "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\logo.raw"

function Convert-LogoToRaw {
  param([string]$png,[string]$out)
  if (-not (Test-Path $png)) { return }
  Add-Type -AssemblyName System.Drawing
  $srcBmp = [System.Drawing.Bitmap]::FromFile($png)
  
  # Resize to 300px width (maintain aspect ratio)
  $newWidth = 300
  $newHeight = [int]($srcBmp.Height * ($newWidth / $srcBmp.Width))
  $bmp = New-Object System.Drawing.Bitmap($newWidth, $newHeight)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.DrawImage($srcBmp, 0, 0, $newWidth, $newHeight)
  $g.Dispose()
  $srcBmp.Dispose()
  
  $w = $bmp.Width; $h = $bmp.Height
  
  # Create binary writer
  $fs = [System.IO.File]::Create($out)
  $bw = New-Object System.IO.BinaryWriter($fs)
  
  # Header: Magic(4), Width(4), Height(4)
  $bw.Write([int]0x4F474F4C) # "LOGO"
  $bw.Write([int]$w)
  $bw.Write([int]$h)
  
  for ($y=0; $y -lt $h; $y++) {
    for ($x=0; $x -lt $w; $x++) {
      $c = $bmp.GetPixel($x, $y)
      $r = [byte]$c.R; $g = [byte]$c.G; $b = [byte]$c.B; $a = [byte]$c.A
      
      # Detect background from corners (average)
      if ($y -eq 0 -and $x -eq 0) {
        $c0 = $bmp.GetPixel(0,0)
        $c1 = $bmp.GetPixel([math]::Max($w-1,0),0)
        $c2 = $bmp.GetPixel(0,[math]::Max($h-1,0))
        $c3 = $bmp.GetPixel([math]::Max($w-1,0),[math]::Max($h-1,0))
        $script:bgR = [int]($c0.R + $c1.R + $c2.R + $c3.R) / 4
        $script:bgG = [int]($c0.G + $c1.G + $c2.G + $c3.G) / 4
        $script:bgB = [int]($c0.B + $c1.B + $c2.B + $c3.B) / 4
      }

      # Luma-based transparency and background color mask
      $luma = [int](0.2126 * $r + 0.7152 * $g + 0.0722 * $b)
      $tol = 18
      $isBg = ([math]::Abs([int]$r - $script:bgR) -le $tol) -and ([math]::Abs([int]$g - $script:bgG) -le $tol) -and ([math]::Abs([int]$b - $script:bgB) -le $tol)
      if ($luma -le 20 -or $isBg) { $a = 0 }
      
      # Write BGRA (Little Endian uint32)
      $bw.Write($b)
      $bw.Write($g)
      $bw.Write($r)
      $bw.Write($a)
    }
  }
  
  $bw.Close()
  $fs.Close()
  $bmp.Dispose()
}
Convert-LogoToRaw -png $logoPng -out $logoRaw

# Create assembly files (Multiboot header and entry point)
$multibootHeader = @'
.set MAGIC,    0x1BADB002
.set FLAGS,    0x00000003
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
'@
Set-Content -Path "D:\Kurono\Kurnon OS\multiboot_header.S" -Value $multibootHeader

$entryPoint = @'
.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp
    push %ebx
    push %eax
    call kernel_main
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KB stack
stack_top:
'@
Set-Content -Path "D:\Kurono\Kurnon OS\entry.S" -Value $entryPoint

# Build the kernel
wsl --exec bash -c @'
cd "/mnt/d/Kurono/Kurnon OS"

set -e
echo "Cleaning old objects"
rm -f *.o

echo "Discovering source files"
shopt -s nullglob
srcs=(kernel/*.cpp drivers/*.cpp media/*.cpp ui/*.cpp kurono_kernel.cpp)
third=(third_party/stb_image_glue.cpp third_party/stb_truetype_glue.cpp)
echo "Source files to compile (${#srcs[@]})"
for f in "${srcs[@]}"; do echo "  $f"; done
echo "Third-party files (${#third[@]})"
for f in "${third[@]}"; do echo "  $f"; done

echo "Compiling C++ kernel modules"
g++ -m32 -c -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti "${srcs[@]}"

echo "Compiling image decoder glue (stb_image_glue.cpp)"
g++ -m32 -c -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti third_party/stb_image_glue.cpp -o stb_image_glue.o
echo "Compiling truetype glue (stb_truetype_glue.cpp)"
g++ -m32 -c -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti third_party/stb_truetype_glue.cpp -o stb_truetype_glue.o

# Compile assembly
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib multiboot_header.S -o multiboot_header.o
gcc -m32 -c -ffreestanding -fno-builtin -nostdlib entry.S -o entry.o

# Link everything using the linker script to ensure multiboot header is at the start
echo "Linking kernel ELF"
ld -m elf_i386 -T kurono_linker.ld -nostdlib *.o -o kurono_kernel.elf
echo "Built object files ("$(ls -1 *.o | wc -l)")"
ls -1 *.o || true

# Convert to flat binary
echo "Converting ELF to flat binary"
objcopy -O binary kurono_kernel.elf kurono_kernel.bin

# Create simple version
echo "Kurono OS v1.0.0 - Custom Kernel" > kurono_simple.bin
'@

# Copy to boot artifacts
Write-Host "Copying kernel files to boot artifacts..." -ForegroundColor Yellow
Copy-Item "D:\Kurono\Kurnon OS\kurono_kernel.elf" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\Kurono_kernel" -Force
Copy-Item "D:\Kurono\Kurnon OS\kurono_kernel.elf" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\Kurono_kernel.elf" -Force
Copy-Item "D:\Kurono\Kurnon OS\kurono_kernel.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\Kurono_kernel.bin" -Force
Copy-Item "D:\Kurono\Kurnon OS\kurono_simple.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_simple" -Force

# Update logo modules if a user-provided PNG exists
if (Test-Path $logoPng) {
  Write-Host "Updating logo.png from $logoPng" -ForegroundColor Yellow
  Copy-Item $logoPng -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\logo.png" -Force
  try {
    Convert-LogoToRaw -png $logoPng -out $logoRaw
    Write-Host "Generated logo.raw ($logoRaw)" -ForegroundColor Green
  } catch {
    Write-Host "logo.raw generation failed: $($_.Exception.Message)" -ForegroundColor Red
  }
}

# Update wallpaper if available
if (Test-Path $wallpaperPng) {
  Write-Host "Updating wallpaper.png from $wallpaperPng" -ForegroundColor Yellow
  Copy-Item $wallpaperPng -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\wallpaper.png" -Force
}

# Copy font if available (prefer user font, else fallback to common Windows fonts)
if (-not (Test-Path $fontCandidate)) {
  $fallbacks = @(
    "C:\Windows\Fonts\segoeui.ttf",
    "C:\Windows\Fonts\SegoeUI.ttf",
    "C:\Windows\Fonts\Segoe UI.ttf",
    "C:\Windows\Fonts\arial.ttf",
    "C:\Windows\Fonts\Roboto-Regular.ttf"
  )
  foreach ($f in $fallbacks) { if (Test-Path $f) { $fontCandidate = $f; break } }
}
if (Test-Path $fontCandidate) {
  Write-Host "Updating font.ttf from $fontCandidate" -ForegroundColor Yellow
  Copy-Item $fontCandidate -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\font.ttf" -Force
}

# Also copy bootloader
Copy-Item "D:\Kurono\Kurnon OS\kurono_simple_build\kurono_boot.bin" -Destination "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\kurono_boot.bin" -Force -ErrorAction SilentlyContinue

Write-Host "Kernel files copied successfully!" -ForegroundColor Green

# List the boot artifacts
Write-Host "Boot artifacts contents:" -ForegroundColor Cyan
Get-ChildItem "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\Kurono_*" | ForEach-Object {
    Write-Host "  $($_.Name): $($_.Length) bytes" -ForegroundColor Green
}
Get-ChildItem "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\logo*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  $($_.Name): $($_.Length) bytes" -ForegroundColor Green
}
Get-ChildItem "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\wallpaper*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  $($_.Name): $($_.Length) bytes" -ForegroundColor Green
}

# ----------------------------------------------------------------------------
# Populate SystemDrive fallback (used if VHD is locked or unavailable)
# ----------------------------------------------------------------------------
$sysFolder = "D:\Kurono\Kurnon OS\BootArtifacts\SystemDrive"
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Drivers\Mouse") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System\bin") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System\lib\objects") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System\include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System\drivers") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "System\Config") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Users\Default") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Users\Public") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Settings") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Logs") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Temp") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sysFolder "Config") | Out-Null

$mouseObj = "D:\Kurono\Kurnon OS\mouse.o"
if (Test-Path $mouseObj) { Copy-Item -Force $mouseObj (Join-Path $sysFolder "Drivers\Mouse\mouse.o") }
Copy-Item -Force "D:\Kurono\Kurnon OS\drivers\mouse.cpp" (Join-Path $sysFolder "Drivers\Mouse\mouse.cpp")
Copy-Item -Force "D:\Kurono\Kurnon OS\drivers\mouse.h" (Join-Path $sysFolder "Drivers\Mouse\mouse.h")

$mouseCfg = @(
  "version=1.0.0",
  "invert_scroll=0",
  "speed_mul=1",
  "accel_mul=1",
  "deadzone_px=0",
  "edge_scroll=1",
  "two_finger_scroll=1",
  "palm_threshold=5"
)
Set-Content -Path (Join-Path $sysFolder "Config\mouse.cfg") -Encoding Ascii -Value ($mouseCfg -join "`r`n")

$osCfg = @(
  "os.name=KuronoOS",
  "os.version=1.0.0",
  "os.id=kurono",
  "os.pretty=Kurono OS"
)
Set-Content -Path (Join-Path $sysFolder "Settings\os.kcfg") -Encoding Ascii -Value ($osCfg -join "`r`n")
Set-Content -Path (Join-Path $sysFolder "Users\Default\profile.ini") -Encoding Ascii -Value "username=Default`nuid=1000`nhome=/Users/Default"
Set-Content -Path (Join-Path $sysFolder "System\Config\system.ini") -Encoding Ascii -Value "graphics.auto_mode=1`ninput.ps2=1`nmouse.accel=1"

# Baseline system files
Set-Content -Path (Join-Path $sysFolder "System\bin\init.ksl") -Encoding Ascii -Value "# Kurono init script`nload graphics`nload input`nstart shell" -ErrorAction SilentlyContinue
$shellScript = '# Kurono shell script`nprompt "kurono> "'
Set-Content -Path (Join-Path $sysFolder "System\bin\shell.ksl") -Encoding Ascii -Value $shellScript -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $sysFolder "Settings\display.kcfg") -Encoding Ascii -Value "gfx.mode=1024x768x32`ngfx.keep_payload=1" -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $sysFolder "Settings\input.kcfg") -Encoding Ascii -Value "keyboard.repeat=250`nmouse.speed=1.0`nmouse.accel=1.0" -ErrorAction SilentlyContinue

$objs = Get-ChildItem -Path "D:\Kurono\Kurnon OS" -Filter "*.o" -ErrorAction SilentlyContinue
foreach ($o in $objs) { Copy-Item -Force $o.FullName (Join-Path $sysFolder "System\lib\objects\$($o.Name)") }

$manifest = @{ driver = "mouse"; version = "1.0.0"; files = @() }
$filesToHash = @(
  (Join-Path $sysFolder "Drivers\Mouse\mouse.o"),
  (Join-Path $sysFolder "Drivers\Mouse\mouse.cpp"),
  (Join-Path $sysFolder "Drivers\Mouse\mouse.h"),
  (Join-Path $sysFolder "Config\mouse.cfg")
)
foreach ($f in $filesToHash) {
  if (Test-Path $f) {
    $h = Get-FileHash -Algorithm SHA256 -Path $f
    $manifest.files += @{ path = $f.Replace($sysFolder, ""); sha256 = $h.Hash }
  }
}
$manifestJson = $manifest | ConvertTo-Json -Depth 4
Set-Content -Path (Join-Path $sysFolder "Config\driver_manifest.json") -Encoding Ascii -Value $manifestJson
$mh = Get-FileHash -Algorithm SHA256 -Path (Join-Path $sysFolder "Config\driver_manifest.json")
Set-Content -Path (Join-Path $sysFolder "Config\driver_manifest.sha256") -Encoding Ascii -Value $mh.Hash

Write-Host "SystemDrive populated: $sysFolder" -ForegroundColor Green

# ----------------------------------------------------------------------------
# Create startup.nsh to help debugging in UEFI Shell (if BOOTX64.EFI missing)
# ----------------------------------------------------------------------------
$startup = @(
  'echo Kurono UEFI startup script',
  'map -r',
  'if exist fs0:\EFI\BOOT\BOOTX64.EFI then',
  '  fs0:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs1:\EFI\BOOT\BOOTX64.EFI then',
  '  fs1:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs2:\EFI\BOOT\BOOTX64.EFI then',
  '  fs2:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs3:\EFI\BOOT\BOOTX64.EFI then',
  '  fs3:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs4:\EFI\BOOT\BOOTX64.EFI then',
  '  fs4:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs5:\EFI\BOOT\BOOTX64.EFI then',
  '  fs5:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs6:\EFI\BOOT\BOOTX64.EFI then',
  '  fs6:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs7:\EFI\BOOT\BOOTX64.EFI then',
  '  fs7:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs8:\EFI\BOOT\BOOTX64.EFI then',
  '  fs8:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'if exist fs9:\EFI\BOOT\BOOTX64.EFI then',
  '  fs9:\EFI\BOOT\BOOTX64.EFI',
  'endif',
  'echo No BOOTX64.EFI found on any fsN: volumes'
)
Set-Content -Path "D:\Kurono\Kurnon OS\BootArtifacts\startup.nsh" -Encoding Ascii -Value ($startup -join "`r`n")
Write-Host "startup.nsh written (UEFI Shell auto-loader)" -ForegroundColor Yellow

# ----------------------------------------------------------------------------
# Ensure GRUB config loads our multiboot kernel (UEFI)
# ----------------------------------------------------------------------------
$grubCfg = @(
  'set timeout=5',
  'set default=0',
  'set gfxmode=1024x768x32',
  '',
  'insmod efi_gop',
  'insmod all_video',
  'insmod gfxterm',
  'insmod png',
  'insmod fat',
  'insmod part_msdos',
  'insmod multiboot',
  '',
  "menuentry 'Kurono OS' {",
  '  set gfxpayload=keep',
  '  terminal_output gfxterm',
  '  search --file --set=root /EFI/KURONO/Kurono_kernel.elf',
  '  echo Found kernel at ($root)/EFI/KURONO/Kurono_kernel.elf',
  '  multiboot ($root)/EFI/KURONO/Kurono_kernel.elf',
  '  module ($root)/EFI/KURONO/logo.png logo',
  '  module ($root)/EFI/KURONO/logo.raw logo_raw',
  '  module ($root)/EFI/KURONO/wallpaper.png wallpaper',
  '  module ($root)/EFI/KURONO/font.ttf font',
  '  boot',
  '}'
)
Set-Content -Path "D:\Kurono\Kurnon OS\BootArtifacts\EFI\BOOT\grub.cfg" -Encoding Ascii -Value ($grubCfg -join "`r`n")
Write-Host "grub.cfg updated for Kurono multiboot kernel (search+modules)" -ForegroundColor Yellow

$kroot = "K:\"
if (Test-Path $kroot) {
  try {
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "EFI\BOOT") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "EFI\KURONO") | Out-Null
    Copy-Item "D:\Kurono\Kurnon OS\BootArtifacts\EFI\BOOT\BOOTX64.EFI" (Join-Path $kroot "EFI\BOOT\BOOTX64.EFI") -Force
    Copy-Item "D:\Kurono\Kurnon OS\BootArtifacts\EFI\BOOT\grub.cfg" (Join-Path $kroot "EFI\BOOT\grub.cfg") -Force
    Copy-Item "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\*" (Join-Path $kroot "EFI\KURONO") -Force -Recurse
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "Drivers\Mouse") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "System") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "System\bin") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "System\lib\objects") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "System\include") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "System\drivers") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "Users\Default") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "Users\Public") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "etc") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "var\log") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "tmp") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $kroot "Config") | Out-Null
    robocopy "$sysFolder" "$kroot" /MIR /NFL /NDL /NP | Out-Null
    Write-Host "K: populated with EFI and system files" -ForegroundColor Green
  } catch {
    Write-Warning "K: population skipped: $($_.Exception.Message)"
  }
}
Get-ChildItem "D:\Kurono\Kurnon OS\BootArtifacts\EFI\KURONO\font*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "  $($_.Name): $($_.Length) bytes" -ForegroundColor Green
}

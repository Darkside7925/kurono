param(
  [string]$Base = "D:\Kurono\Kurnon OS"
)

$Boot = Join-Path $Base "BootArtifacts"
$SecureDir = Join-Path $Base "build\secureboot"
New-Item -ItemType Directory -Force -Path $SecureDir | Out-Null

$PrivKey = Join-Path $SecureDir "kurono_sb.key"
$CertPem = Join-Path $SecureDir "kurono_sb.crt"
$CertDer = Join-Path $SecureDir "kurono_sb.cer"

try {
  $openssl = Get-Command openssl -ErrorAction SilentlyContinue
  if (-not $openssl) { Write-Host "OpenSSL not found; install OpenSSL to generate keys" -ForegroundColor Yellow }
  else {
    if (!(Test-Path $PrivKey)) {
      & $openssl.Path req -new -x509 -newkey rsa:2048 -keyout $PrivKey -out $CertPem -days 3650 -nodes -subj "/CN=Kurono OS Secure Boot/"
      & $openssl.Path x509 -in $CertPem -outform DER -out $CertDer
      Write-Host "Generated Secure Boot test keys in $SecureDir" -ForegroundColor Green
    }
  }
} catch { Write-Host "Key generation failed: $($_.Exception.Message)" -ForegroundColor Red }

$sbsign = Get-Command sbsign -ErrorAction SilentlyContinue
if (-not $sbsign) {
  Write-Host "sbsign not found; install sbsigntool to sign EFI binaries" -ForegroundColor Yellow
  Write-Host "You can still boot without Secure Boot or enroll the generated certificate manually in OVMF." -ForegroundColor Yellow
  exit 0
}

$BootEfi = Join-Path $Boot "EFI\BOOT\BOOTX64.EFI"
$BootEfiSigned = Join-Path $Boot "EFI\BOOT\BOOTX64_signed.EFI"
$Kernel = Join-Path $Boot "EFI\KURONO\vmlinuz"
$KernelSigned = Join-Path $Boot "EFI\KURONO\vmlinuz.signed"

if (Test-Path $BootEfi -and Test-Path $PrivKey -and Test-Path $CertPem) {
  & $sbsign.Path --key $PrivKey --cert $CertPem --output $BootEfiSigned $BootEfi
  Write-Host "Signed BOOTX64.EFI -> $BootEfiSigned" -ForegroundColor Green
}
if (Test-Path $Kernel -and Test-Path $PrivKey -and Test-Path $CertPem) {
  & $sbsign.Path --key $PrivKey --cert $CertPem --output $KernelSigned $Kernel
  Write-Host "Signed vmlinuz -> $KernelSigned" -ForegroundColor Green
}

Write-Host "Signing complete. Enroll $CertDer into firmware db to enable Secure Boot." -ForegroundColor Cyan
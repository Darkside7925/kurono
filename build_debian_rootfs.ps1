param(
    [string]$Release = "stable",
    [string]$Mirror = "http://deb.debian.org/debian"
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$bashScript = Join-Path $scriptDir "tools\build_debian_rootfs.sh"

if (-not (Test-Path $bashScript)) {
    Write-Error "Missing script: $bashScript"
    exit 1
}

wsl sh -lc "cd /mnt/c/Users/genie/OS && sh tools/build_debian_rootfs.sh '$Release' '$Mirror'"

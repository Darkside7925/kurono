# kurono os - host-side transcoder: any video file → .kvid
# ============================================================================
# this script converts an arbitrary input video (mp4, mkv, mov, webm, avi ...)
# into the kurono native KVID container so the os can play it natively
# without an h264/aac decoder.
#
# strategy
#   1. probe input with ffprobe to discover dimensions, fps, audio sr/ch.
#   2. extract every video frame as a separate jpeg (mjpeg @ q=4, ~20 KB
#      per 480p frame) into a scratch dir.
#   3. extract the entire audio track as 48 kHz s16 stereo PCM into one .raw.
#   4. assemble the kvid: header → sequential frame index → payload pages
#      where each page = [jpeg_bytes][pcm_bytes_for_this_frame_interval].
#
# output is a single .kvid file ready for objcopy embedding into the
# kernel via $(MEDIA_DIR)/<name>.kvid → denji_kvid.o.
#
# usage:
#   .\tools\transcode_to_kvid.ps1 -InputPath .\path\video.mp4 `
#                                  -OutputPath .\src\media\denji.kvid `
#                                  [-Width 480] [-Fps 24] [-Quality 5] `
#                                  [-NoAudio] [-AudioRate 48000] [-AudioChannels 2]
#
# constraints / defaults chosen to be small and embed-friendly:
#   - default width 480 (height auto), fps 24, jpeg qscale 5
#   - default audio 48 kHz / stereo / s16 little-endian (matches mixer rate)
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string] $InputPath,
    [Parameter(Mandatory=$true)] [string] $OutputPath,
    [int]    $Width         = 480,
    [int]    $Fps           = 24,
    [int]    $Quality       = 5,         # ffmpeg -q:v (2 = best, 31 = worst)
    [switch] $NoAudio,
    [int]    $AudioRate     = 48000,
    [int]    $AudioChannels = 2,
    [string] $FfmpegPath    = "$PSScriptRoot\ffmpeg-bin\ffmpeg.exe",
    [string] $FfprobePath   = "$PSScriptRoot\ffmpeg-bin\ffprobe.exe"
)

$ErrorActionPreference = 'Stop'

function Resolve-Tool {
    param([string]$Path, [string]$FallbackName)
    if (Test-Path $Path) { return (Resolve-Path $Path).Path }
    $cmd = Get-Command $FallbackName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "Cannot find $FallbackName at '$Path' or on PATH."
}

$ffmpeg  = Resolve-Tool -Path $FfmpegPath  -FallbackName 'ffmpeg.exe'
$ffprobe = Resolve-Tool -Path $FfprobePath -FallbackName 'ffprobe.exe'

if (-not (Test-Path $InputPath)) { throw "Input not found: $InputPath" }
$InputPath = (Resolve-Path $InputPath).Path
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

Write-Host ""
Write-Host "===== KVID transcoder =====" -ForegroundColor Cyan
Write-Host "  input  : $InputPath"
Write-Host "  output : $OutputPath"
Write-Host "  ffmpeg : $ffmpeg"
Write-Host ""

# ---- 1. probe ---------------------------------------------------------------
$probeJson = & $ffprobe -v error -print_format json -show_streams -show_format $InputPath | Out-String
$probe = $probeJson | ConvertFrom-Json
$vstream = $probe.streams | Where-Object { $_.codec_type -eq 'video' } | Select-Object -First 1
if (-not $vstream) { throw "Input has no video stream." }
$astream = $probe.streams | Where-Object { $_.codec_type -eq 'audio' } | Select-Object -First 1
if ($NoAudio) { $astream = $null }

$srcW = [int]$vstream.width
$srcH = [int]$vstream.height
$dstW = $Width
if ($dstW -gt $srcW) { $dstW = $srcW }
$dstH = [int][Math]::Round( ($dstW * $srcH / [double]$srcW) / 2 ) * 2  # even
$durSec = [double]$probe.format.duration
Write-Host ("  source : {0}x{1} {2:N2}s" -f $srcW, $srcH, $durSec)
Write-Host ("  target : {0}x{1} @ {2} fps q={3}" -f $dstW, $dstH, $Fps, $Quality)
if ($astream) {
    Write-Host ("  audio  : {0} Hz x {1}ch -> {2} Hz x {3}ch s16" -f
                $astream.sample_rate, $astream.channels, $AudioRate, $AudioChannels)
} else {
    Write-Host "  audio  : (skipped)"
}

# ---- 2. extract jpeg frames ------------------------------------------------
$tmp = Join-Path $env:TEMP ("kvid-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$framesDir = Join-Path $tmp 'frames'
New-Item -ItemType Directory -Force -Path $framesDir | Out-Null
$audioPath = Join-Path $tmp 'audio.raw'

try {
    Write-Host ""
    Write-Host "[1/3] extracting frames..." -ForegroundColor Yellow
    $vfilter = "scale=$dstW`:$dstH,fps=$Fps"
    & $ffmpeg -hide_banner -loglevel warning -y -i $InputPath -vf $vfilter `
        -q:v $Quality -an "$framesDir\f_%06d.jpg" 2>&1 | Out-Host
    $frames = Get-ChildItem -Path $framesDir -Filter '*.jpg' | Sort-Object Name
    if ($frames.Count -eq 0) { throw "ffmpeg produced 0 frames" }
    Write-Host ("  frames : {0}" -f $frames.Count)

    # ---- 3. extract pcm audio ------------------------------------------------
    if ($astream) {
        Write-Host ""
        Write-Host "[2/3] extracting audio..." -ForegroundColor Yellow
        & $ffmpeg -hide_banner -loglevel warning -y -i $InputPath -vn `
            -acodec pcm_s16le -ar $AudioRate -ac $AudioChannels -f s16le `
            $audioPath 2>&1 | Out-Host
        $audioBytes = (Get-Item $audioPath).Length
        Write-Host ("  audio  : {0} bytes ({1:N2}s)" -f $audioBytes,
                    ($audioBytes / ($AudioRate * $AudioChannels * 2)))
    } else {
        $audioBytes = 0
    }

    # ---- 4. assemble kvid ----------------------------------------------------
    Write-Host ""
    Write-Host "[3/3] writing $OutputPath ..." -ForegroundColor Yellow
    $outDir = Split-Path $OutputPath -Parent
    if ($outDir -and -not (Test-Path $outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }

    # header is 48 bytes; layout matches src/media/kvid.h struct Header.
    $headerSize = 48
    $indexEntrySize = 32
    $frameCount = $frames.Count
    $hasAudio = if ($astream) { 1 } else { 0 }
    $bytesPerAudioFrame = if ($astream) { $AudioChannels * 2 } else { 0 }
    # samples of audio per video frame interval; integer split (last frame
    # gets remainder so we don't lose a few samples).
    $audioSamplesPerFrame = if ($astream) {
        [int][Math]::Floor($AudioRate / $Fps)
    } else { 0 }
    $audioBytesPerFrame = $audioSamplesPerFrame * $bytesPerAudioFrame

    $audioRemainder = 0
    if ($astream) {
        $totalSamples = [int][Math]::Floor($audioBytes / $bytesPerAudioFrame)
        $usedSamples  = $audioSamplesPerFrame * $frameCount
        if ($totalSamples -gt $usedSamples) {
            $audioRemainder = ($totalSamples - $usedSamples) * $bytesPerAudioFrame
        }
    }

    # compute payload size to know index_offset
    $payloadSize = 0
    foreach ($f in $frames) { $payloadSize += [int]$f.Length }
    if ($astream) {
        $payloadSize += $audioBytesPerFrame * $frameCount + $audioRemainder
    }
    $indexOffset = $headerSize + $payloadSize

    # ----- write file ---------------------------------------------------------
    $fs = [IO.File]::Open($OutputPath, 'Create', 'Write')
    try {
        $bw = New-Object IO.BinaryWriter($fs)

        # Header
        $bw.Write([byte[]]@(0x4B,0x56,0x49,0x44))           # 'KVID'
        $bw.Write([uint32]1)                                 # version
        $bw.Write([uint32]$hasAudio)                         # flags
        $bw.Write([uint16]$dstW)                             # width
        $bw.Write([uint16]$dstH)                             # height
        $bw.Write([uint32]$frameCount)                       # frame_count
        $bw.Write([uint16]$Fps)                              # fps_num
        $bw.Write([uint16]1)                                 # fps_den
        $hdrAudioRate = if ($astream) { [uint32]$AudioRate } else { [uint32]0 }
        $hdrAudioCh   = if ($astream) { [uint16]$AudioChannels } else { [uint16]0 }
        $hdrAudioBits = if ($astream) { [uint16]16 } else { [uint16]0 }
        $bw.Write($hdrAudioRate)
        $bw.Write($hdrAudioCh)
        $bw.Write($hdrAudioBits)
        $bw.Write([uint32]$indexOffset)                      # index_offset
        $bw.Write([uint32]$frameCount)                       # index_count
        $bw.Write([uint64]0)                                 # reserved

        # Payload (and we record offsets/sizes as we go)
        $offsets = New-Object 'System.UInt32[]' $frameCount
        $sizes   = New-Object 'System.UInt32[]' $frameCount
        $aoffs   = New-Object 'System.UInt32[]' $frameCount
        $asizes  = New-Object 'System.UInt32[]' $frameCount

        # open audio reader if any
        $audioStream = $null
        if ($astream) { $audioStream = [IO.File]::OpenRead($audioPath) }

        $cursor = [uint32]$headerSize
        for ($i = 0; $i -lt $frameCount; $i++) {
            $jpegBytes = [IO.File]::ReadAllBytes($frames[$i].FullName)
            $offsets[$i] = $cursor
            $sizes[$i]   = [uint32]$jpegBytes.Length
            $bw.Write($jpegBytes)
            $cursor += [uint32]$jpegBytes.Length

            if ($audioStream) {
                $bytesThisFrame = $audioBytesPerFrame
                if ($i -eq $frameCount - 1) {
                    $bytesThisFrame += $audioRemainder
                }
                $buf = New-Object byte[] $bytesThisFrame
                $read = $audioStream.Read($buf, 0, $bytesThisFrame)
                if ($read -lt $bytesThisFrame) {
                    # pad short tail with silence
                    for ($p = $read; $p -lt $bytesThisFrame; $p++) { $buf[$p] = 0 }
                }
                $aoffs[$i]  = $cursor
                $asizes[$i] = [uint32]$bytesThisFrame
                $bw.Write($buf)
                $cursor += [uint32]$bytesThisFrame
            } else {
                $aoffs[$i]  = 0
                $asizes[$i] = 0
            }

            if (($i + 1) % 60 -eq 0 -or $i -eq $frameCount - 1) {
                $pct = [int](($i + 1) * 100 / $frameCount)
                Write-Progress -Activity "writing kvid" -Status "$($i+1)/$frameCount" -PercentComplete $pct
            }
        }
        if ($audioStream) { $audioStream.Close() }

        # Index
        $usPerFrame = [uint64](1000000 / $Fps)
        for ($i = 0; $i -lt $frameCount; $i++) {
            $bw.Write([uint32]$offsets[$i])
            $bw.Write([uint32]$sizes[$i])
            $bw.Write([uint32]$aoffs[$i])
            $bw.Write([uint32]$asizes[$i])
            $bw.Write([uint64]([uint64]$i * $usPerFrame))     # dts_us
            $bw.Write([uint64]0)                              # reserved
        }

        $bw.Flush()
    } finally {
        $fs.Close()
    }

    Write-Progress -Activity "writing kvid" -Completed
    $outSize = (Get-Item $OutputPath).Length
    Write-Host ""
    Write-Host ("done. output: {0:N0} bytes ({1:N2} MB)" -f $outSize, ($outSize/1MB)) -ForegroundColor Green
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

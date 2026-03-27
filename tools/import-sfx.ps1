[CmdletBinding()]
param(
    [string]$SourceDir = "E:\WinDownloads\Watcher表情总览_数据表_表格_附件",
    [string]$FfmpegPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-FfmpegPath {
    param(
        [string]$RequestedPath
    )

    if ($RequestedPath) {
        if (Test-Path $RequestedPath) {
            return (Resolve-Path $RequestedPath).Path
        }

        throw "未找到 ffmpeg: $RequestedPath"
    }

    $command = Get-Command "ffmpeg" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "C:\ffmpeg\bin\ffmpeg.exe",
        "C:\ProgramData\chocolatey\bin\ffmpeg.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ffmpeg.exe",
        "$env:USERPROFILE\scoop\apps\ffmpeg\current\bin\ffmpeg.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "未找到 ffmpeg。请安装 ffmpeg，或通过 -FfmpegPath 显式传入路径。"
}

$repoRoot = Resolve-RepoRoot
$resolvedSourceDir = (Resolve-Path $SourceDir).Path
$resolvedFfmpegPath = Resolve-FfmpegPath -RequestedPath $FfmpegPath
$sfxDir = Join-Path $repoRoot "firmware\s3\spiffs\sfx"
$soundIds = @("boot", "error", "happy", "standby", "thinking")

if (-not (Test-Path $sfxDir)) {
    $null = New-Item -ItemType Directory -Path $sfxDir -Force
}

$manifestSounds = [ordered]@{}

Write-Host "Source : $resolvedSourceDir"
Write-Host "ffmpeg : $resolvedFfmpegPath"
Write-Host "Target : $sfxDir"

foreach ($soundId in $soundIds) {
    $inputPath = Join-Path $resolvedSourceDir "$soundId.mp3"
    $outputPath = Join-Path $sfxDir "$soundId.pcm"

    if (-not (Test-Path $inputPath)) {
        throw "缺少源文件: $inputPath"
    }

    Write-Host "Converting $soundId.mp3 -> $soundId.pcm"
    & $resolvedFfmpegPath -y -i $inputPath -f s16le -acodec pcm_s16le -ac 1 -ar 24000 $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg 转换失败: $inputPath"
    }

    $manifestSounds[$soundId] = "$soundId.pcm"
}

$manifest = [ordered]@{
    version = "1.0"
    sounds = $manifestSounds
}

$manifestPath = Join-Path $sfxDir "manifest.json"
$manifestJson = $manifest | ConvertTo-Json -Depth 4
Set-Content -Path $manifestPath -Value $manifestJson -Encoding UTF8

Write-Host "Updated manifest: $manifestPath"
foreach ($soundId in $soundIds) {
    $pcmPath = Join-Path $sfxDir "$soundId.pcm"
    $pcmSize = (Get-Item $pcmPath).Length
    $durationMs = [int][Math]::Round($pcmSize / 48.0, 0)
    Write-Host ("  {0,-8} {1,8} bytes  ~{2} ms" -f $soundId, $pcmSize, $durationMs)
}

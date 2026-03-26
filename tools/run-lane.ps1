[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Firmware,

    [Parameter(Mandatory = $true)]
    [string]$Feature,

    [Parameter(Mandatory = $true)]
    [string]$DeviceAlias,

    [string]$Operator = $env:USERNAME,

    [string]$BuildPath,

    [string]$DeviceMapPath,

    [switch]$NoBuild,

    [switch]$NoMonitor,

    [int]$MonitorSeconds = 20,

    [int]$MonitorMaxLines = 200,

    [switch]$DryRun,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraIdfArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "codex-device-map.ps1")

function Get-TrackedUtcTimestamp {
    return (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
}

function Get-GitBranchName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    try {
        $branch = git -C $RepoRoot branch --show-current 2>$null
        if ($LASTEXITCODE -eq 0 -and $branch) {
            return $branch.Trim()
        }
    }
    catch {
    }

    return "<unknown>"
}

$repoRoot = Get-CodexRepoRoot -StartPath (Join-Path $PSScriptRoot "..")
$projectPath = Join-Path $repoRoot "firmware\$Firmware"
if (-not (Test-Path $projectPath)) {
    throw "未找到固件目录: $projectPath"
}

$flashScript = Join-Path $projectPath "tools\flash-monitor.ps1"
if (-not (Test-Path $flashScript)) {
    throw "未找到 flash-monitor 脚本: $flashScript"
}

$mapping = Resolve-CodexDeviceMapping -Alias $DeviceAlias -RepoRoot $repoRoot -Firmware $Firmware -DeviceMapPath $DeviceMapPath

$resolvedBuildPath = if ($BuildPath) {
    if ([System.IO.Path]::IsPathRooted($BuildPath)) {
        $BuildPath
    }
    else {
        Join-Path $projectPath $BuildPath
    }
}
else {
    Join-Path $projectPath "build-$DeviceAlias"
}

$resolvedBuildPath = [System.IO.Path]::GetFullPath($resolvedBuildPath)
$laneRoot = Join-Path $repoRoot ".codex\local\lanes\$Operator\$Feature\$DeviceAlias"
$logRoot = Join-Path $repoRoot ".codex\local\logs\$Operator\$Feature\$DeviceAlias"
$null = New-Item -ItemType Directory -Path $laneRoot -Force
$null = New-Item -ItemType Directory -Path $logRoot -Force
$sessionStamp = Get-Date -Format "yyyyMMdd-HHmmss"
$laneFile = Join-Path $laneRoot "latest.json"
$sessionFile = Join-Path $laneRoot "session-$sessionStamp.json"
$transcriptFile = Join-Path $logRoot "lane-$sessionStamp.log"
$serialLogFile = Join-Path $logRoot "serial-$sessionStamp.log"
$branchName = Get-GitBranchName -RepoRoot $repoRoot

$laneRecord = [ordered]@{
    operator        = $Operator
    firmware        = $Firmware
    feature         = $Feature
    worktree        = $repoRoot
    branch          = $branchName
    device_alias    = $DeviceAlias
    build_dir       = $resolvedBuildPath
    test_profile    = "smoke"
    status          = "starting"
    device_map_path = $mapping.SourcePath
    local_port      = $mapping.Port
    serial_log      = $serialLogFile
    updated_at_utc  = Get-TrackedUtcTimestamp
}

$laneJson = $laneRecord | ConvertTo-Json -Depth 6
Set-Content -Path $laneFile -Value $laneJson -Encoding UTF8
Set-Content -Path $sessionFile -Value $laneJson -Encoding UTF8

Write-Host "Lane    : $Operator / $Feature / $DeviceAlias"
Write-Host "Firmware: $Firmware"
Write-Host "Branch  : $branchName"
Write-Host "Build   : $resolvedBuildPath"
Write-Host "Port    : $($mapping.Port) (from $($mapping.SourcePath))"
if (-not $DryRun) {
    Write-Host "LaneLog : $transcriptFile"
    Write-Host "Serial  : $serialLogFile"
}

$flashArgs = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $flashScript,
    "-ProjectPath", $projectPath,
    "-Port", $mapping.Port,
    "-BuildPath", $resolvedBuildPath
)

if ($NoBuild) {
    $flashArgs += "-NoBuild"
}

if ($NoMonitor) {
    $flashArgs += "-NoMonitor"
}

if (-not $NoMonitor -and $MonitorSeconds -gt 0) {
    $flashArgs += "-MonitorSeconds"
    $flashArgs += $MonitorSeconds
}

if (-not $NoMonitor -and $MonitorMaxLines -gt 0) {
    $flashArgs += "-MonitorMaxLines"
    $flashArgs += $MonitorMaxLines
}

if (-not $NoMonitor) {
    $flashArgs += "-MonitorLogPath"
    $flashArgs += $serialLogFile
}

if ($DryRun) {
    $flashArgs += "-DryRun"
}

if ($ExtraIdfArgs) {
    $flashArgs += $ExtraIdfArgs
}

$transcriptStarted = $false
$exitCode = 0

try {
    if (-not $DryRun) {
        Start-Transcript -Path $transcriptFile -Force | Out-Null
        $transcriptStarted = $true
    }

    & powershell @flashArgs
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "flash-monitor 退出码: $exitCode"
    }

    $laneRecord.status = if ($DryRun) { "dry_run" } else { "completed" }
}
catch {
    $laneRecord.status = "failed"
    $laneRecord.error = $_.Exception.Message
    $exitCode = if ($exitCode -ne 0) { $exitCode } else { 1 }
    throw
}
finally {
    $laneRecord.updated_at_utc = Get-TrackedUtcTimestamp
    $laneJson = $laneRecord | ConvertTo-Json -Depth 6
    Set-Content -Path $laneFile -Value $laneJson -Encoding UTF8
    Set-Content -Path $sessionFile -Value $laneJson -Encoding UTF8

    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

exit $exitCode

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Port,

    [Parameter()]
    [string]$ProjectPath,

    [Parameter()]
    [string]$BuildPath,

    [Parameter()]
    [switch]$NoBuild,

    [Parameter()]
    [switch]$DryRun,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraIdfArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $ProjectPath) {
    if (-not $PSScriptRoot) {
        throw "无法解析脚本目录，请显式传入 -ProjectPath。"
    }
    $ProjectPath = Split-Path -Parent $PSScriptRoot
}

function Resolve-ProjectDescriptionPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ResolvedProjectPath,

        [string]$ResolvedBuildPath
    )

    if ($ResolvedBuildPath) {
        $candidate = Join-Path $ResolvedBuildPath "project_description.json"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $defaultCandidate = Join-Path $ResolvedProjectPath "build\project_description.json"
    if (Test-Path $defaultCandidate) {
        return $defaultCandidate
    }

    return $null
}

function Resolve-IdfPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ResolvedProjectPath,

        [string]$ResolvedBuildPath
    )

    if ($env:IDF_PATH -and (Test-Path $env:IDF_PATH)) {
        return $env:IDF_PATH
    }

    $projectDescriptionPath = Resolve-ProjectDescriptionPath -ResolvedProjectPath $ResolvedProjectPath -ResolvedBuildPath $ResolvedBuildPath
    if ($projectDescriptionPath) {
        $projectDescription = Get-Content $projectDescriptionPath -Raw | ConvertFrom-Json
        if ($projectDescription.idf_path -and (Test-Path $projectDescription.idf_path)) {
            return $projectDescription.idf_path
        }
    }

    $fallbacks = @(
        "C:\Espressif\frameworks\esp-idf-v5.2.1",
        "C:\Espressif\frameworks\esp-idf"
    )

    foreach ($candidate in $fallbacks) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "未找到 ESP-IDF。请先设置 IDF_PATH，或确保 build/project_description.json 中包含有效的 idf_path。"
}

function Resolve-FlashPort {
    param(
        [string]$RequestedPort
    )

    if ($RequestedPort) {
        return $RequestedPort.ToUpperInvariant()
    }

    $ports = [System.IO.Ports.SerialPort]::GetPortNames() |
        Where-Object { $_ -match '^COM\d+$' } |
        Sort-Object { [int]($_ -replace '^COM', '') }

    if (-not $ports -or $ports.Count -eq 0) {
        throw "未检测到可用串口，无法自动选择烧录端口。"
    }

    return $ports[-1].ToUpperInvariant()
}

$resolvedProjectPath = (Resolve-Path $ProjectPath).Path
$resolvedBuildPath = $null
if ($BuildPath) {
    $resolvedBuildPath = (Resolve-Path $BuildPath).Path
}

$resolvedPort = Resolve-FlashPort -RequestedPort $Port

$idfPath = Resolve-IdfPath -ResolvedProjectPath $resolvedProjectPath -ResolvedBuildPath $resolvedBuildPath
$idfExportScript = Join-Path $idfPath "export.ps1"

if (-not (Test-Path $idfExportScript)) {
    throw "未找到 ESP-IDF 导出脚本: $idfExportScript"
}

$idfArgs = @()
if ($resolvedBuildPath) {
    $idfArgs += "-B"
    $idfArgs += $resolvedBuildPath
}

$idfArgs += "-p"
$idfArgs += $resolvedPort

if (-not $NoBuild) {
    $idfArgs += "build"
}

$idfArgs += "flash"
$idfArgs += "monitor"

if ($ExtraIdfArgs) {
    $idfArgs += $ExtraIdfArgs
}

Write-Host "Project : $resolvedProjectPath"
if ($resolvedBuildPath) {
    Write-Host "Build   : $resolvedBuildPath"
}
Write-Host "IDF     : $idfPath"
Write-Host "Port    : $resolvedPort"
Write-Host "Command : idf.py $($idfArgs -join ' ')"

if ($DryRun) {
    exit 0
}

Push-Location $resolvedProjectPath
try {
    # ESP-IDF's export.ps1 probes $IsWindows before defining it in some shells.
    if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
        $IsWindows = $true
    }
    . $idfExportScript
    if (-not (Get-Command "idf.py" -ErrorAction SilentlyContinue)) {
        throw "ESP-IDF 环境已加载，但未找到 idf.py 函数。"
    }

    & idf.py @idfArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}

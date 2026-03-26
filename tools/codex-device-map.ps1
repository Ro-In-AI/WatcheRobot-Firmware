[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-CodexRepoRoot {
    [CmdletBinding()]
    param(
        [string]$StartPath = $PSScriptRoot
    )

    $resolved = (Resolve-Path $StartPath).Path
    $current = $resolved

    while ($true) {
        if (Test-Path (Join-Path $current ".git")) {
            return $current
        }

        $parent = Split-Path -Parent $current
        if (-not $parent -or $parent -eq $current) {
            break
        }

        $current = $parent
    }

    throw "未找到仓库根目录，请从 Git worktree 内运行。"
}

function Get-CodexDeviceMapPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [string]$DeviceMapPath
    )

    if ($DeviceMapPath) {
        if ([System.IO.Path]::IsPathRooted($DeviceMapPath)) {
            return $DeviceMapPath
        }

        return Join-Path $RepoRoot $DeviceMapPath
    }

    if ($env:CODEX_DEVICE_MAP_PATH) {
        if ([System.IO.Path]::IsPathRooted($env:CODEX_DEVICE_MAP_PATH)) {
            return $env:CODEX_DEVICE_MAP_PATH
        }

        return Join-Path $RepoRoot $env:CODEX_DEVICE_MAP_PATH
    }

    return Join-Path $RepoRoot ".codex\local\device-map.toml"
}

function Read-CodexDeviceMap {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        throw "未找到设备映射文件: $Path。请先复制 .codex\device-map.example.toml 到 .codex\local\device-map.toml，并填写本机串口。"
    }

    $devices = @{}
    $currentAlias = $null
    $lineNumber = 0

    foreach ($rawLine in [System.IO.File]::ReadLines($Path)) {
        $lineNumber += 1
        $line = $rawLine.Trim()

        if (-not $line -or $line.StartsWith("#")) {
            continue
        }

        if ($line -match '^\[(?<section>[^\]]+)\]$') {
            $section = $Matches.section
            if ($section -match '^devices\.(?<alias>[A-Za-z0-9._-]+)$') {
                $currentAlias = $Matches.alias
                if (-not $devices.ContainsKey($currentAlias)) {
                    $devices[$currentAlias] = @{}
                }
                continue
            }

            $currentAlias = $null
            continue
        }

        if (-not $currentAlias) {
            continue
        }

        if ($line -notmatch '^(?<key>[A-Za-z0-9_-]+)\s*=\s*"(?<value>[^"]*)"\s*$') {
            throw "设备映射文件格式错误: ${Path}:$lineNumber -> $rawLine"
        }

        $devices[$currentAlias][$Matches.key] = $Matches.value
    }

    return $devices
}

function Resolve-CodexDeviceMapping {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Alias,

        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [string]$Firmware,

        [string]$DeviceMapPath
    )

    $mapPath = Get-CodexDeviceMapPath -RepoRoot $RepoRoot -DeviceMapPath $DeviceMapPath
    $resolvedMapPath = $mapPath
    if (Test-Path $mapPath) {
        $resolvedMapPath = (Resolve-Path $mapPath).Path
    }

    $devices = Read-CodexDeviceMap -Path $resolvedMapPath

    if (-not $devices.ContainsKey($Alias)) {
        throw "未在设备映射文件中找到别名 '$Alias'。请在 $resolvedMapPath 中添加 [devices.$Alias]。"
    }

    $entry = $devices[$Alias]
    $mappedFirmware = [string]$entry["firmware"]
    $mappedPort = [string]$entry["port"]

    if (-not $mappedPort) {
        throw "设备别名 '$Alias' 缺少 port 字段，请在 $resolvedMapPath 中补齐。"
    }

    if ($Firmware -and $mappedFirmware -and $mappedFirmware -ne $Firmware) {
        throw "设备别名 '$Alias' 的 firmware=$mappedFirmware 与当前 lane 的 firmware=$Firmware 不一致。"
    }

    return [pscustomobject]@{
        Alias       = $Alias
        Firmware    = $mappedFirmware
        Port        = $mappedPort
        SourcePath  = $resolvedMapPath
    }
}

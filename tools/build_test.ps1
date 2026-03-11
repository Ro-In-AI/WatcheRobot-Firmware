# Build Test Script for WatcheRobot Firmware
# Run this in ESP-IDF PowerShell environment
#
# Usage:
#   C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1
#   cd D:\GithubRep\WatcheRobot-Firmware\firmware\s3
#   powershell -File ../../tools/build_test.ps1

param(
    [string]$Target = "esp32s3",
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  WatcheRobot Firmware Build Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$StartTime = Get-Date

# Navigate to firmware directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FirmwareDir = Join-Path $ScriptDir "..\firmware\s3"

Push-Location $FirmwareDir

try {
    # Step 1: Clean if requested
    if ($Clean) {
        Write-Host "[1/4] Cleaning build..." -ForegroundColor Yellow
        idf.py fullclean
        if ($LASTEXITCODE -ne 0) {
            throw "Clean failed"
        }
    }

    # Step 2: Set target
    Write-Host "[2/4] Setting target to $Target..." -ForegroundColor Yellow
    idf.py set-target $Target 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Set target failed"
    }

    # Step 3: Build
    Write-Host "[3/4] Building..." -ForegroundColor Yellow
    $BuildOutput = idf.py build 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host $BuildOutput
        throw "Build failed"
    }

    # Step 4: Verify output
    Write-Host "[4/4] Verifying output..." -ForegroundColor Yellow
    $BinaryPath = "build\WatcheRobot-S3.bin"
    if (Test-Path $BinaryPath) {
        $FileSize = (Get-Item $BinaryPath).Length / 1MB
        Write-Host "Binary size: $([math]::Round($FileSize, 2)) MB" -ForegroundColor Green
    } else {
        throw "Binary not found: $BinaryPath"
    }

    $EndTime = Get-Date
    $Duration = $EndTime - $StartTime

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  BUILD SUCCESS!" -ForegroundColor Green
    Write-Host "  Duration: $($Duration.TotalSeconds.ToString('F1')) seconds" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green

} catch {
    $EndTime = Get-Date
    $Duration = $EndTime - $StartTime

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "  BUILD FAILED!" -ForegroundColor Red
    Write-Host "  Error: $_" -ForegroundColor Red
    Write-Host "  Duration: $($Duration.TotalSeconds.ToString('F1')) seconds" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red

    exit 1
} finally {
    Pop-Location
}

<#
.SYNOPSIS
    Build firmware for ESP32-S3 (DevKitC-1 N16R8).
.PARAMETER FullClean
    Perform a clean rebuild (idf.py fullclean).
#>
param(
    [switch]$FullClean
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FirmwareDir = Split-Path -Parent $ScriptDir

Set-Location $FirmwareDir

if ($FullClean) {
    Write-Host "Performing fullclean..." -ForegroundColor Yellow
    idf.py fullclean
}

Write-Host "Building firmware for ESP32-S3 (WROOM-1-N16R8)..." -ForegroundColor Cyan
idf.py build

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nESP32-S3 Firmware Build Successful!" -ForegroundColor Green
} else {
    Write-Error "Build failed with exit code $LASTEXITCODE"
}

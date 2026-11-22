<#
.SYNOPSIS
    Install the firmware on a new ESP32-S3 board and show its Wi-Fi password.
.PARAMETER Port
    COM port (for example 5 or COM5). Asked for when omitted.
#>
param([string]$Port = "")

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "ap_password.ps1")

$PackageRoot = Split-Path $PSScriptRoot -Parent
$esptool = Join-Path $PSScriptRoot "esptool.exe"
$factory = Join-Path $PackageRoot "firmware\factory.bin"

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Fish Pump - firmware installer            " -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Plug the USB cable into the port labelled COM (or UART), not the one labelled USB." -ForegroundColor Yellow

if (-not (Test-Path $esptool)) { Write-Host "ERROR: esptool.exe not found in tools\" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
if (-not (Test-Path $factory)) { Write-Host "ERROR: missing $factory" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
if (-not $Port) { $Port = Select-ComPort }
if ($Port -match '^\d+$') { $Port = "COM$Port" }

Write-Host "Installing on $Port (the board is erased first) ..." -ForegroundColor Cyan
# factory.bin is the whole flash image starting at address 0x0; "keep" leaves the flash settings the build chose.
& $esptool --chip esp32s3 --port $Port --baud 460800 write_flash --erase-all --flash-mode keep --flash-freq keep --flash-size keep 0x0 $factory
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: install failed. Check the cable and use the COM/UART port." -ForegroundColor Red
    Read-Host "Press Enter to exit"; exit 1
}
Write-Host "[OK] Firmware installed." -ForegroundColor Green

Start-Sleep -Milliseconds 1500
$mac = Read-BoardMac -EsptoolExe $esptool -Port $Port
if (-not $mac) {
    Write-Host "Could not read the MAC. Run SHOW_PASSWORD.bat later." -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "  Wi-Fi name    : FishPump-Setup"
    Write-Host "  Wi-Fi password: $(Get-ApPassword -Mac $mac)  (unique to this board; write it on a label)" -ForegroundColor Yellow
    Write-Host "  Web page      : http://192.168.4.1  (login: admin, you will be asked to change the password)"
}
Write-Host ""
Read-Host "Press Enter to exit"

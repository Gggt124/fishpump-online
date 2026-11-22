<#
.SYNOPSIS
    Erase the whole flash of the board (Wi-Fi settings, data and firmware).
.PARAMETER Port
    COM port (for example 5 or COM5). Asked for when omitted.
#>
param([string]$Port = "")

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "ap_password.ps1")

$esptool = Join-Path $PSScriptRoot "esptool.exe"
if (-not (Test-Path $esptool)) { Write-Host "ERROR: esptool.exe not found in tools\" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
if (-not $Port) { $Port = Select-ComPort }
if ($Port -match '^\d+$') { $Port = "COM$Port" }

Write-Host "This erases EVERYTHING on the board at $Port. Run FLASH.bat afterwards to install the firmware again." -ForegroundColor Yellow
if ((Read-Host "Type YES to continue") -ne "YES") { Write-Host "Cancelled."; exit 0 }

& $esptool --chip esp32s3 --baud 460800 --port $Port erase_flash
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: erase failed" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
Write-Host "[OK] Flash erased." -ForegroundColor Green
Read-Host "Press Enter to exit"

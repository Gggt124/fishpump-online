<#
.SYNOPSIS
    Read the board MAC over USB and show its FishPump-Setup Wi-Fi password.
.PARAMETER Port
    COM port (for example 5 or COM5). Asked for when omitted.
.PARAMETER SelfTest
    Check the password formula against a fixed test vector and exit (no board needed).
#>
param(
    [string]$Port = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "ap_password.ps1")

if ($SelfTest) {
    # Synthetic MAC, not a real board: bytes 2..5 = 22 33 44 55, XOR 0x5A = 78 69 1E 0F
    $got = Get-ApPassword -Mac ([byte[]](0x00, 0x11, 0x22, 0x33, 0x44, 0x55))
    if ($got -eq "78691E0F") { Write-Host "OK"; exit 0 }
    Write-Host "FAIL: expected 78691E0F, got $got"; exit 1
}

$esptool = Join-Path $PSScriptRoot "esptool.exe"
if (-not (Test-Path $esptool)) { Write-Host "ERROR: esptool.exe not found in tools\" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }
if (-not $Port) { $Port = Select-ComPort }
if ($Port -match '^\d+$') { $Port = "COM$Port" }

Write-Host "Reading the board MAC on $Port ..." -ForegroundColor Cyan
$mac = Read-BoardMac -EsptoolExe $esptool -Port $Port
if (-not $mac) {
    Write-Host "ERROR: could not read the MAC. Check the cable and use the COM/UART port." -ForegroundColor Red
    Read-Host "Press Enter to exit"; exit 1
}
Write-Host ""
Write-Host "  Wi-Fi name    : FishPump-Setup"
Write-Host "  Wi-Fi password: $(Get-ApPassword -Mac $mac)" -ForegroundColor Yellow
Write-Host "  Write it on a label and stick it on the device."
Write-Host ""
Read-Host "Press Enter to exit"

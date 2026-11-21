@echo off
:: Shows the Wi-Fi password of the connected board: runs tools\show_ap_password.ps1.
PowerShell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\show_ap_password.ps1"
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] PowerShell script exited with code %errorlevel%
    pause
)

@echo off
:: Installs the firmware on a new board: runs tools\flash_and_show.ps1 from the folder this file is in.
PowerShell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\flash_and_show.ps1"
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] PowerShell script exited with code %errorlevel%
    pause
)

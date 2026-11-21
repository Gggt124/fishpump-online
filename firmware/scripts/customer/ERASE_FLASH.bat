@echo off
:: Erases the whole board: runs tools\erase_flash.ps1 from the folder this file is in.
PowerShell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\erase_flash.ps1"
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] PowerShell script exited with code %errorlevel%
    pause
)

<#
.SYNOPSIS
    Build the sensor self-test app and run it in QEMU. Exit 0 on "SELFTEST PASS".
#>
$ErrorActionPreference = "Stop"
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = Join-Path $AppDir "build\qemu.log"

idf.py -C $AppDir build | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "self-test build failed"; exit 2 }

$job = Start-Job -ScriptBlock {
    param($dir, $log)
    idf.py -C $dir qemu *> $log
} -ArgumentList $AppDir, $Log

$result = $null
for ($i = 0; $i -lt 120 -and -not $result; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $Log) {
        $result = Select-String -Path $Log -Pattern "SELFTEST (PASS|FAIL)" | Select-Object -First 1
    }
}
Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not $result) { Write-Error "no SELFTEST line within 60 s (see $Log)"; exit 3 }
Select-String -Path $Log -Pattern "FAIL " | ForEach-Object { $_.Line }
$result.Line
if ($result.Line -match "SELFTEST PASS") { exit 0 } else { exit 1 }

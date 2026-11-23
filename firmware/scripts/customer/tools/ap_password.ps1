# Shared helpers for the customer tools (dot-sourced by the other scripts in this folder).
# ASCII only: Windows PowerShell 5.1 reads BOM-less files in the ANSI code page.

# The FishPump-Setup Wi-Fi password is derived from the board MAC exactly like
# wifi_manager_build_ap_password() in the firmware: bytes 2..5 of the MAC, each
# XOR 0x5A, written as 8 upper-case hex characters.
function Get-ApPassword {
    param([Parameter(Mandatory = $true)][byte[]]$Mac)
    if ($Mac.Count -ne 6) { throw "MAC address must be 6 bytes" }
    $salt = 0x5A
    return ("{0:X2}{1:X2}{2:X2}{3:X2}" -f ($Mac[2] -bxor $salt), ($Mac[3] -bxor $salt), ($Mac[4] -bxor $salt), ($Mac[5] -bxor $salt))
}

# Reads the MAC of the connected board with esptool. Returns [byte[]] or $null.
function Read-BoardMac {
    param([string]$EsptoolExe, [string]$Port, [string]$Chip = "esp32s3", [string]$Baud = "460800")
    $out = & $EsptoolExe --chip $Chip --baud $Baud --port $Port read-mac 2>&1
    $line = $out | Select-String -Pattern "MAC:\s*([0-9a-fA-F:]{17})" | Select-Object -First 1
    if (-not $line) { return $null }
    return [byte[]]($line.Matches.Groups[1].Value.Trim() -split ":" | ForEach-Object { [Convert]::ToInt32($_, 16) })
}

# Asks for a COM port when none was given. Returns for example "COM5".
function Select-ComPort {
    while ($true) {
        $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
        if ($ports.Count -eq 0) {
            Read-Host "No COM port found. Plug in the board, then press Enter to scan again"
            continue
        }
        Write-Host "Available COM ports: $($ports -join ', ')" -ForegroundColor Cyan
        $choice = (Read-Host "Type the port number (for example 5), or press Enter if only one is listed. R = rescan").Trim()
        if ($choice -match '^[rR]$') { continue }
        if ($choice -eq "" -and $ports.Count -eq 1) { return $ports[0] }
        if ($choice -match '^\d+$') { return "COM$choice" }
        if ($choice -ne "") { return $choice }
        Write-Host "Several ports are listed; please type one." -ForegroundColor Red
    }
}

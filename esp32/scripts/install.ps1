# Copy esp32/src/* to a connected ESP32 via mpremote (PowerShell).
#
# Prerequisite: pip install mpremote
#
# Usage:
#   esp32\scripts\install.ps1 COM7

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$Port
)

$ErrorActionPreference = "Stop"

$srcDir = Join-Path (Split-Path -Parent $PSCommandPath) "..\src" | Resolve-Path

if (-not (Test-Path $srcDir)) {
    Write-Error "Source directory not found: $srcDir"
    exit 1
}

if (-not (Get-Command mpremote -ErrorAction SilentlyContinue)) {
    Write-Error "mpremote not installed. Run: pip install mpremote"
    exit 1
}

Write-Host "==> Copying esp32/src/* to $Port..." -ForegroundColor Cyan
mpremote connect $Port cp -r "$srcDir\." ":"

Write-Host ""
Write-Host "==> Verifying file listing on device:" -ForegroundColor Cyan
mpremote connect $Port ls

Write-Host ""
Write-Host "Install complete. Soft-reboot the board to run main.py:" -ForegroundColor Green
Write-Host "  mpremote connect $Port reset"

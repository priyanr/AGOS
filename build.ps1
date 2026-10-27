# Antigravity OS Windows PowerShell Build Helper
# Usage: .\build.ps1 [x86_64 | aarch64 | all]

param (
    [string]$Arch = "x86_64"
)

Write-Host ">>> Antigravity OS Build Launcher (Architecture: $Arch) <<<" -ForegroundColor Cyan

# Check if WSL is available
$wslCheck = Get-Command wsl -ErrorAction SilentlyContinue
if (-not $wslCheck) {
    Write-Error "Windows Subsystem for Linux (WSL) is required to build Antigravity OS."
    exit 1
}

# Run build.sh inside WSL
wsl bash -c "./build.sh $Arch"

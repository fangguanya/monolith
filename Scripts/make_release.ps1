# Monolith Release Zip Builder
# Creates a release zip with "Installed": true for Blueprint-only compatibility.
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.5.0"

param(
    [Parameter(Mandatory=$true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

$PluginDir = Split-Path -Parent $PSScriptRoot
$OutputZip = Join-Path (Split-Path -Parent $PluginDir) "Monolith-v$Version.zip"
$TempDir = Join-Path $env:TEMP "Monolith_Release_$Version"

Write-Host "Building Monolith v$Version release zip..." -ForegroundColor Cyan

# Clean temp
if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -ItemType Directory -Path $TempDir | Out-Null

# Copy plugin files, excluding build artifacts and git
$exclude = @('Intermediate', 'Saved', 'DerivedDataCache', '.git', '__pycache__')
$items = Get-ChildItem -Path $PluginDir -Force | Where-Object { $exclude -notcontains $_.Name }
foreach ($item in $items) {
    if ($item.PSIsContainer) {
        Copy-Item $item.FullName -Destination (Join-Path $TempDir $item.Name) -Recurse -Force
    } else {
        Copy-Item $item.FullName -Destination (Join-Path $TempDir $item.Name) -Force
    }
}

# Patch .uplugin: set "Installed": true for Blueprint-only users
$upluginPath = Join-Path $TempDir "Monolith.uplugin"
$content = Get-Content $upluginPath -Raw
$content = $content -replace '"Installed":\s*false', '"Installed": true'
Set-Content $upluginPath $content -NoNewline

Write-Host "  Set 'Installed': true in release .uplugin" -ForegroundColor Green

# Create zip
if (Test-Path $OutputZip) { Remove-Item $OutputZip -Force }
Compress-Archive -Path "$TempDir\*" -DestinationPath $OutputZip -Force

# Clean temp
Remove-Item $TempDir -Recurse -Force

$sizeMB = [math]::Round((Get-Item $OutputZip).Length / 1MB, 1)
Write-Host "Done: $OutputZip ($sizeMB MB)" -ForegroundColor Green

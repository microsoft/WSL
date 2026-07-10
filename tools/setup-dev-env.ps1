<#
.SYNOPSIS
    Sets up the development environment for building WSL.
.DESCRIPTION
    Detects any existing Visual Studio 2026 installation and runs the
    matching WinGet Configuration to install all prerequisites:
    Developer Mode, CMake, Visual Studio 2026, and required workloads
    from .vsconfig.

    If VS 2026 is already installed, the script picks the configuration
    matching that edition (Community, Professional, or Enterprise).
    If no VS 2026 is found, it defaults to Community edition.
.EXAMPLE
    .\tools\setup-dev-env.ps1
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$configDir = Join-Path $repoRoot ".config"

# ── Detect existing VS 2026 edition ─────────────────────────────────
$configFile = "configuration.winget" # default: Community
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $vswhere)
{
    $productId = (& $vswhere -version "[18.0,19.0)" -products * -latest -property productId 2>$null |
        Select-Object -First 1)

    if ($productId) { $productId = $productId.Trim() }

    switch ($productId)
    {
        "Microsoft.VisualStudio.Product.Professional" {
            $configFile = "configuration.vsProfessional.winget"
            Write-Host "Detected VS 2026 Professional - using $configFile" -ForegroundColor Green
        }
        "Microsoft.VisualStudio.Product.Enterprise" {
            $configFile = "configuration.vsEnterprise.winget"
            Write-Host "Detected VS 2026 Enterprise - using $configFile" -ForegroundColor Green
        }
        "Microsoft.VisualStudio.Product.Community" {
            Write-Host "Detected VS 2026 Community - using $configFile" -ForegroundColor Green
        }
        default {
            Write-Host "No VS 2026 found - will install Community edition" -ForegroundColor Yellow
        }
    }
}
else
{
    Write-Host "No VS 2026 found - will install Community edition" -ForegroundColor Yellow
}

$configPath = Join-Path $configDir $configFile
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf))
{
    Write-Host "Configuration file not found: $configPath" -ForegroundColor Red
    Write-Host "Ensure the repository is fully checked out and the .config folder contains $configFile." -ForegroundColor Yellow
    exit 1
}

# ── Run WinGet Configuration ────────────────────────────────────────
Write-Host ""
Write-Host "Running WinGet Configuration ($configFile)..." -ForegroundColor Cyan
Write-Host "  This will install: Developer Mode, CMake, VS 2026 + required components"
Write-Host ""

winget configure --enable
if ($LASTEXITCODE -ne 0)
{
    Write-Host "Failed to enable WinGet configuration feature." -ForegroundColor Red
    exit 1
}

winget configure -f "$configPath" --accept-configuration-agreements
if ($LASTEXITCODE -ne 0)
{
    Write-Host "Failed to apply WinGet configuration file: $configPath" -ForegroundColor Red
    exit 1
}

# ── Done ────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "All prerequisites installed. Ready to build!" -ForegroundColor Green
Write-Host ""
Write-Host "  cmake ."
Write-Host "  cmake --build . -- -m"
Write-Host ""

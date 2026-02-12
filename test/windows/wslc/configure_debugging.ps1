# Configure Visual Studio debugging settings for wsltests project
param(
    [string]$Configuration = "Debug",
    [string]$Platform = "x64",
    [string]$Filter = "*WSLCCLI*UnitTests*"
)

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))

# Find TAEF in packages
$packagesConfig = Join-Path $repoRoot "packages.config"
if (-not (Test-Path $packagesConfig)) {
    Write-Host "ERROR: packages.config not found" -ForegroundColor Red
    exit 1
}

[xml]$packagesXml = Get-Content $packagesConfig
$taefPackage = $packagesXml.packages.package | Where-Object { $_.id -eq "Microsoft.Taef" }
if ($null -eq $taefPackage) {
    Write-Host "ERROR: Microsoft.Taef package not found" -ForegroundColor Red
    exit 1
}

$taefVersion = $taefPackage.version
$teExePath = "`$(SolutionDir)packages\Microsoft.Taef.$taefVersion\build\Binaries\`$(Platform)\te.exe"

# Find the wsltests.vcxproj file
$vcxprojPath = Join-Path $repoRoot "test\windows\wsltests.vcxproj"
if (-not (Test-Path $vcxprojPath)) {
    Write-Host "ERROR: wsltests.vcxproj not found at: $vcxprojPath" -ForegroundColor Red
    exit 1
}

# Create the .user file path
$userFilePath = "$vcxprojPath.user"

# Create the XML content for debugging configuration
$userFileContent = @"
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='$Configuration|$Platform'">
    <LocalDebuggerCommand>$teExePath</LocalDebuggerCommand>
    <LocalDebuggerCommandArguments>`$(TargetPath) /name:$Filter /inproc /p:Version=2 /p:AllowUnsigned=1</LocalDebuggerCommandArguments>
    <LocalDebuggerWorkingDirectory>`$(SolutionDir)</LocalDebuggerWorkingDirectory>
    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
  </PropertyGroup>
  <PropertyGroup>
    <ShowAllFiles>false</ShowAllFiles>
  </PropertyGroup>
</Project>
"@

# Write the .user file
Set-Content -Path $userFilePath -Value $userFileContent -Encoding UTF8

Write-Host ""
Write-Host "Successfully configured debugging for wsltests!" -ForegroundColor Green
Write-Host ""

# Try to reload the solution in Visual Studio if it's running
$reloaded = $false
try {
    # Try VS 2022 first (DTE version 17.0)
    $dte = [System.Runtime.InteropServices.Marshal]::GetActiveObject("VisualStudio.DTE.17.0")
    if ($dte -and $dte.Solution.IsOpen) {
        $solutionPath = $dte.Solution.FullName
        Write-Host "Detected Visual Studio 2022, reloading solution..." -ForegroundColor Yellow
        
        # Close and reopen
        $dte.Solution.Close($false)  # Don't save
        Start-Sleep -Milliseconds 500
        $dte.Solution.Open($solutionPath)
        
        $reloaded = $true
        Write-Host "Solution reloaded successfully!" -ForegroundColor Green
    }
} catch {
    # Try VS 2019 (DTE version 16.0)
    try {
        $dte = [System.Runtime.InteropServices.Marshal]::GetActiveObject("VisualStudio.DTE.16.0")
        if ($dte -and $dte.Solution.IsOpen) {
            $solutionPath = $dte.Solution.FullName
            Write-Host "Detected Visual Studio 2019, reloading solution..." -ForegroundColor Yellow
            $dte.Solution.Close($false)
            Start-Sleep -Milliseconds 500
            $dte.Solution.Open($solutionPath)
            $reloaded = $true
            Write-Host "Solution reloaded successfully!" -ForegroundColor Green
        }
    } catch {
        # Visual Studio not running or automation failed
    }
}

if (-not $reloaded) {
    Write-Host ""
    Write-Host "Could not auto-reload solution. Please reload manually in Visual Studio:" -ForegroundColor Yellow
    Write-Host "  Quick reload: Press Ctrl+Shift+F5 (if you set up the shortcut)" -ForegroundColor White
    Write-Host "  OR: File -> Close Solution, then File -> Recent Projects -> wsl.sln" -ForegroundColor White
    Write-Host ""
}

Write-Host ""
Write-Host "Configuration: $Configuration|$Platform" -ForegroundColor Cyan
Write-Host "Test Filter: $Filter" -ForegroundColor Cyan
Write-Host ""
Write-Host "To debug:" -ForegroundColor Yellow
Write-Host "  1. Set wsltests as StartUp Project (right-click -> Set as StartUp Project)" -ForegroundColor White
Write-Host "  2. Set breakpoints in your test code" -ForegroundColor White
Write-Host "  3. Press F5 to start debugging" -ForegroundColor White
Write-Host ""
param (
    [int] $PeriodSeconds = 3600,
    [ValidateNotNullOrEmpty()]
    [string] $Profile = (Join-Path $PSScriptRoot "wsl.wprp")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

trap
{
    try { & wpr.exe -cancel | Out-Null } catch {}
    throw
}

[console]::TreatControlCAsInput = $true


Write-Host "Press any key to exit"

$running = $true
while($running)
{
    & wpr.exe -start $Profile -filemode
    $rotation = (Get-Date).AddSeconds($PeriodSeconds)

    while($rotation -Gt (Get-Date))
    {
        if ($Host.UI.RawUI.KeyAvailable)
        {
            $keyPressed = $Host.UI.RawUI.ReadKey("NoEcho, IncludeKeyUp, IncludeKeyDown")
            if ($keyPressed.KeyDown -eq "True")
            {
                $running = $false
                Write-Host "Exiting"
                break
            }
        }

        [Threading.Thread]::Sleep(1000)
    }

    $file = Get-Date ($rotation.ToUniversalTime()) -UFormat '%Y-%m-%d %H-%M-%S'
    $file += ".etl"
    Write-Host Writing logs to $file
    & wpr.exe -stop $file
}
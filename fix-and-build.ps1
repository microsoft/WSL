$ErrorActionPreference = 'Continue'
$repo = 'C:\Users\gavingarzia\repos\WSL'
$logDir = 'C:\Users\gavingarzia\.copilot\session-state\5e1713ed-368a-4854-8fd8-38a8fa7216bf\files\buildlogs2'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
Set-Location $repo

$branches = @(
    'user/ggarzia/wslc-prune-docker-parity',
    'user/ggarzia/wslc-logs-details',
    'user/ggarzia/wslc-quiet-parity',
    'user/ggarzia/wslc-inspect-size',
    'user/ggarzia/wslc-ps-size',
    'user/ggarzia/wslc-pull-all-tags',
    'user/ggarzia/wslc-images-digests',
    'user/ggarzia/wslc-images-all',
    'user/ggarzia/unity-build'
)

$target = 'test/windows/wslc/WSLCCLIParserUnitTests.cpp'
$old = 'Argument::Create(ArgType::ObjectId, false, Limit::Unlimited)'
$new = 'Argument::Create(ArgType::ObjectId, {.Required = false, .Limit = Limit::Unlimited})'

$summary = @()
foreach ($b in $branches) {
    $short = $b -replace '^user/ggarzia/', ''
    $log = Join-Path $logDir "$short.log"
    Write-Host "=== $short : checkout ==="
    git checkout $b 2>&1 | Out-Null

    # Apply the master-inherited build fix.
    $path = Join-Path $repo $target
    $content = Get-Content $path -Raw
    $patched = $false
    if ($content.Contains($old)) {
        $content = $content.Replace($old, $new)
        Set-Content -Path $path -Value $content -NoNewline
        $patched = $true
    }

    if ($patched) {
        # Format the whole branch diff, per repo convention.
        $base = git merge-base origin/master HEAD
        $files = git diff --name-only $base HEAD | Where-Object { $_ -match '\.(h|cpp|hpp|c|hxx)$' }
        $files += $target
        $files | Sort-Object -Unique | Out-File -Encoding ascii "$env:TEMP\fmt_$short.txt"
        powershell -ExecutionPolicy Bypass -File .\FormatSource.ps1 -ChangesFile "$env:TEMP\fmt_$short.txt" 2>&1 | Out-Null
        git add -A
        git commit --amend --no-edit 2>&1 | Out-Null
    }

    # Build, retrying once to clear the documented stale-PCH (C4651/C2220) storm.
    $code = 1
    $mins = 0
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        Write-Host "=== $short : build attempt $attempt ==="
        $start = Get-Date
        cmake --build . -- -m 2>&1 | Tee-Object -FilePath $log | Out-Null
        $code = $LASTEXITCODE
        $mins = [math]::Round(((Get-Date) - $start).TotalMinutes, 1)
        if ($code -eq 0) { break }
    }

    $real = Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+' -ErrorAction SilentlyContinue |
            Where-Object { $_.Line -notmatch 'C2220' } |
            ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique

    $summary += [pscustomobject]@{
        Branch  = $short
        Patched = $patched
        Result  = $(if ($code -eq 0) { 'OK' } else { 'FAILED' })
        Minutes = $mins
        RealErrors = @($real).Count
    }
    Write-Host "=== $short : exit=$code ${mins}min realerrors=$(@($real).Count) ==="
    if ($code -ne 0) { $real | Select-Object -First 5 | ForEach-Object { Write-Host "    $_" } }
}

Write-Host ""
Write-Host "############ SUMMARY ############"
$summary | Format-Table -AutoSize | Out-String -Width 220 | Write-Host

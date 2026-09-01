$ErrorActionPreference = 'Continue'
Set-Location 'C:\Users\gavingarzia\repos\WSL'

$branches = @(
    'user/ggarzia/wslc-prune-docker-parity',
    'user/ggarzia/wslc-logs-details',
    'user/ggarzia/wslc-quiet-parity',
    'user/ggarzia/wslc-inspect-size',
    'user/ggarzia/wslc-ps-size',
    'user/ggarzia/wslc-pull-all-tags',
    'user/ggarzia/wslc-images-digests',
    'user/ggarzia/wslc-images-all',
    'user/ggarzia/unity-build',
    'user/ggarzia/wslc-push-all-tags',
    'user/ggarzia/wslc-cp-follow-link'
)

foreach ($b in $branches) {
    $short = $b -replace '^user/ggarzia/', ''
    git checkout $b 2>&1 | Out-Null
    $out = git merge origin/master 2>&1
    $conflicts = git diff --name-only --diff-filter=U

    if ($conflicts) {
        Write-Host "$short : CONFLICT"
        $conflicts | ForEach-Object { Write-Host "    $_" }
        git merge --abort 2>&1 | Out-Null
    }
    elseif ($out -match 'Already up to date') {
        Write-Host "$short : already up to date"
    }
    else {
        Write-Host "$short : MERGED CLEAN"
    }
}

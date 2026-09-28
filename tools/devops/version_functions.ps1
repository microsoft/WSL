Set-StrictMode -Version Latest

function Get-Current-Commit-Hash()
{
    return ([string](git log -1 --pretty=%h)).Trim()
}

function Get-VersionInfo
{
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Nightly)

    $ErrorActionPreference = "Stop"

    if ($Nightly)
    {
        $suffix = 'nightly'
    }
    else
    {
        $suffix = 'build'
    }

    $output = git.exe describe --tags --match *.*.* --abbrev=1
    if ($LastExitCode -ne 0)
    {
        throw "git describe exited with error status: $LastExitCode. Make sure the main branch and tags are available."
    }

    $versionInfo = $output.split('-')
    if ($versionInfo.Length -lt 1)
    {
        throw "Unexpected output from git describe: $output"
    }

    $version = $versionInfo[0]
    if ($versionInfo.Length -lt 2)
    {
        $revision = 0 # This path is taken when the commit is directly on a tag
    }
    else
    {
        $revision = $versionInfo[1]
    }

    $result = @{
                'MsixVersion' = "$version.$revision"
                'NugetVersion' = "$version-$suffix{0:d3}" -f $revision
              }

    return $result
}

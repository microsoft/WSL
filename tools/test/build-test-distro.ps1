<#
.SYNOPSIS
    Takes in an exported distribution, installs dependencies for testing, cleans unnecessary components, and exports it for later use.
.PARAMETER Base
    Base distribution to use.
.PARAMETER OutputTarPath
    Path to write the test distro .tar to.
#>

[CmdletBinding()]
Param ($Base)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Run {
    [CmdletBinding()]
    param([scriptblock]$cmd)

    Invoke-Command -ScriptBlock $cmd
    if ($lastexitcode -ne 0) {
        throw ("$cmd failed with exit code: " + $lastexitcode)
    }
}

function RunInDistro {
    [CmdletBinding()]
    param([string]$cmd)
    & cmd /c "wsl.exe -d test_distro -- $cmd"
    if ($lastexitcode -ne 0) {
        throw ("wsl.exe -d test_distro -- $cmd failed with exit code: " + $lastexitcode)
    }
}

$git_version = (git describe --tags).split('-')
$version = "$($git_version[0])-$($git_version[1])"

echo "Building test_distro version: $version"

Run { wsl.exe --install $Base --name test_distro --version 2 --no-launch }

RunInDistro("apt update")
RunInDistro("apt install daemonize libmount-dev genisoimage dosfstools make gcc socat systemd libpam-systemd bind9-dnsutils xz-utils bzip2 -f -y --no-install-recommends")
RunInDistro("apt purge cpio isc-dhcp-client isc-dhcp-common nftables rsyslog vim vim-tiny vim-common whiptail xxd init genisoimage tasksel kmod mawk udev cron -y -f --allow-remove-essential")
RunInDistro("apt-get autopurge -y")
RunInDistro("apt clean")
RunInDistro("umount /usr/lib/wsl/drivers")
RunInDistro("umount /usr/lib/wsl/lib")
RunInDistro("rm -rf /etc/wsl-distribution.conf /etc/wsl.conf /usr/share/doc/* /var/lib/apt/lists/* /var/log/* /var/cache/debconf/* /var/cache/ldconfig/* /usr/lib/wsl /usr/share/{gdb,vim,zsh,man}")
RunInDistro('rm -rf -- $(ls /usr/share/locale ^| grep -vE "en|locale.alias")')
RunInDistro("rm /usr/lib/systemd/user/{systemd-tmpfiles-setup.service,systemd-tmpfiles-clean.timer,systemd-tmpfiles-clean.service}")
RunInDistro("rm /usr/lib/systemd/system/{systemd-tmpfiles-setup-dev.service,systemd-tmpfiles-setup.service,systemd-tmpfiles-clean.timer,systemd-tmpfiles-clean.service} /lib/systemd/system/{kmod-static-nodes.service,kmod.service,sysinit.target.wants/kmod-static-nodes.service}")
Run { wsl.exe --export test_distro "test_distro.tar" }
Run { wsl.exe xz -e9 "test_distro.tar" }

& "$PSScriptRoot/../../_deps/nuget.exe" pack Microsoft.WSL.TestDistro.nuspec -Properties  version=$version
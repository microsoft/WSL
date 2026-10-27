# WSLC standalone developer cluster prototype

This branch contains only the standalone developer cluster additions to WSLC.
The scenario is local, disposable Kubernetes development without Azure
enrollment, but with internet access. It is not an air-gapped installation.
Managed AKS Arc provisioning and Azure Kubernetes Fleet operations are not
included.

## Prototype source

- Repository: <https://github.com/microsoft/WSL>
- Branch:
  [`user/ptrivedi/wslc-developer-prototype`](https://github.com/microsoft/WSL/tree/user/ptrivedi/wslc-developer-prototype)

The current implementation still delegates to the external AKS Desktop SDK
`aks-desktop.exe` executable using its WSL driver. It has not yet been converted to
run the agent inside the regular WSLC utility VM.

Clusters are disposable development environments. In-place upgrades and
recovery are outside the initial offering; delete and recreate instead.
The prototype retains status, kubeconfig, diagnostics, and SDK catalog
utilities alongside create and delete.

## Build

Use an x64 Windows development machine with WSL installed and the build
prerequisites described in the repository's contributor instructions.

```powershell
git clone https://github.com/microsoft/WSL.git
Set-Location .\WSL
git fetch origin user/ptrivedi/wslc-developer-prototype
git switch --track origin/user/ptrivedi/wslc-developer-prototype

Set-ExecutionPolicy -Scope Process Bypass
.\tools\setup-dev-env.ps1
```

Open a new terminal if setup modifies Visual Studio, CMake, or environment
settings. Enable Developer Mode or use an elevated build terminal.

```powershell
cmake .
cmake --build . --config Debug --target wslc
$wslc = "$PWD\bin\x64\Debug\wslc.exe"
```

## External runtime assets

These files are intentionally not committed to the WSL repository and must
be obtained separately:

```text
aks-desktop.exe
aks-bmagent.deb
```

Place them beside `bin\x64\Debug\wslc.exe`, or configure their full paths:

```powershell
$env:AKS_DESKTOP_BIN = "C:\path\to\aks-desktop.exe"
$env:AKSEDGE_AGENT_DEB = "C:\path\to\aks-bmagent.deb"
```

The executable can also be selected with `--aks-desktop-path`. The legacy
`--aksedge-path` and `AKSEDGE_BIN` overrides remain accepted. Do not specify
both path flags. Discovery prefers the path flag, then `AKS_DESKTOP_BIN`,
then `AKSEDGE_BIN`, then a sibling `aks-desktop.exe`, and finally
`aks-desktop.exe` on PATH. An invalid explicit path is an error.
An agent repository can be supplied with `--agent-repo` instead of a local package.

## Create

```powershell
& $wslc cluster create `
  --developer `
  --name dev-cluster `
  --distribution K8s `
  --cni cilium `
  --k8s-version <version> `
  --merge
```

Available options:

```text
--name <cluster-name>
--distro <wsl-distro>
--host-name <host-name>
--node-name <node-name>
--node-ip <ip-address>
--agent-deb <path>
--agent-repo <apt-source>
--api-port <port>
--k8s-version <version>
--pod-cidr <cidr>
--distribution <K8s|K3s>
--cni <cilium|none>
--enable-gpu
--gpu-vendor <vendor>
--network <network>
--output <kubeconfig-file>
--merge
--merge-into <kubeconfig-file>
--aks-desktop-path <path>
```

The SDK defaults include distro `aks-edge`, API port `6443`, and pod CIDR
`10.244.0.0/16`. Use either `--agent-deb` or `--agent-repo`, not both.
Use either `--merge` or `--merge-into`, not both.

No Azure subscription, tenant, authentication, or Fleet options are accepted.

## Delete

```powershell
& $wslc cluster delete --developer --name dev-cluster
```

`remove` and `rm` are aliases for `delete`. Optional arguments are `--distro`,
`--prune-kubeconfig-file`, and `--aks-desktop-path`.

Deletion requests guest removal and kubeconfig pruning from the SDK.
Cluster-local data is disposable.

## Supporting utilities

```powershell
& $wslc cluster status --developer
& $wslc cluster kubeconfig --developer --name dev-cluster --output .\kubeconfig
& $wslc cluster kubeconfig --developer --name dev-cluster --merge
& $wslc cluster kubeconfig --developer --name dev-cluster --merge-into .\kubeconfig

& $wslc cluster diagnostics `
  --developer `
  --name dev-cluster `
  --output .\support `
  --since 2h `
  --redact

& $wslc cluster versions --developer
& $wslc cluster distributions --developer
& $wslc cluster cnis --developer
```

Status accepts `--distro`. Kubeconfig and diagnostics also accept `--distro`;
diagnostics accepts an optional cluster name. All SDK-backed commands accept
`--aks-desktop-path` (or legacy `--aksedge-path`).

## Help

```powershell
& $wslc cluster --help
& $wslc cluster create --help
& $wslc cluster delete --help
```

All leaf commands require `--developer`. Pass `--help` to a command for the
options supported by that build.

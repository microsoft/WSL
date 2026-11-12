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

The current implementation delegates to the external AKS Developer CLI
`aks-devctl.exe` (SDK 0.3.8) using its WSL provider. It has not yet been converted to
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

The SDK executable is intentionally not committed to the WSL repository and
must be obtained separately:

```text
aks-devctl.exe
```

Place it beside `bin\x64\Debug\wslc.exe`, or configure its full path:

```powershell
$env:AKS_DEVCTL_BIN = "C:\path\to\aks-devctl.exe"
```

The executable can also be selected with `--aks-devctl-path`. The legacy
`--aks-desktop-path`, `--aksedge-path`, `AKS_DESKTOP_BIN`, and `AKSEDGE_BIN`
overrides remain accepted, but must point to the new SDK. Specify only one
path flag. Discovery prefers the path flag, then `AKS_DEVCTL_BIN`,
`AKS_DESKTOP_BIN`, `AKSEDGE_BIN`, a sibling `aks-devctl.exe`, and finally
`aks-devctl.exe` on PATH. An invalid explicit path is an error.
Older `aks-desktop.exe` SDK command contracts are no longer supported.
The new SDK also requires its own WSL instance ownership metadata. Clusters
created by the old SDK are not automatically adopted: operations such as
status refuse an existing instance that is not owned by `aks-devctl`. Keep
using the old SDK to manage those clusters, or create a new cluster with a
different name. Updating this wrapper does not migrate or delete them.

The SDK creates `aks-devctl.settings.json` beside its executable when missing.
That directory must be writable. Do not copy another user's settings file;
use generated defaults or configure the SDK explicitly:

```powershell
& .\bin\x64\Debug\aks-devctl.exe setting get --all
& .\bin\x64\Debug\aks-devctl.exe setting schema
& .\bin\x64\Debug\aks-devctl.exe setting set --help
```

Provider settings, including agent installation, connectivity, and CNI
configuration, now live in that file rather than WSLC command flags.
The SDK downloads the Ubuntu image and management-agent packages as needed.
No local `.deb` is required.

## Create

```powershell
& $wslc cluster create `
  --developer `
  --name dev-cluster `
  --distribution K8s `
  --k8s-version v1.36.5 `
  --output .\dev-kubeconfig.yaml
```

Available options:

```text
--name <cluster-name>
--k8s-version <version>
--pod-cidr <cidr>
--distribution <K8s|K3s>
--enable-gpu
--gpu-vendor <vendor>
--nvidia-gpu-sharing <0|2|4>
--output <kubeconfig-file>
--overwrite
--overwrite-existing
--aks-devctl-path <path>
```

On Windows, the cluster name is also the WSL instance name and the Kubernetes
node name. The SDK uses Ubuntu 24.04 and rejects an existing instance with the
same name; it never reuses or overwrites it.
Omitting the version selects the highest non-deprecated version in the SDK's
embedded catalog.

Without `--output`, credentials are merged into `%USERPROFILE%\.kube\config`;
the SDK ignores `KUBECONFIG`. Use `--overwrite-existing` to replace conflicting
entries. With `--output`, the parent directory must exist and an existing file
requires `--overwrite`, which replaces the entire file.

The old `--distro`, `--host-name`, `--node-name`, `--node-ip`, `--agent-repo`,
`--api-port`, `--cni`, `--network`, `--merge`, and `--merge-into` flags are no
longer accepted. Use the cluster name, SDK settings, and output options above.
WSLC retains `--k8s-version` and `--distribution`, translating them to the SDK's
`--kubernetes-version` and `--kubernetes-distribution`.

Create and delete select the SDK's JSON output and stream its progress events
to the terminal.

No Azure subscription, tenant, authentication, or Fleet options are accepted.

## NVIDIA GPU sharing

```powershell
& $wslc cluster create `
  --developer `
  --name gpu-dev `
  --nvidia-gpu-sharing 4 `
  --output .\gpu-dev.yaml
```

`--enable-gpu` configures guest GPU support and installs the device plugin.
`--gpu-vendor nvidia` explicitly selects NVIDIA when GPU support is enabled.
`--nvidia-gpu-sharing` accepts `0` (sharing disabled), `2`, or `4` time-slicing
slots per physical GPU. A nonzero value enables NVIDIA GPU support and sharing
without requiring a separate `--enable-gpu`.
Time-slicing does not provide memory or compute isolation.
GPU operation requires compatible NVIDIA hardware and a Windows driver with
WSL GPU support; enabling the flag does not emulate a GPU.

After creation, inspect the advertised resources:

```powershell
kubectl --kubeconfig .\gpu-dev.yaml get nodes `
  -o 'custom-columns=NAME:.metadata.name,GPU:.status.allocatable.nvidia\.com/gpu'
```

## Delete

```powershell
& $wslc cluster delete --developer --name dev-cluster --yes
```

`remove` and `rm` are aliases for `delete`. The name is required. Omit `--yes`
to require interactive confirmation before deletion.

Deletion permanently removes the named WSL instance and prunes its entries
from the default kubeconfig. Separate kubeconfig files are not changed.
The old `--prune-kubeconfig-file` flag is no longer accepted.
Cluster-local data is disposable. The new SDK does not shut down other WSL
instances during deletion.

## Supporting utilities

```powershell
& $wslc cluster status --developer --name dev-cluster
& $wslc cluster kubeconfig --developer --name dev-cluster --output .\kubeconfig
& $wslc cluster kubeconfig --developer --name dev-cluster --overwrite-existing

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

Status reports environment and agent availability, not Kubernetes readiness.
Kubeconfig retrieval merges into the default host kubeconfig unless `--output`
selects a separate file. Existing contexts or files require
`--overwrite-existing`; an explicitly selected file is replaced in full.
Unlike create, credential retrieval creates missing parent directories.
Kubeconfig YAML on standard output is no longer supported.

Diagnostics requires both `--name` and `--output`. The output is a `.tar.gz`
support bundle; the destination may be a file path or an existing directory.
The `cnis` utility now shows the combined distribution/CNI catalog, just like
`distributions`.
All SDK-backed commands accept `--aks-devctl-path` (or a legacy path alias).

## Help

```powershell
& $wslc cluster --help
& $wslc cluster create --help
& $wslc cluster delete --help
```

All leaf commands require `--developer`. Pass `--help` to a command for the
options supported by that build.

# WSLC cluster and Fleet prototype

This branch adds Kubernetes cluster lifecycle and Azure Kubernetes Fleet
Manager operations to `wslc`.

## Prototype source

- Repository: <https://github.com/microsoft/WSL>
- Branch:
  [`user/ptrivedi/wslc-fleet-prototype`](https://github.com/microsoft/WSL/tree/user/ptrivedi/wslc-fleet-prototype)

The prototype includes:

- Managed AKS Arc cluster creation, deletion, status, and kubeconfig retrieval.
- Standalone developer clusters selected with `--developer`.
- Azure Kubernetes Fleet membership during managed cluster creation.
- Fleet `join`, `leave`, and `status` operations.
- Fleet creation with interactive consent or `--create-fleet`.
- Explicit movement between Fleets with `--move`.

Fleet operations currently support managed AKS Arc clusters only. They are not
supported with `--developer`.

## Build prerequisites

Use an x64 Windows development machine with WSL installed:

```powershell
wsl --install
```

Building the repository requires:

- Developer Mode, or an elevated build terminal.
- CMake 3.25 or newer.
- Visual Studio 2022 with the workloads specified by the repository's
  `.vsconfig`.

The repository can install its required development environment:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\setup-dev-env.ps1
```

Open a new PowerShell terminal if the setup script installs or modifies Visual
Studio, CMake, or environment settings.

The prerequisites can alternatively be installed manually:

```powershell
winget install Kitware.CMake
winget install Microsoft.VisualStudio.2022.Community `
  --override "--wait --quiet --config .vsconfig"
```

Enable Developer Mode in Windows Settings if the build is not running as
Administrator.

## Clone and build

```powershell
git clone https://github.com/microsoft/WSL.git
Set-Location .\WSL

git fetch origin user/ptrivedi/wslc-fleet-prototype
git switch --track origin/user/ptrivedi/wslc-fleet-prototype

cmake .
cmake --build . --config Debug --target wslc
```

The resulting executable is:

```text
bin\x64\Debug\wslc.exe
```

Configure a convenient variable:

```powershell
$wslc = "$PWD\bin\x64\Debug\wslc.exe"
```

Confirm the command surface:

```powershell
& $wslc cluster --help
& $wslc cluster fleet --help
```

This workflow runs `wslc.exe` directly from the build output. To deploy the
complete locally built WSL package instead, build the full solution and install
`bin\x64\Debug\wsl.msi`, or run:

```powershell
powershell .\tools\deploy\deploy-to-host.ps1
```

Deploying the complete WSL build modifies the machine's installed WSL package
and may require elevation. It is not necessary to inspect or invoke the
prototype CLI.

## Standalone developer-flow assets

Standalone developer clusters require two additional runtime files that are
intentionally not committed to the WSL repository:

```text
aksedge-windows-amd64.exe
aks-bmagent.deb
```

Place both files beside the built executable:

```text
bin\x64\Debug\wslc.exe
bin\x64\Debug\aksedge-windows-amd64.exe
bin\x64\Debug\aks-bmagent.deb
```

The files can instead be configured by full path:

```powershell
$env:AKSEDGE_BIN = "C:\path\to\aksedge-windows-amd64.exe"
$env:AKSEDGE_AGENT_DEB = "C:\path\to\aks-bmagent.deb"
```

The executable can also be selected per command:

```text
--aksedge-path C:\path\to\aksedge-windows-amd64.exe
```

## Command overview

```text
wslc cluster create
wslc cluster delete
wslc cluster status
wslc cluster kubeconfig

wslc cluster fleet join
wslc cluster fleet leave
wslc cluster fleet status

wslc cluster diagnostics --developer
wslc cluster versions --developer
wslc cluster distributions --developer
wslc cluster cnis --developer
```

All commands support:

```text
--help, -?
--session <session>
```

## Managed AKS Arc clusters

### Create

```powershell
& $wslc cluster create `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  --tenant-id <tenant-id> `
  [--location <location>] `
  [--distribution <k8s|k3s>] `
  [--distro <wsl-distro>] `
  [--auth-mode <browser|device-code|sp>] `
  [--client-id <service-principal-client-id>] `
  [--client-secret <service-principal-secret>] `
  [--cmp-subscription <subscription-id>] `
  [--cmp-resource-group <resource-group>] `
  [--cmp-name <cmp-name>] `
  [--aksarc-wheel <wsl-path>] `
  [--aksarc-build-id <build-id>] `
  [--enable-gpu]
```

Defaults:

```text
--location eastus
--distribution k8s
--distro aks-edge
```

Service-principal authentication requires:

```powershell
& $wslc cluster create `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  --tenant-id <tenant-id> `
  --auth-mode sp `
  --client-id <client-id> `
  --client-secret <client-secret>
```

### Configuration file

Cluster settings can be provided in a `KEY=VALUE` file:

```powershell
& $wslc cluster create --config .\cluster.env
```

Supported managed-cluster keys include:

```text
SUBSCRIPTION
RESOURCE_GROUP
TENANT_ID
LOCATION
DISTRIBUTION
ENABLE_GPU
AUTH_MODE
AZURE_CLIENT_ID
AZURE_CLIENT_SECRET
DISTRO
CMP_SUBSCRIPTION
CMP_RESOURCE_GROUP
CMP_NAME
AKSARC_WHEEL_PATH
AKSARC_BUILD_ID
```

Fleet keys include:

```text
FLEET_NAME
FLEET_SUBSCRIPTION
FLEET_RESOURCE_GROUP
FLEET_LOCATION
FLEET_MEMBER_NAME
CREATE_FLEET
```

Explicit command-line values override configuration-file values.

### Delete

```powershell
& $wslc cluster delete `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  [--distro <wsl-distro>]
```

Aliases:

```powershell
& $wslc cluster remove ...
& $wslc cluster rm ...
```

### Status

```powershell
& $wslc cluster status `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  [--distro <wsl-distro>]
```

### Kubeconfig

Write the kubeconfig to standard output:

```powershell
& $wslc cluster kubeconfig `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  [--distro <wsl-distro>]
```

Write it to a file:

```powershell
& $wslc cluster kubeconfig `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  --output .\kubeconfig
```

## Fleet operations

Fleet operations apply to managed AKS Arc clusters. They resolve and enroll the
corresponding `Microsoft.Kubernetes/connectedClusters` resource.

### Create a cluster and join an existing Fleet

```powershell
& $wslc cluster create `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --tenant-id <tenant-id> `
  --fleet <fleet-name> `
  [--fleet-subscription <fleet-subscription>] `
  [--fleet-resource-group <fleet-resource-group>] `
  [--fleet-member-name <member-name>]
```

Fleet defaults:

```text
Fleet subscription    = cluster subscription
Fleet resource group  = cluster resource group
Fleet location        = cluster location
Fleet member name     = Arc-connected cluster name
```

### Create a missing Fleet

Interactive use asks permission when the Fleet does not exist. Noninteractive
use must provide `--create-fleet`:

```powershell
& $wslc cluster create `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --tenant-id <tenant-id> `
  --fleet <fleet-name> `
  --fleet-subscription <fleet-subscription> `
  --fleet-resource-group <fleet-resource-group> `
  --fleet-location <location> `
  --create-fleet
```

The prototype creates a hubful Fleet with a managed identity. Azure consequently
creates a managed resource group named similarly to:

```text
FL_<fleet-resource-group>_<fleet-name>_<location>
```

Subscription policies must allow this resource group to be created. For
example, a policy requiring tags on every resource group can block Fleet
creation because Fleet Manager creates this managed resource group itself.

### Join an existing managed cluster

```powershell
& $wslc cluster fleet join `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --fleet <fleet-name> `
  [--fleet-subscription <fleet-subscription>] `
  [--fleet-resource-group <fleet-resource-group>] `
  [--fleet-member-name <member-name>] `
  [--distro <wsl-distro>]
```

`attach` is an alias for `join`:

```powershell
& $wslc cluster fleet attach ...
```

Create a missing Fleet during the join:

```powershell
& $wslc cluster fleet join `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --fleet <fleet-name> `
  --fleet-subscription <fleet-subscription> `
  --fleet-resource-group <fleet-resource-group> `
  --fleet-location <location> `
  --create-fleet
```

### Move a cluster between Fleets

Joining is rejected when the cluster belongs to another Fleet unless `--move`
is specified:

```powershell
& $wslc cluster fleet join `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --fleet <destination-fleet> `
  --fleet-subscription <fleet-subscription> `
  --fleet-resource-group <fleet-resource-group> `
  --move
```

The implementation attempts to restore the previous membership if the
destination join fails.

### Show Fleet membership

```powershell
& $wslc cluster fleet status `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  [--distro <wsl-distro>]
```

The output includes:

```text
Cluster: <connected-cluster>
Fleet: <fleet>
Member name: <member>
Membership resource: <resource-id>
```

### Leave a Fleet

With confirmation:

```powershell
& $wslc cluster fleet leave `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  [--distro <wsl-distro>]
```

Without confirmation:

```powershell
& $wslc cluster fleet leave `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --yes
```

`detach` is an alias for `leave`:

```powershell
& $wslc cluster fleet detach ...
```

Leaving removes only Fleet membership. It does not delete the cluster, Arc
resource, or Fleet.

## Standalone developer clusters

### Create

```powershell
& $wslc cluster create `
  --developer `
  --name <cluster-name> `
  [--distro <wsl-distro>] `
  [--host-name <host-name>] `
  [--node-name <node-name>] `
  [--node-ip <ip-address>] `
  [--agent-deb <path>] `
  [--agent-repo <apt-source>] `
  [--api-port <port>] `
  [--k8s-version <version>] `
  [--pod-cidr <cidr>] `
  [--distribution <K8s|K3s>] `
  [--cni <cilium|none>] `
  [--enable-gpu] `
  [--gpu-vendor <vendor>] `
  [--network <network>] `
  [--output <kubeconfig-file>] `
  [--merge] `
  [--merge-into <kubeconfig-file>] `
  [--aksedge-path <path>]
```

Defaults include:

```text
--distro aks-edge
--api-port 6443
--pod-cidr 10.244.0.0/16
```

Use either `--agent-deb` or `--agent-repo`, not both. Use either `--merge` or
`--merge-into`, not both.

Example:

```powershell
& $wslc cluster create `
  --developer `
  --name dev-cluster `
  --distribution K8s `
  --cni cilium `
  --k8s-version <version> `
  --merge
```

### Delete

```powershell
& $wslc cluster delete `
  --developer `
  [--name <cluster-name>] `
  [--distro <wsl-distro>] `
  [--prune-kubeconfig-file <file>] `
  [--aksedge-path <path>]
```

The `remove` and `rm` aliases are also available.

### Status

```powershell
& $wslc cluster status `
  --developer `
  [--distro <wsl-distro>] `
  [--aksedge-path <path>]
```

### Kubeconfig

Write to standard output:

```powershell
& $wslc cluster kubeconfig `
  --developer `
  --name <cluster-name>
```

Write to a file:

```powershell
& $wslc cluster kubeconfig `
  --developer `
  --name <cluster-name> `
  --output .\kubeconfig
```

Merge into the default host kubeconfig:

```powershell
& $wslc cluster kubeconfig `
  --developer `
  --name <cluster-name> `
  --merge
```

Merge into a specific file:

```powershell
& $wslc cluster kubeconfig `
  --developer `
  --name <cluster-name> `
  --merge-into .\kubeconfig
```

### Diagnostics

```powershell
& $wslc cluster diagnostics `
  --developer `
  [--name <cluster-name>] `
  [--distro <wsl-distro>] `
  [--output <file-or-directory>] `
  [--since <duration>] `
  [--redact] `
  [--aksedge-path <path>]
```

Example:

```powershell
& $wslc cluster diagnostics `
  --developer `
  --name dev-cluster `
  --output .\support `
  --since 2h `
  --redact
```

### SDK catalogs

```powershell
& $wslc cluster versions --developer
& $wslc cluster distributions --developer
& $wslc cluster cnis --developer
```

Each catalog command also accepts `--aksedge-path`.

## Generated help

For authoritative options from a particular build:

```powershell
& $wslc cluster --help
& $wslc cluster create --help
& $wslc cluster delete --help
& $wslc cluster status --help
& $wslc cluster kubeconfig --help
& $wslc cluster fleet join --help
& $wslc cluster fleet leave --help
& $wslc cluster fleet status --help
& $wslc cluster diagnostics --help
& $wslc cluster versions --help
& $wslc cluster distributions --help
& $wslc cluster cnis --help
```

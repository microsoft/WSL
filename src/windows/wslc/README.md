### WSL Container CLI
This is the WSL Container CLI README.

## Kubernetes clusters

`wslc cluster` provisions an Azure-connected AKS Arc cluster in a dedicated WSL
distro by default:

```powershell
wslc cluster create `
  --subscription <subscription-id> `
  --resource-group <resource-group> `
  --tenant-id <tenant-id>

wslc cluster status --resource-group <resource-group>
wslc cluster kubeconfig --output "$HOME\.kube\aksarc"
wslc cluster delete --subscription <subscription-id> --resource-group <resource-group>
```

Cluster settings can instead be supplied in a `KEY=VALUE` file:

```powershell
wslc cluster create --config .\aksarc.env
```

Command-line values override values from the config file. Supported keys include
`SUBSCRIPTION`, `RESOURCE_GROUP`, `TENANT_ID`, `LOCATION`, `DISTRIBUTION`,
`ENABLE_GPU`, `AUTH_MODE`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, `DISTRO`,
`CMP_SUBSCRIPTION`, `CMP_RESOURCE_GROUP`, `CMP_NAME`, `AKSARC_WHEEL_PATH`, and
`AKSARC_BUILD_ID`. Fleet settings use `FLEET_NAME`, `FLEET_SUBSCRIPTION`,
`FLEET_RESOURCE_GROUP`, `FLEET_LOCATION`, `FLEET_MEMBER_NAME`, and
`CREATE_FLEET`.

A managed cluster can be joined to Azure Kubernetes Fleet Manager during
creation:

```powershell
wslc cluster create `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --tenant-id <tenant-id> `
  --fleet <fleet-name> `
  --fleet-resource-group <fleet-resource-group>
```

If the Fleet does not exist, an interactive invocation asks permission to
create a public Fleet hub. Use `--create-fleet` to provide advance consent.
The Fleet subscription and resource group default to those of the cluster.

Existing managed clusters support:

```powershell
wslc cluster fleet join `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group> `
  --fleet <fleet-name>

wslc cluster fleet status `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group>

wslc cluster fleet leave `
  --subscription <cluster-subscription> `
  --resource-group <cluster-resource-group>
```

`attach` and `detach` are aliases for `join` and `leave`. Use `--move` with
`fleet join` to move a cluster from another Fleet, and `--yes` with `fleet
leave` to skip its confirmation. Fleet integration is currently a prototype
for managed clusters and is not supported with `--developer`.

Add `--developer` to use the standalone developer flow instead. This provisions
a local single-node Kubernetes cluster through the Edge Core SDK's `aksedge`
CLI. The flow creates a dedicated WSL guest, installs the standalone BMAgent,
and does not use Azure Resource Manager, CAPE, or CMP.

Install `aksedge-windows-amd64.exe` and `aks-bmagent.deb` beside `wslc.exe`, or
set `AKSEDGE_BIN` and `AKSEDGE_AGENT_DEB` to their full file paths. A production
BMAgent package feed can instead be supplied with `--agent-repo`.

```powershell
wslc cluster create --developer `
  --name dev-cluster `
  --distribution K8s `
  --cni cilium `
  --k8s-version <version>

wslc cluster status --developer
wslc cluster kubeconfig --developer --name dev-cluster --merge
wslc cluster diagnostics --developer --name dev-cluster --output .\support
wslc cluster delete --developer --name dev-cluster
```

`create` and `kubeconfig` require a cluster name. `create` also supports K3s, custom pod CIDRs, explicit WSL distro and node
names, kubeconfig output/merge, and optional GPU enablement. `delete` always
runs node cleanup, destroys the dedicated guest, and asks `aksedge` to prune
the cluster from the host kubeconfig. It does not delete standalone kubeconfig
files unless the SDK owns them. Use `versions`, `distributions`, and `cnis` to
query the catalog accepted by the installed SDK.

The developer catalog is also available through `wslc cluster versions
--developer`, `wslc cluster distributions --developer`, and `wslc cluster cnis
--developer`.

The existing root-level `wslc create` command creates containers, so all
Kubernetes cluster lifecycle commands use the non-conflicting
`wslc cluster <operation>` form.

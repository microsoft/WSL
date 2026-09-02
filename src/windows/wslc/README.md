### WSL Container CLI
This is the WSL Container CLI README.

## Managed AKS Arc clusters

`wslc cluster` provisions an Azure-connected AKS Arc cluster in a dedicated WSL
distro:

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
`AKSARC_BUILD_ID`.

## Standalone developer clusters

`wslc developer-cluster` (alias `dev-cluster`) provisions a local single-node
Kubernetes cluster through the Edge Core SDK's `aksedge` CLI. The flow creates a
dedicated WSL guest, installs the standalone BMAgent, and does not use Azure
Resource Manager, CAPE, or CMP.

Install `aksedge-windows-amd64.exe` and `aks-bmagent.deb` beside `wslc.exe`, or
set `AKSEDGE_BIN` and `AKSEDGE_AGENT_DEB` to their full file paths. A production
BMAgent package feed can instead be supplied with `--agent-repo`.

```powershell
wslc developer-cluster create `
  --name dev-cluster `
  --distribution K8s `
  --cni cilium `
  --k8s-version <version>

wslc developer-cluster status
wslc developer-cluster kubeconfig --name dev-cluster --merge
wslc developer-cluster diagnostics --name dev-cluster --output .\support
wslc developer-cluster delete --name dev-cluster
```

`create` and `delete` require a cluster name. `create` also supports K3s, custom pod CIDRs, explicit WSL distro and node
names, kubeconfig output/merge, and optional GPU enablement. `delete` always
runs node cleanup, destroys the dedicated guest, and asks `aksedge` to prune
the cluster from the host kubeconfig. It does not delete standalone kubeconfig
files unless the SDK owns them. Use `versions`, `distributions`, and `cnis` to
query the catalog accepted by the installed SDK.

The existing root-level `wslc create` command creates containers, so cluster
lifecycle commands use the non-conflicting `wslc cluster <operation>` and
`wslc developer-cluster <operation>` forms.

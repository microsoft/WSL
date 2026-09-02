### WSL Container CLI
This is the WSL Container CLI README.

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
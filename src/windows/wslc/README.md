### WSL Container CLI

In addition to container lifecycle commands, `wslc` can provision AKS Arc in a
dedicated WSL distro:

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

The existing root-level `wslc create` command creates containers, so cluster
lifecycle commands use the non-conflicting `wslc cluster <operation>` form.
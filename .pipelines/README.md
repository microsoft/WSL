# WSL Azure DevOps pipelines

This folder contains the Azure DevOps (VSO) pipeline definitions used to build,
test, package and publish WSL.

## GitHub PR token via Azure Key Vault

Two pipelines open pull requests against `microsoft/WSL` through the GitHub REST
API and therefore need a GitHub token:

| Pipeline | Purpose |
|---|---|
| [`wsl-build-nightly-localization.yml`](wsl-build-nightly-localization.yml) | Opens a PR with the nightly localization touchdown changes. |
| [`wsl-build-notice.yml`](wsl-build-notice.yml) | Opens a PR with the regenerated `NOTICE.txt`. |

Both pipelines call `tools/devops/create-change.py` and pass it the token via the
`$(GithubPRToken)` pipeline variable.

Instead of storing the GitHub token directly as a pipeline secret, the token is
now read from an **Azure Key Vault** at run time using the built‑in
[`AzureKeyVault@2`](https://learn.microsoft.com/azure/devops/pipelines/tasks/reference/azure-key-vault-v2)
task:

```yaml
- task: AzureKeyVault@2
  displayName: Fetch GitHub PR token from Key Vault
  inputs:
    azureSubscription: $(KeyVaultServiceConnection)
    KeyVaultName: $(KeyVaultName)
    SecretsFilter: 'GithubPRToken'
```

The task downloads the secret and republishes it as a pipeline variable named
after the secret (i.e. `$(GithubPRToken)`), which the "Create pull request" step
then consumes.

### Pipeline variables (define these as *secret* variables)

The coordinates of the vault are intentionally **not** hard‑coded in the YAML so
the vault name/subscription are not exposed in this public repository. Define the
following as secret variables on each pipeline (Pipeline → **Edit** → **Variables**,
tick *Keep this value secret*), or in a shared variable group linked to both
pipelines:

| Variable | Example value | Description |
|---|---|---|
| `KeyVaultServiceConnection` | `Azure-Connection` | Name of the ARM service connection that can read the vault. |
| `KeyVaultName` | `wsl-pipelines-kv` | Name of the Azure Key Vault. |

> The secret in the vault must be named `GithubPRToken` (the hard-coded
> `SecretsFilter` value) because the `AzureKeyVault@2` task publishes each secret
> as a pipeline variable using its vault name, and the "Create pull request" step
> reads `$(GithubPRToken)`.

## Creating the Key Vault and connecting it to the pipelines

The steps below use the `az` CLI; the same can be done through the Azure Portal
and the Azure DevOps UI. Replace the placeholder names/ids to match your
environment.

### 1. Create the vault and store the token

```bash
# Sign in and pick the subscription that will host the vault.
az login
az account set --subscription "<SUBSCRIPTION_ID>"

# Create (or reuse) a resource group and the vault. RBAC authorization is
# recommended over legacy access policies.
az group create --name wsl-pipelines-rg --location westus2

az keyvault create \
  --name wsl-pipelines-kv \
  --resource-group wsl-pipelines-rg \
  --location westus2 \
  --enable-rbac-authorization true

# Store the GitHub token. Use a fine-grained / classic PAT (or a GitHub App
# installation token) that is allowed to push branches and open PRs against
# microsoft/WSL. The secret name must be GithubPRToken (the hard-coded
# SecretsFilter value used by the pipelines).
az keyvault secret set \
  --vault-name wsl-pipelines-kv \
  --name GithubPRToken \
  --value "<GITHUB_TOKEN>"
```

Alternatively, use the [`tools/devops/update-github-token.py`](../tools/devops/update-github-token.py)
helper to acquire a token and write it to the vault in one step. It can mint a
short-lived GitHub App installation token (recommended for rotation) or store a
token you already have:

```bash
pip install -r tools/devops/requirements.txt

# Mint an installation token from a GitHub App and store it:
python tools/devops/update-github-token.py \
  --vault-name wsl-ci-key-vault \
  --app-id "<GITHUB_APP_ID>" \
  --private-key-file app-private-key.pem

# ...or store an existing token (e.g. a PAT) directly:
python tools/devops/update-github-token.py \
  --vault-name wsl-ci-key-vault \
  --token "<GITHUB_TOKEN>"
```

It authenticates to the vault with `DefaultAzureCredential` (Azure CLI login,
managed identity, or an Azure DevOps service connection), so it can run as a
scheduled pipeline to rotate the token automatically.

### 2. Create an Azure service connection in Azure DevOps (VSO)

The pipelines authenticate to the vault through an **ARM service connection**.
Prefer a **Workload identity federation** connection (no stored secrets) — this
matches the existing `Azure-Connection` federated identity already used by the
localization pipeline's Touchdown task, which you can reuse.

To create a new one:

1. In Azure DevOps: **Project settings → Service connections → New service
   connection → Azure Resource Manager**.
2. Choose **Workload Identity federation (automatic)** (recommended) and select
   the subscription/resource group that contains the vault.
3. Name it (for example `Azure-Connection`) and save. This name is the value of
   the `KeyVaultServiceConnection` pipeline variable.

### 3. Grant the service connection read access to the vault

Find the service principal / managed identity (application id) that backs the
service connection (shown on the service connection details page), then grant it
permission to read secrets:

```bash
# RBAC-authorized vault (recommended):
az role assignment create \
  --assignee "<SERVICE_CONNECTION_APP_ID>" \
  --role "Key Vault Secrets User" \
  --scope "$(az keyvault show --name wsl-pipelines-kv --query id -o tsv)"

# If the vault uses legacy access policies instead of RBAC:
# az keyvault set-policy --name wsl-pipelines-kv \
#   --spn "<SERVICE_CONNECTION_APP_ID>" --secret-permissions get list
```

`Key Vault Secrets User` (get/list secrets) is sufficient — the pipelines only
read the token.

### 4. Authorize the pipelines to use the service connection

The first time a pipeline references the connection you may need to approve it:

- Open the service connection → **Security** and add the two pipelines (or allow
  access to all pipelines), **or**
- Run each pipeline once and use the **Permit** prompt on the run.

### 5. Define the secret pipeline variables

On each pipeline (or in a shared variable group), add the two secret variables
from the table above (`KeyVaultServiceConnection`, `KeyVaultName`). Once set,
remove the old plaintext `GithubPRToken` pipeline secret — it is now sourced from
the vault.

### 6. Validate

Queue each pipeline (or wait for the nightly schedule) and confirm the **Fetch
GitHub PR token from Key Vault** step succeeds and the **Create pull request**
step opens a PR.

## Creating Localization Tracking Bugs (GCS Azure DevOps)

Community-submitted localization PRs (edits to `localization/strings/<locale>/Resources.resw`, or
brand-new locale files) are reviewed/incorporated by the Microsoft Global Collaboration Service (GCS)
localization pipeline, **not** merged directly from GitHub. To route one for review, file a tracking
**Bug** in the GCS Azure DevOps project.

### Coordinates

| | |
|---|---|
| Org | `https://dev.azure.com/GlobalCollaborationService` |
| Project | `Global Collaboration Service Project` |
| Process | `GCS_Agile` |
| Work item type | `Bug` |
| Web "create from template" link | `https://dev.azure.com/GlobalCollaborationService/Global%20Collaboration%20Service%20Project/_workitems/create/Bug?templateId=b97df4d3-4106-4099-a3a9-1782b5891bec` |

### Required fields (and known-good values for a community loc PR)

The Bug type has several required custom picklist fields. Values that work for "community translation PR
needs review":

| Field (reference name) | Value |
|---|---|
| `System.Title` | `WSL: Review community <locale> localization PR (GitHub #<PR>)` |
| `System.Description` / `Microsoft.VSTS.TCM.ReproSteps` | PR link + scope + action (see below) |
| `Custom.IssueType` | `Incorrect Translation` |
| `Custom.ProductArea` | `Software` |
| `Custom.How_Found` | `Community` |
| `Custom.Severity_Impact` | `S3 - Medium` (field default) |
| `Microsoft.VSTS.Common.ValueArea` | `Business` (field default) |
| `Custom.HaveyouattachedEnglishandlocalizedscreenshots` | `No` (allowed: `Yes`/`No`; no default — must be set) |
| `Custom.Language` | picklist — see mapping below (the WSL locale code is **not** accepted) |

Other required custom flags (`Custom.BugBlocked`, `Custom.XLanguage`,
`Custom.OverrideMaxStringsThreshold`, `Custom.SkipSourceAudioFilesValidation`,
`Custom.DidCopilotrespondinthesamelanguage`, `Custom.Copilotattachallfiles`) default to `0` and
auto-fill; leave them unset.

### `Custom.Language` mapping (picklist values, not locale codes)

`Custom.Language` is a 148-value picklist keyed by display name. Map the WSL `Resources.resw` locale
folder to the picklist value:

| WSL locale | `Custom.Language` value |
|---|---|
| `es-ES` | `Spanish (Spain, International Sort)` |
| `zh-CN` | `Chinese (Simplified) - PRC` |
| `el-GR` | `Greek (Greece)` |
| `tr-TR` | `Turkish (Turkey)` |

For other locales, list the allowed values and match by display name:

```powershell
$org="https://dev.azure.com/GlobalCollaborationService"
$projEnc="Global%20Collaboration%20Service%20Project"
$res="499b84ac-1321-427f-aa17-267ca6975798"   # Azure DevOps AAD resource id
az rest --method get --resource $res `
  --uri "$org/$projEnc/_apis/wit/workitemtypes/Bug/fields/Custom.Language?`$expand=all" `
  --headers "Accept=application/json;api-version=7.1-preview.3" |
  ConvertFrom-Json | Select-Object -ExpandProperty allowedValues
```

### Suggested description body

```
Community-submitted localization PR on GitHub microsoft/WSL needs review by the loc pipeline.

PR: https://github.com/microsoft/WSL/pull/<PR>
Language: <locale>
Scope: <edits to existing | brand-new> localization/strings/<locale>/Resources.resw (+A/-B, 1 file).
Action: validate placeholder/locked-token parity vs en-US (tools/devops/validate-localization.py),
then incorporate or advise. New locales additionally need build registration, not just a file drop.
```

### Creating via CLI (`az boards`)

Requires the `azure-devops` az extension and an identity with **create/edit work item** permission in
the project (see caveat below).

```powershell
$org="https://dev.azure.com/GlobalCollaborationService"
$proj="Global Collaboration Service Project"
$area="Global Collaboration Service Project\Global\Windows"   # working area path, see Permissions gotcha
$desc="Community es-ES localization PR. PR: https://github.com/microsoft/WSL/pull/14109 ..."
az boards work-item create --org $org --project $proj --type "Bug" `
  --title "WSL: Review community es-ES localization PR (GitHub #14109)" `
  --area "$area" --description $desc `
  --fields "System.Description=$desc" `
           "Microsoft.VSTS.TCM.ReproSteps=$desc" `
           "Custom.Language=Spanish (Spain, International Sort)" `
           "Custom.IssueType=Incorrect Translation" `
           "Custom.ProductArea=Software" `
           "Custom.How_Found=Community" `
           "Custom.Severity_Impact=S3 - Medium" `
           "Microsoft.VSTS.Common.ValueArea=Business" `
           "Custom.HaveyouattachedEnglishandlocalizedscreenshots=No"
```

### Gotchas

- **`az.cmd` eats `&` in URLs.** When using `az rest`, never put `&` (multiple query params) in
  `--uri` on Windows — cmd truncates the URL there. Pass `api-version` via the **Accept header**
  instead: `--headers "Accept=application/json;api-version=7.1"`, keeping at most one `?param` in the URI.
- **Permissions are area-path-scoped, and the path matters.** Creating work items requires explicit rights on
  the *specific area path*, not the project as a whole. The correct, durable path for WSL localization bugs is
  `Global Collaboration Service Project\Global\Windows` (confirmed against existing manually-filed WSL loc
  bugs). Always set `System.AreaPath` (or `--area`) to that path. A `@ntdev.microsoft.com` identity is denied
  at the project root and most other paths (`TF237111: ... does not have permissions to save work items under
  the specified area path`). Do NOT file under `...\C and AI\Unspecified`: an identity may briefly appear to
  have access there, but bugs filed there get cleaned up and the access is not stable. To probe a path without
  creating anything, POST with `?validateOnly=true`. If your identity lacks rights everywhere, create the bug
  through the web template link above.
- **`System.Description` is required, separately from repro steps.** The Bug type rejects a create with
  `TF401320: Rule Error for field Description ... Required, InvalidEmpty` unless `System.Description` is set.
  Set both `System.Description` and `Microsoft.VSTS.TCM.ReproSteps` (the same body is fine). When POSTing a
  JSON-patch document via `az rest`, use newline-to-`<br>` so the HTML-rendered fields keep their line breaks.
- **Picklist discovery.** Field allowed-values often don't expand through the plain field endpoint; use
  `?$expand=all` on `.../workitemtypes/Bug/fields/<ref>` as shown above.
- **"Not found" can mean "no access," not "doesn't exist."** WIQL and `GET .../workitems/<id>` honor ACLs:
  a `CreatedBy=@me` query returning zero rows, or `TF401232: Work item <id> does not exist, or you do not
  have permissions to read it`, often just means your identity can't read that area path. Don't assume the
  bug was deleted. A gap in sequential work-item IDs is a tell that items were created but are now invisible
  to you (e.g. filed under a path whose access got revoked, then reaped) - which is exactly why you should
  stick to the durable `Global\Windows` path above.
- **Read the bug back after creating it.** Immediately `GET` each new work item id (or open the URL) to
  confirm it's actually persisted and readable under your identity. A create that "succeeds" against a flaky
  area path can vanish later; a read-back catches that while you can still re-file.
- **Non-ASCII gets mangled, and `az` errors poison `ConvertFrom-Json`.** Locale content (Turkish, Greek, CJK)
  comes back cp1252-garbled unless you set `[Console]::OutputEncoding=[System.Text.Encoding]::UTF8` first.
  Also, `az` prints errors to the stream, so piping straight into `ConvertFrom-Json` throws on any failure;
  capture with `Out-String` and parse, or check `$LASTEXITCODE`. Pass JSON-patch bodies via `--body "@file"`
  (content type `application/json-patch+json`) rather than inline to dodge quoting hell.

### After creating

Cross-link the systems: paste the ADO Bug ID into the GitHub PR (and vice-versa) so reviewers can find both.
Then read each new bug back (see gotcha above) to confirm it persisted under `Global\Windows`.

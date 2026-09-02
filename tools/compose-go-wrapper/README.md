# compose-go wrapper prototype

This directory demonstrates a process boundary between WSLC C++ components and
the Go `compose-go` library. It is an isolated proof of concept and is not wired
into the WSL build or approved for product use.

The helper reads one versioned JSON request from standard input, loads and
normalizes the supplied Compose files with `compose-go`, and writes one
versioned JSON response to standard output. A built Go executable contains the
Go runtime and does not require Go to be installed on the target machine.

## Process model

The process boundary is similar to `wslrelay.exe`: C++ launches a separately
packaged executable, connects explicit standard handles, supervises it with a
job object and cancellation policy, and validates its result. This helper is a
finite request-response process rather than a long-running relay.

Compose file content and environment values travel through standard input, not
the command line. Standard output contains only the JSON response. Standard
error is reserved for failures that prevent the helper from honoring the JSON
protocol, such as an internal panic or an output failure.

## Build and test

```powershell
go test ./...
go build -o compose-go-wrapper.exe .
```

## Request

```json
{
  "schemaVersion": 1,
  "projectName": "example",
  "workingDirectory": "C:\\src\\example",
  "files": [
    {
      "path": "C:\\src\\example\\compose.yaml",
      "content": "services:\n  web:\n    image: \"nginx:${TAG}\"\n"
    }
  ],
  "environment": {
    "TAG": "latest"
  },
  "profiles": []
}
```

The caller supplies file content and environment values explicitly. The helper
does not read its own process environment.

## Response

A successful response contains parser version information, the resolved input
context, and the normalized Compose model:

```json
{
  "schemaVersion": 1,
  "parser": {
    "name": "compose-go",
    "version": "v2.14.0"
  },
  "project": {
    "name": "example",
    "workingDirectory": "C:\\src\\example",
    "composeFiles": [
      "C:\\src\\example\\compose.yaml"
    ],
    "model": {
      "name": "example",
      "services": {
        "web": {
          "image": "nginx:latest"
        }
      }
    }
  }
}
```

Invalid requests and Compose errors are returned as structured JSON with a
nonzero process exit code.

## Prototype limitations

- `include`, external `extends.file`, `env_file`, and `label_file` inputs are
  rejected because schema version 1 does not carry their content.
- The normalized `model` currently follows compose-go's JSON shape. A product
  implementation needs a separately versioned WSLC-owned model.
- The prototype does not implement capability validation, localization,
  cancellation, process sandboxing, or the C++ process launcher.
- Product integration requires separate approval for the Go toolchain and
  embedded runtime distribution, compose-go, and every transitive dependency.
  It also requires reproducible-build, signing, servicing, and security review.

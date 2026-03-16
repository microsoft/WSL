---
description: 'WSL MkDocs documentation build and validation'
applyTo: 'doc/**'
---

# Documentation — WSL

## Published Docs

- **User docs**: https://learn.microsoft.com/windows/wsl/
- **Developer docs**: https://wsl.dev/
- **Source**: `doc/` directory, built with MkDocs

## Building Documentation (Linux or Windows)

```bash
pip install mkdocs-mermaid2-plugin mkdocs --break-system-packages
mkdocs build -f doc/mkdocs.yml
```

- Build time: ~0.5 seconds
- Output: `doc/site/`
- **Note**: May show warnings about mermaid CDN access on restricted networks

## Validation

- After changes, run `mkdocs build -f doc/mkdocs.yml` and verify no errors.
- Review generated HTML in `doc/site/`.

## Key Doc Pages

| Page | Source | Content |
|------|--------|---------|
| Getting started | `doc/docs/dev-loop.md` | Build, test, deploy instructions |
| Debugging | `doc/docs/debugging.md` | ETL tracing, debuggers, debug console |
| Architecture | `doc/docs/technical-documentation/index.md` | Component overview with Mermaid diagram |
| Boot process | `doc/docs/technical-documentation/boot-process.md` | WSL2 boot sequence diagram |
| Interop | `doc/docs/technical-documentation/interop.md` | Running Windows executables from Linux |
| Drvfs | `doc/docs/technical-documentation/drvfs.md` | Accessing Windows drives from Linux |
| Systemd | `doc/docs/technical-documentation/systemd.md` | Systemd integration |

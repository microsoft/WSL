# Wslc User Settings

## Overview

This document proposes a user settings feature for `wslc`, allowing users to persist default values for frequently-used options (session settings, registry, storage options, etc.) in a configuration file, eliminating the need to re-specify them on every invocation.

---

## 1. Settings File Format Comparison

Four formats are in common use across the Linux container ecosystem:

### 1.1 JSON

**Used by:** Docker daemon (`/etc/docker/daemon.json`), Docker user config (`~/.docker/config.json`)

**Pros:**
- Zero new library cost — `nlohmann/json` is already a dependency of `wslc`
- Universally understood; no learning curve for developers
- Strict, unambiguous syntax; no hidden parsing surprises
- Excellent tooling: schema validators, formatters, IDE support everywhere

**Cons:**
- No native comment support — users cannot annotate their settings or leave reminders
- Verbose for simple key-value data (braces, commas, quoted keys)
- A single missing comma or trailing comma breaks the entire file

**Sample:**
```json
{
  "session": {
    "cpuCount": 4,
    "memorySizeMb": 8192,
    "maxStorageSizeMb": 51200,
    "defaultStoragePath": "C:\\Users\\user\\wslc\\storage"
  }
}
```

### 1.2 TOML

**Used by:** Podman (`containers.conf`), containerd (`config.toml`), Docker BuildKit (`buildkitd.toml`), Cargo (`Cargo.toml`)

**Pros:**
- Comments supported (`#`) — users can document why they set a value
- Section-based structure (`[defaults]`, `[registry]`) maps naturally to command groups
- Not whitespace-sensitive — no tabs-vs-spaces class of bugs
- Unambiguous semantics (unlike YAML's implicit type coercion)
- Explicitly designed for human-edited configuration files
- The de-facto choice for new container daemons (Podman, containerd)

**Cons:**
- No existing TOML parser in the wslc dependency tree — requires adding one
- Slightly less familiar to developers who primarily know Docker/Kubernetes
- Verbosity increases with deeply nested tables

**Sample:**
```toml
[session]
# Number of virtual CPUs allocated to the session
cpu_count = 4

# Memory limit for the session in megabytes
memory_size_mb = 8192

# Maximum disk image size in megabytes
max_storage_size_mb = 51200

# Default path for container storage
default_storage_path = "C:\\Users\\user\\wslc\\storage"
```

### 1.3 YAML

**Used by:** Kubernetes manifests, Docker Compose (`compose.yaml`), Helm charts, GitHub Actions

**Pros:**
- Excellent human readability for complex nested structures
- Comments supported
- Dominant in the cloud-native DevOps ecosystem; most container developers know it

**Cons:**
- Whitespace-sensitive indentation is a frequent source of hard-to-spot errors
- Implicit type coercion causes subtle bugs (`yes`/`no`/`on`/`off` parsed as booleans, bare numbers parsed as integers, etc.)
- Multiple equivalent representations of the same data cause inconsistency
- Significantly over-engineered for a small settings file with flat-to-shallow structure
- No existing YAML parser in the wslc dependency tree

**Sample:**
```yaml
session:
  # Number of virtual CPUs allocated to the session
  cpuCount: 4

  # Memory limit for the session in megabytes
  memorySizeMb: 8192

  # Maximum disk image size in megabytes
  maxStorageSizeMb: 51200

  # Default path for container storage
  defaultStoragePath: "C:\\Users\\user\\wslc\\storage"
```

### 1.4 INI

**Used by:** WSL global settings (`%USERPROFILE%\.wslconfig`), `wsl.conf` (per-distribution), systemd unit files

**Pros:**
- Maximally simple — `key = value` under `[section]` headers
- Familiar to WSL users specifically (`.wslconfig` uses this format)
- No learning curve whatsoever
- Comments supported

**Cons:**
- No standard for nested structures, arrays, or typed values beyond strings
- No formal specification — multiple incompatible dialects exist
- Does not scale to the settings complexity `wslc` will need (e.g., per-registry options, volume mounts defaults, environment variable lists)
- Associated with legacy tooling; rarely chosen for new container-native CLIs

**Sample:**
```ini
[session]
; Number of virtual CPUs allocated to the session
cpu_count = 4

; Memory limit for the session in megabytes
memory_size_mb = 8192

; Maximum disk image size in megabytes
max_storage_size_mb = 51200

; Default path for container storage
default_storage_path = C:\Users\user\wslc\storage
```

> Note: INI has no standard for typed integers — parsers treat all values as strings, requiring the application to convert them. Path quoting rules also vary by parser.

### 1.5 Format Comparison Matrix

| Criterion | JSON | TOML | YAML | INI |
|---|---|---|---|---|
| Comments | No | Yes | Yes | Yes |
| Human editability | Medium | High | High | High |
| Whitespace sensitivity | No | No | **Yes** | No |
| Nested/typed data | Excellent | Good | Excellent | Poor |
| Implicit type coercion risks | No | No | **Yes** | No |
| Container ecosystem usage | Docker | Podman, containerd | Kubernetes, Compose | WSL only |
| Existing wslc dependency | **Yes** | No | No | No |
| Comments in user-facing config | N/A | Critical | Critical | Partial |

---

## 2. Recommended Format: YAML (JSON as fallback)

After some quick discussions with several team members, **YAML is the preferred format, with JSON as the backup option.** The rationale:

1. **YAML dominates the container ecosystem that `wslc` users already live in.** Kubernetes manifests, Docker Compose files, Helm charts, and most CI/CD pipelines (GitHub Actions, GitLab CI) are all YAML. Users of WSL containers are overwhelmingly familiar with YAML and will feel at home editing a YAML settings file without consulting documentation.

2. **Comment support is non-negotiable for a user-facing settings file.** Both YAML and JSON-with-comments variants support `#` comments; plain JSON does not. This alone disqualifies JSON as the primary choice — users must be able to annotate their settings and the file must be able to ship with inline guidance (e.g., `# Options: always | missing | never`).

3. **YAML's readability advantage is meaningful at this scale.** The `wslc` settings file is shallow (one or two levels of nesting at most) and small. At this scale, YAML's indentation structure produces the most human-readable result and imposes the least syntactic noise. The whitespace-sensitivity concern — YAML's most common criticism — is largely mitigated when the file is short, flat, and ships with a well-formatted template that users edit in place.

4. **INI does not support typed arrays or nested structures** that `wslc` settings will need (e.g., default environment variable lists, per-registry options).

5. **TOML is not recommended** despite its container daemon usage (Podman, containerd) because it is less familiar to the broader developer audience and offers no significant advantage over YAML for a settings file of this complexity.

**Backup option — JSON:** If adding a YAML parser dependency is blocked, or if the team prioritizes zero new dependencies, JSON is acceptable. The trade-offs are: no comment support (the file cannot include inline guidance), and a more verbose syntax. The existing `nlohmann/json` library covers JSON at no added cost. To partially mitigate the lack of comments, the `wslc settings --list` command would become more important as the primary way to discover available keys and their current values.

### File Extension

- **Primary (YAML):** `UserSettings.yaml`
- **Fallback (JSON):** `UserSettings.json`

---

## 3. File Location

```
%LOCALAPPDATA%\Microsoft\wslc\UserSettings.yaml
```

**Rationale:**
- `%LOCALAPPDATA%\Microsoft\WSL\` is the natural home for WSL user data on Windows (consistent with WSL's existing conventions)
- Per-user, not system-wide — settings affect only the invoking user
- The `wslc\` subdirectory scopes it cleanly away from WSL service data
- `UserSettings.yaml` is widely recognized; `.yaml` is the preferred extension over `.yml` per the YAML FAQ

A backup copy is maintained alongside the primary file:
```
%LOCALAPPDATA%\Microsoft\wslc\UserSettings.yaml.bak
```

---

## 4. `wslc settings` Command Design

### 4.1 Subcommands

```
wslc settings              Open settings file in default editor (or Notepad)
wslc settings --reset      Restore settings to built-in defaults (prompts for confirmation)
```

The primary usage (`wslc settings` with no flags) opens the file for editing. This is the most common workflow: a user wants to change something, they open it, edit, save, and the next invocation picks up the change.

`--reset` is a safe escape hatch when a user has broken their settings and wants to start fresh.

### 4.2 Editor Behavior

The file is opened via `ShellExecuteW` with the file path, which respects the user's `.yaml` file association. If no application is associated, Notepad is used as the fallback.

The command does not wait for the editor to close — it opens the file and exits immediately.

### 4.3 First Run / File Creation

On first invocation of any `wslc` command, if the settings file does not exist, it is created at the standard location with all keys commented out and their defaults shown inline:

```yaml
# wslc user settings
# https://aka.ms/wslc-settings

session:
  # Number of virtual CPUs allocated to the session
  # cpuCount: 4

  # Memory limit for the session in megabytes
  # memorySizeMb: 8192

  # Maximum disk image size in megabytes
  # maxStorageSizeMb: 51200

  # Default path for container storage
  # defaultStoragePath: "C:\\Users\\user\\wslc\\storage"
```

Commenting out all entries rather than writing the defaults explicitly prevents `wslc` from mistaking user-set values from file defaults when applying command-line override precedence.

### 4.4 Backup and Recovery

A user might make a mistake that could make the settings file unparsable. To protect against this there will be a backup settings file with the latest known good settings file.
Before opening the editor (i.e. `wslc settings`, or any future settings write event), `wslc` copies the current valid settings to `UserSettings.yaml.bak`. On startup, settings are loaded with the following precedence:

1. `UserSettings.yaml` (if it parses without syntax errors and passes schema validation)
2. `UserSettings.yaml.bak` (if the primary fails; a warning is printed to stderr)
3. Built-in defaults (if both fail; a warning is printed to stderr)

```
Warning: UserSettings.yaml could not be parsed. Using backup settings.
Warning: UserSettings.yaml.bak could not be parsed. Using built-in defaults.
```

No warning is printed if neither file exists (first run).

---

## 5. Argument Override Precedence

Settings from the file represent the user's persistent defaults. They are overridden by explicit command-line arguments, which are overridden by nothing. The precedence chain from lowest to highest:

```
Built-in defaults  <  UserSettings.yaml  <  command-line flags
```

Example: if `UserSettings.yaml` contains `memorySizeMb: 8192` and the user runs `wslc session --memory 4096`, the effective memory size is `4096`. If `--memory` is not given, it is `8192`. If the key is not in settings, it falls back to the built-in default.

This means commands must check whether an argument was explicitly provided by the user versus not present at all (not yet set in `ArgMap`), rather than checking for a default value. The existing `ArgMap::Contains()` method already provides this distinction cleanly.

---

## 6. Validation

### Syntax Validation

YAML syntax errors (malformed structure, invalid indentation, unclosed strings, invalid escape sequences) are caught on parse and trigger the backup/fallback recovery flow described in §4.4.

### Setting Value Validation

After parsing, each known key is validated against its expected type and allowed values:

- **Type errors** (e.g., `cpuCount: "four"` when an integer is expected) → treat as a schema error, use backup/defaults, and print a warning identifying the offending key
- **Invalid enum values** (e.g., `memorySizeMb: -1` when a positive integer is expected) → same: warn and fall back
- **Unknown keys** → warn but do not fail; allows forward compatibility when a newer settings file is used with an older `wslc` binary:
  ```
  Warning: Unknown settings key 'defaults.experimental'. Ignoring.
  ```
- **Out-of-range numeric values** → warn and fall back to the default for that key only (not the entire file)

---

## 7. Implementation Notes

### Settings Loading

Settings are loaded once at startup in `CoreMain`, before command dispatch, and stored in `CLIExecutionContext`. Settings will be implemented as a singleton and globally read accessible.

### Settings File Write (for `--reset`)

`--reset` overwrites `UserSettings.yaml` with the commented-out defaults template (the same content generated on first run) after prompting:

```
This will reset all settings to defaults. Continue? [y/N]
```

### No Live Reload

Settings are read once per invocation. There is no file-watcher or live-reload mechanism. This is intentional: CLI tools should be predictable — the settings active when a command starts are the settings used for that command's lifetime.

### Libyaml

If in the end yaml is the chosen format, [Libyaml](https://github.com/yaml/libyaml) will be added as a dependency of this project. WinGet already has a YamlWrapper implementation on top of that.

---

## 8. Out of Scope for Initial Implementation

- **`wslc settings set <key> <value>`** — programmatic key setting without opening an editor. Useful for scripting. Deferred to v2; manual editing covers the primary use case.
- **System-wide settings** — a machine-level settings file (e.g., `%ProgramData%\Microsoft\WSL\wslc\UserSettings.yaml`) that applies to all users. Deferred; per-user is sufficient for v1.
- **Settings profiles** — named profiles for different environments. Deferred.
- **Schema export** — `wslc settings --schema` to print a JSON Schema or TOML template. Useful for editor autocompletion; deferred.
- **Environment variable overrides** — `WSLC_DEFAULTS_FORMAT=json` style overrides. Useful for CI pipelines; deferred.

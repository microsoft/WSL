# wslc User Settings — Design Document

## Overview

This document proposes a user settings feature for `wslc`, allowing users to persist default values for frequently-used options (format, pull policy, default session, registry scheme, etc.) in a configuration file, eliminating the need to re-specify them on every invocation.

---

## 1. Settings File Format Comparison

Four formats are in common use across the Linux container ecosystem:

### 1.1 JSON

**Used by:** Docker daemon (`/etc/docker/daemon.json`), Docker user config (`~/.docker/config.json`), winget (`settings.json`)

**Pros:**
- Zero new library cost — `nlohmann/json` is already a dependency of `wslc`
- Universally understood; no learning curve for developers
- Strict, unambiguous syntax; no hidden parsing surprises
- Excellent tooling: schema validators, formatters, IDE support everywhere

**Cons:**
- No native comment support — users cannot annotate their settings or leave reminders
- Verbose for simple key-value data (braces, commas, quoted keys)
- A single missing comma or trailing comma breaks the entire file

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

## 2. Recommended Format: TOML

**TOML is the recommended format.** The rationale:

1. **Comment support is non-negotiable for a user-facing settings file.** Without comments, users cannot annotate their choices, and the file cannot include inline guidance (e.g., `# Available values: always | missing | never`). JSON's lack of comments is its primary disqualifier for this use case.

2. **TOML is the established choice for Linux container daemon configuration.** Both Podman (`containers.conf`) and containerd (`config.toml`) use it. Users who configure those tools will recognize the format immediately.

3. **TOML's explicit type system and no-whitespace-sensitivity make it safer than YAML** for hand-editing. YAML's implicit coercions and indentation sensitivity are well-documented sources of user frustration in config files.

4. **INI does not support typed arrays or nested structures** that `wslc` settings will need (e.g., default environment variable lists, per-registry options).

5. **The JSON library cost argument is weak.** The cost of integrating a small TOML parser (e.g., `toml++`, header-only, MIT license) is low and pays off in user experience. Alternatively, JSON with a parallel machine-generated comment scaffold (as some tools implement) is an acceptable fallback but is more complex to implement correctly.

> **Fallback option:** If adding a new parser dependency is blocked, JSON remains acceptable with the trade-off that the file ships with a commented-out schema reference or a `wslc settings --schema` command that prints all valid keys and their types.

---

## 3. File Location

```
%LOCALAPPDATA%\Microsoft\WSL\wslc\settings.toml
```

**Rationale:**
- `%LOCALAPPDATA%\Microsoft\WSL\` is the natural home for WSL user data on Windows (consistent with WSL's existing conventions)
- Per-user, not system-wide — settings affect only the invoking user
- The `wslc\` subdirectory scopes it cleanly away from WSL service data
- `settings.toml` is the conventional name for TOML-format settings files (cf. `buildkitd.toml`, `config.toml`)

A backup copy is maintained alongside the primary file:
```
%LOCALAPPDATA%\Microsoft\WSL\wslc\settings.toml.bak
```

---

## 4. `wslc settings` Command Design

### 4.1 Subcommands

```
wslc settings              Open settings file in default editor (or Notepad)
wslc settings --edit       Same as above (explicit flag)
wslc settings --get <key>  Print the current value of a specific key
wslc settings --list       Print all current settings and their effective values
wslc settings --reset      Restore settings to built-in defaults (prompts for confirmation)
```

The primary usage (`wslc settings` with no flags) opens the file for editing, modeled after `winget settings`. This is the most common workflow: a user wants to change something, they open it, edit, save, and the next invocation picks up the change.

`--get` and `--list` are useful for scripting and for verifying the effective configuration without opening an editor.

`--reset` is a safe escape hatch when a user has broken their settings and wants to start fresh.

### 4.2 Editor Behavior

The file is opened via `ShellExecuteW` with the file path, which respects the user's `.toml` file association. If no application is associated, Notepad is used as the fallback. This matches winget's behavior and requires no custom editor logic.

The command does not wait for the editor to close — it opens the file and exits immediately, just like `winget settings`.

### 4.3 First Run / File Creation

On first invocation of any `wslc` command, if the settings file does not exist, it is created at the standard location with all keys commented out and their defaults shown inline:

```toml
# wslc user settings
# https://aka.ms/wslc-settings

[defaults]
# Output format for list commands. Options: table | json
# format = "table"

# Session to connect to if --session is not specified on the command line.
# session = ""

# Image pull policy. Options: always | missing | never
# pull = "never"

# Progress display type. Options: ansi | none
# progress = "ansi"

[registry]
# Default scheme for registry connections. Options: https | http
# scheme = "https"
```

Commenting out all entries rather than writing the defaults explicitly prevents `wslc` from mistaking user-set values from file defaults when applying command-line override precedence.

### 4.4 Backup and Recovery

Before opening the editor (or on any settings write operation), `wslc` copies the current valid settings to `settings.toml.bak`. On startup, settings are loaded with the following precedence:

1. `settings.toml` (if it parses without syntax errors and passes schema validation)
2. `settings.toml.bak` (if the primary fails; a warning is printed to stderr)
3. Built-in defaults (if both fail; a warning is printed to stderr)

```
Warning: settings.toml could not be parsed. Using backup settings.
Warning: settings.toml.bak could not be parsed. Using built-in defaults.
```

No warning is printed if neither file exists (first run).

---

## 5. Settings Schema

Settings are organized into sections that correspond to wslc's command groups.

### `[defaults]` — Global command defaults

| Key | Type | Default | Description |
|---|---|---|---|
| `format` | string | `"table"` | Output format for list/inspect commands (`table` \| `json`) |
| `session` | string | `""` | Session name to connect to when `--session` is not passed |
| `pull` | string | `"never"` | Image pull policy (`always` \| `missing` \| `never`) |
| `progress` | string | `"ansi"` | Progress output type (`ansi` \| `none`) |
| `quiet` | bool | `false` | Suppress non-essential output by default |

### `[container]` — Container command defaults

| Key | Type | Default | Description |
|---|---|---|---|
| `user` | string | `""` | Default user (`uid`, `uid:gid`, or username) for `run`/`exec` |
| `entrypoint` | string | `""` | Default entrypoint override for new containers |
| `remove` | bool | `false` | Auto-remove containers after they stop (equivalent to `--rm`) |
| `dns` | string | `""` | Default DNS server IP for new containers |
| `no_dns` | bool | `false` | Disable DNS configuration in new containers by default |

### `[registry]` — Registry connection defaults

| Key | Type | Default | Description |
|---|---|---|---|
| `scheme` | string | `"https"` | Default registry connection scheme (`https` \| `http`) |

### Future sections (reserved, not in initial implementation)

- `[volume]` — Default volume mount paths
- `[network]` — Default port publish rules
- `[env]` — Default environment variables for all containers

---

## 6. Argument Override Precedence

Settings from the file represent the user's persistent defaults. They are overridden by explicit command-line arguments, which are overridden by nothing. The precedence chain from lowest to highest:

```
Built-in defaults  <  settings.toml  <  command-line flags
```

Example: if `settings.toml` contains `pull = "missing"` and the user runs `wslc run --pull always myimage`, the effective pull policy is `always`. If `--pull` is not given, it is `missing`. If the key is not in settings, it falls back to the built-in default (`never`).

This means commands must check whether an argument was explicitly provided by the user versus not present at all (not yet set in `ArgMap`), rather than checking for a default value. The existing `ArgMap::Contains()` method already provides this distinction cleanly.

---

## 7. Validation

### Syntax Validation

TOML syntax errors (malformed keys, unclosed strings, invalid escape sequences) are caught on parse and trigger the backup/fallback recovery flow described in §4.4.

### Schema Validation

After parsing, each known key is validated against its expected type and allowed values:

- **Type errors** (e.g., `format = 42` when a string is expected) → treat as a schema error, use backup/defaults, and print a warning identifying the offending key
- **Invalid enum values** (e.g., `pull = "sometimes"`) → same: warn and fall back
- **Unknown keys** → warn but do not fail; allows forward compatibility when a newer settings file is used with an older `wslc` binary:
  ```
  Warning: Unknown settings key 'defaults.experimental'. Ignoring.
  ```
- **Out-of-range numeric values** → warn and fall back to the default for that key only (not the entire file)

### Validation Command

`wslc settings --list` shows each key, its source (file | backup | default), and its effective value, making it easy to diagnose misconfiguration without running a container operation.

---

## 8. Implementation Notes

### Settings Loading

Settings are loaded once at startup in `CoreMain`, before command dispatch, and stored in `CLIExecutionContext`. Commands read from `context.Settings` (a new typed struct) when an argument is not present in `context.Args`.

### Settings File Write (for `--reset`)

`--reset` overwrites `settings.toml` with the commented-out defaults template (the same content generated on first run) after prompting:

```
This will reset all settings to defaults. Your current settings.toml will be saved as settings.toml.bak. Continue? [y/N]
```

### No Live Reload

Settings are read once per invocation. There is no file-watcher or live-reload mechanism. This is intentional: CLI tools should be predictable — the settings active when a command starts are the settings used for that command's lifetime.

---

## 9. Out of Scope for Initial Implementation

- **`wslc settings set <key> <value>`** — programmatic key setting without opening an editor. Useful for scripting. Deferred to v2; manual editing covers the primary use case.
- **System-wide settings** — a machine-level settings file (e.g., `%ProgramData%\Microsoft\WSL\wslc\settings.toml`) that applies to all users. Deferred; per-user is sufficient for v1.
- **Settings profiles** — named profiles for different environments. Deferred.
- **Schema export** — `wslc settings --schema` to print a JSON Schema or TOML template. Useful for editor autocompletion; deferred.
- **Environment variable overrides** — `WSLC_DEFAULTS_FORMAT=json` style overrides. Useful for CI pipelines; deferred.

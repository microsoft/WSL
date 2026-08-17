### WSL Container CLI
This is the WSL Container CLI README

#### Basic compose support

The initial compose implementation accepts a YAML file containing only service names and images:

```yaml
services:
  web:
    name: sample-web
    image: nginx:stable-alpine
```

Use `wslc compose create <path>`, `wslc compose start <path>`, `wslc compose attach <path>`, and
`wslc compose stop <path>`. Attach currently supports single-container compose sessions.
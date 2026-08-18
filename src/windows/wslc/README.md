### WSL Container CLI
This is the WSL Container CLI README

#### Basic compose support

The initial compose implementation accepts a YAML file containing only service names and images:

```yaml
services:
  web:
    name: sample-web
    image: nginx:stable-alpine
    environment:
      MODE: development
    working_dir: /usr/share/nginx/html
    command: ["/docker-entrypoint.sh", "nginx", "-g", "daemon off;"]
    volumes:
      - ./html:/usr/share/nginx/html:ro
    ports:
      - "8080:80"
```

Use `wslc compose create <path>`, `wslc compose up <path>`, `wslc compose start <path>`,
`wslc compose attach <path>`, and `wslc compose stop <path>`. Up starts the session and attaches to it. Attach
displays all container output on the current console and routes interactive input to the first container. Ports
support only `host:container` TCP mappings and bind to IPv4 loopback.
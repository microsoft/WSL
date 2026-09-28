#!/bin/sh
set -e

if [ -n "$USERNAME" ]; then
    mkdir -p /auth
    htpasswd -Bbn "$USERNAME" "$PASSWORD" > /auth/htpasswd

    export REGISTRY_AUTH=htpasswd
    export REGISTRY_AUTH_HTPASSWD_PATH=/auth/htpasswd
    export REGISTRY_AUTH_HTPASSWD_REALM="WSLC Registry"
fi

exec registry serve /etc/distribution/config.yml

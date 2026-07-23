# Copyright (C) Microsoft Corporation. All rights reserved.

FROM debian:bookworm-slim

ARG GIT_FIXTURE_URL=https://github.com/microsoft/TypeScript.git
ARG GIT_FIXTURE_REF=v5.8.3

RUN apt-get update \
    && apt-get install --no-install-recommends --yes ca-certificates git util-linux \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /fixtures/git \
    && git clone --depth 1 --branch "${GIT_FIXTURE_REF}" "${GIT_FIXTURE_URL}" /tmp/git-fixture \
    && git clone --bare /tmp/git-fixture /fixtures/git/repository.git \
    && git -C /tmp/git-fixture archive --format=tar --output=/fixtures/typescript.tar HEAD \
    && rm -rf /tmp/git-fixture

COPY run-workload.sh /usr/local/bin/run-workload.sh
RUN chmod +x /usr/local/bin/run-workload.sh

ENTRYPOINT ["/usr/local/bin/run-workload.sh"]
# Copyright (C) Microsoft Corporation. All rights reserved.

FROM node:22-bookworm-slim

ARG GIT_FIXTURE_URL=https://github.com/microsoft/TypeScript.git
ARG GIT_FIXTURE_REF=v5.8.3
ARG NPM_FIXTURE_URL=https://github.com/expressjs/express.git
ARG NPM_FIXTURE_REF=5.1.0

RUN apt-get update \
    && apt-get install --no-install-recommends --yes ca-certificates git util-linux \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /fixtures/git \
    && git clone --depth 1 --branch "${GIT_FIXTURE_REF}" "${GIT_FIXTURE_URL}" /tmp/git-fixture \
    && git clone --bare /tmp/git-fixture /fixtures/git/repository.git \
    && rm -rf /tmp/git-fixture

RUN git clone --depth 1 --branch "${NPM_FIXTURE_REF}" "${NPM_FIXTURE_URL}" /fixtures/npm-project \
    && rm -rf /fixtures/npm-project/.git \
    && cd /fixtures/npm-project \
    && npm ci \
    && npm cache verify \
    && rm -rf node_modules

COPY run-workload.sh /usr/local/bin/run-workload.sh
RUN chmod +x /usr/local/bin/run-workload.sh

ENTRYPOINT ["/usr/local/bin/run-workload.sh"]
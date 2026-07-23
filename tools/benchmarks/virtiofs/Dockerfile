# Copyright (C) Microsoft Corporation. All rights reserved.

FROM debian:bookworm-slim

ARG GIT_FIXTURE_URL=https://github.com/microsoft/TypeScript.git
ARG GIT_FIXTURE_REF=v5.8.3

RUN apt-get update \
    && apt-get install --no-install-recommends --yes ca-certificates git util-linux \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /fixtures/git /tmp/source-fixture \
    && git clone --depth 1 --branch "${GIT_FIXTURE_REF}" "${GIT_FIXTURE_URL}" /tmp/upstream \
    && git -C /tmp/upstream archive --format=tar --output=/fixtures/typescript-source.tar HEAD src tests/cases/project tests/cases/projects \
    && tar --extract --file=/fixtures/typescript-source.tar --directory=/tmp/source-fixture \
    && git -C /tmp/source-fixture init \
    && git -C /tmp/source-fixture config user.email benchmark@example.invalid \
    && git -C /tmp/source-fixture config user.name "Virtiofs Benchmark" \
    && git -C /tmp/source-fixture add . \
    && git -C /tmp/source-fixture commit --quiet --message fixture \
    && git clone --bare /tmp/source-fixture /fixtures/git/repository.git \
    && rm -rf /tmp/upstream /tmp/source-fixture

COPY run-workload.sh /usr/local/bin/run-workload.sh
RUN sed --in-place 's/\r$//' /usr/local/bin/run-workload.sh \
    && chmod +x /usr/local/bin/run-workload.sh

ENTRYPOINT ["/usr/local/bin/run-workload.sh"]
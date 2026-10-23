# Official Debian base pinned for this validation recipe. Package repository
# state is still live; record installed versions with validation evidence.
FROM debian:bookworm-20261005-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587

RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 libasound2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
ENV PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["timeout", "--signal=TERM", "--kill-after=2s", "615s", "python3", "/app/alsa_capture_probe.py"]

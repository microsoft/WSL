# Public ALSA capture probe

This standalone Python probe captures an explicitly selected ALSA input as 48 kHz, stereo, interleaved `S16_LE`. It prints one JSON object containing aggregate frame and sample counts, RMS, peak, and nonzero-sample count. It does not save, log, or transmit PCM data and has no playback path.

Capture requires both `--device` and `--acknowledge-microphone-access`. Duration is an integer from 1 to 600 seconds. Reads use nonblocking ALSA mode, bounded waits, and a monotonic deadline of duration plus 10 seconds (capped at 610 seconds). `-EAGAIN` retries after another wait. `-EPIPE` is reported as an XRUN and capture stops without recovery.

Run the mock-only checks from this directory with:

```sh
python3 -m unittest discover -s tests -t .
```

An authorized local capture has this form:

```sh
python3 alsa_capture_probe.py --device hw:0,0 --duration 1 --acknowledge-microphone-access
```

The Compose recipe maps `/dev/snd` without renumbering cards, mounts the probe script read-only, uses a configurable unprivileged UID/GID, drops all Linux capabilities, disables networking, and makes the container root filesystem read-only. Set `ALSA_DEVICE` explicitly to the selected capture device (for example, `hw:0,0`) and set `AUDIO_GID` to its group ID; `PROBE_UID`, `PROBE_GID`, and `DURATION_SECONDS` can also be set. Then run `docker compose -f compose.yaml run --rm capture-probe`.

The Python deadline is cooperative: it cannot interrupt a native-library call that fails to return. The container entrypoint additionally uses GNU `timeout`, sending TERM after 615 seconds and KILL two seconds later if necessary. Neither limit guarantees device teardown if the kernel is unresponsive. The requested PCM duration is distinct from total wall time including setup and teardown. RMS uses signed 16-bit sample units; nonzero samples do not identify audio content or establish fidelity.

The Dockerfile pins its Debian base image by digest but uses the live Bookworm package repository; package versions are not reproducibly pinned. Offline mock tests check the probe logic. Physical capture and container acceptance must be evaluated separately for the exact script revision and runtime under test. Results from other recorders do not validate this script.

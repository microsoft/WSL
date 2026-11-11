#!/usr/bin/env python3
"""Bounded, summary-only ALSA capture probe for explicitly authorized tests."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import errno
import json
import math
import sys
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass
from typing import Protocol


RATE_HZ = 48_000
CHANNELS = 2
MAX_DURATION_SECONDS = 600
DEADLINE_PADDING_SECONDS = 10
MAX_DEADLINE_SECONDS = 610
WAIT_SLICE_MS = 250
READ_CHUNK_FRAMES = 1_024
ALSA_PCM_STREAM_CAPTURE = 1
ALSA_PCM_NONBLOCK = 1
ALSA_PCM_FORMAT_S16_LE = 2
ALSA_PCM_ACCESS_RW_INTERLEAVED = 3
ALSA_LATENCY_US = 500_000


class CaptureError(RuntimeError):
    """A capture setup or stream error."""


class CaptureTimeout(CaptureError):
    """The capture did not collect its requested frames before the deadline."""


class XRunError(CaptureError):
    """The PCM stream reported an XRUN; this probe does not recover it."""


class AlsaBackend(Protocol):
    def open(self, device: str) -> object: ...

    def set_params(self, handle: object) -> int: ...

    def get_params(self, handle: object) -> tuple[int, int] | None: ...

    def start(self, handle: object) -> int: ...

    def wait(self, handle: object, timeout_ms: int) -> int: ...

    def readi(self, handle: object, buffer: object, frames: int) -> int: ...

    def close(self, handle: object) -> int: ...

    def error_text(self, code: int) -> str: ...


@dataclass(frozen=True)
class CaptureStats:
    frames: int
    samples: int
    rms: float
    peak: int
    nonzero_samples: int


@dataclass
class CaptureDiagnostics:
    frames_read: int = 0
    elapsed_seconds: float = 0.0
    max_loop_gap_ms: float = 0.0
    buffer_frames: int | None = None
    period_frames: int | None = None
    stream_started: bool = False


class NativeAlsaBackend:
    """Small ctypes wrapper around the public libasound PCM interface."""

    def __init__(self) -> None:
        library_name = ctypes.util.find_library("asound")
        if not library_name:
            raise CaptureError("could not locate libasound")
        try:
            self._lib = ctypes.CDLL(library_name)
        except OSError as exc:
            raise CaptureError(f"could not load {library_name}: {exc}") from exc
        self._configure_signatures()

    def _configure_signatures(self) -> None:
        lib = self._lib
        lib.snd_pcm_open.argtypes = [
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.snd_pcm_open.restype = ctypes.c_int
        lib.snd_pcm_set_params.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_uint,
        ]
        lib.snd_pcm_set_params.restype = ctypes.c_int
        lib.snd_pcm_get_params.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong),
        ]
        lib.snd_pcm_get_params.restype = ctypes.c_int
        lib.snd_pcm_start.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_start.restype = ctypes.c_int
        lib.snd_pcm_wait.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.snd_pcm_wait.restype = ctypes.c_int
        lib.snd_pcm_readi.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
        lib.snd_pcm_readi.restype = ctypes.c_long
        lib.snd_pcm_close.argtypes = [ctypes.c_void_p]
        lib.snd_pcm_close.restype = ctypes.c_int
        lib.snd_strerror.argtypes = [ctypes.c_int]
        lib.snd_strerror.restype = ctypes.c_char_p

    def error_text(self, code: int) -> str:
        raw = self._lib.snd_strerror(code)
        return raw.decode("utf-8", errors="replace") if raw else f"ALSA error {code}"

    def open(self, device: str) -> object:
        if not device or "\x00" in device:
            raise CaptureError("device must be a non-empty ALSA device name")
        handle = ctypes.c_void_p()
        result = self._lib.snd_pcm_open(
            ctypes.byref(handle),
            device.encode("utf-8"),
            ALSA_PCM_STREAM_CAPTURE,
            ALSA_PCM_NONBLOCK,
        )
        if result < 0:
            raise CaptureError(f"snd_pcm_open failed ({result}): {self.error_text(result)}")
        return handle

    def set_params(self, handle: object) -> int:
        return int(
            self._lib.snd_pcm_set_params(
                handle,
                ALSA_PCM_FORMAT_S16_LE,
                ALSA_PCM_ACCESS_RW_INTERLEAVED,
                CHANNELS,
                RATE_HZ,
                1,
                ALSA_LATENCY_US,
            )
        )

    def get_params(self, handle: object) -> tuple[int, int] | None:
        buffer_frames = ctypes.c_ulong()
        period_frames = ctypes.c_ulong()
        result = self._lib.snd_pcm_get_params(
            handle,
            ctypes.byref(buffer_frames),
            ctypes.byref(period_frames),
        )
        if result < 0:
            return None
        return int(buffer_frames.value), int(period_frames.value)

    def start(self, handle: object) -> int:
        return int(self._lib.snd_pcm_start(handle))

    def wait(self, handle: object, timeout_ms: int) -> int:
        return int(self._lib.snd_pcm_wait(handle, timeout_ms))

    def readi(self, handle: object, buffer: object, frames: int) -> int:
        return int(
            self._lib.snd_pcm_readi(
                handle,
                ctypes.cast(buffer, ctypes.c_void_p),
                frames,
            )
        )

    def close(self, handle: object) -> int:
        return int(self._lib.snd_pcm_close(handle))


def _raise_alsa_error(backend: AlsaBackend, operation: str, code: int) -> None:
    text = backend.error_text(code)
    if code == -errno.EPIPE:
        raise XRunError(f"ALSA XRUN during {operation} (-EPIPE): {text}; no recovery attempted")
    raise CaptureError(f"{operation} failed ({code}): {text}")


def _check_deadline(monotonic: Callable[[], float], deadline: float) -> None:
    if monotonic() >= deadline:
        raise CaptureTimeout("capture exceeded its monotonic deadline")


def capture(
    backend: AlsaBackend,
    device: str,
    duration_seconds: int,
    *,
    monotonic: Callable[[], float] = time.monotonic,
    chunk_frames: int = READ_CHUNK_FRAMES,
    diagnostics: CaptureDiagnostics | None = None,
) -> CaptureStats:
    """Capture exact-duration S16_LE stereo frames and retain aggregates only."""
    if isinstance(duration_seconds, bool) or not isinstance(duration_seconds, int):
        raise ValueError("duration must be an integer number of seconds")
    if not 1 <= duration_seconds <= MAX_DURATION_SECONDS:
        raise ValueError(f"duration must be between 1 and {MAX_DURATION_SECONDS} seconds")
    if chunk_frames < 1:
        raise ValueError("chunk_frames must be positive")
    if not device or "\x00" in device:
        raise ValueError("device must be a non-empty ALSA device name")

    started = monotonic()
    diagnostic_state = diagnostics if diagnostics is not None else CaptureDiagnostics()
    deadline = started + min(
        duration_seconds + DEADLINE_PADDING_SECONDS,
        MAX_DEADLINE_SECONDS,
    )
    target_frames = duration_seconds * RATE_HZ
    handle = backend.open(device)
    active_error: BaseException | None = None
    capture_started: float | None = None
    frames_read = 0
    previous_loop_started: float | None = None
    try:
        _check_deadline(monotonic, deadline)
        result = backend.set_params(handle)
        if result < 0:
            _raise_alsa_error(backend, "snd_pcm_set_params", result)

        try:
            negotiated_params = backend.get_params(handle)
        except Exception:
            # Buffer details help diagnose a stream failure but must not alter
            # the capture path if an ALSA implementation cannot report them.
            negotiated_params = None
        if negotiated_params is not None:
            diagnostic_state.buffer_frames, diagnostic_state.period_frames = negotiated_params

        _check_deadline(monotonic, deadline)
        result = backend.start(handle)
        if result < 0:
            _raise_alsa_error(backend, "snd_pcm_start", result)
        capture_started = monotonic()
        diagnostic_state.stream_started = True

        buffer = (ctypes.c_int16 * (chunk_frames * CHANNELS))()
        sample_count = 0
        sum_squares = 0
        peak = 0
        nonzero_samples = 0

        while frames_read < target_frames:
            loop_started = monotonic()
            if previous_loop_started is not None:
                loop_gap = max(0.0, loop_started - previous_loop_started)
                diagnostic_state.max_loop_gap_ms = max(
                    diagnostic_state.max_loop_gap_ms,
                    loop_gap * 1000,
                )
            previous_loop_started = loop_started
            diagnostic_state.frames_read = frames_read
            diagnostic_state.elapsed_seconds = max(0.0, loop_started - capture_started)
            _check_deadline(monotonic, deadline)
            remaining = deadline - monotonic()
            wait_ms = min(WAIT_SLICE_MS, max(1, math.ceil(remaining * 1000)))
            ready = backend.wait(handle, wait_ms)
            if ready < 0:
                _raise_alsa_error(backend, "snd_pcm_wait", ready)
            if ready == 0:
                continue

            _check_deadline(monotonic, deadline)
            request_frames = min(chunk_frames, target_frames - frames_read)
            count = backend.readi(handle, buffer, request_frames)
            if count == -errno.EAGAIN:
                continue
            if count < 0:
                _raise_alsa_error(backend, "snd_pcm_readi", count)
            if count == 0:
                continue
            if count > request_frames:
                raise CaptureError("snd_pcm_readi returned more frames than requested")

            frames_read += count
            diagnostic_state.frames_read = frames_read
            for sample in buffer[: count * CHANNELS]:
                magnitude = abs(int(sample))
                peak = max(peak, magnitude)
                sum_squares += int(sample) * int(sample)
                nonzero_samples += int(sample != 0)
                sample_count += 1

        _check_deadline(monotonic, deadline)
        diagnostic_state.frames_read = frames_read
        diagnostic_state.elapsed_seconds = max(0.0, monotonic() - capture_started)
        rms = math.sqrt(sum_squares / sample_count) if sample_count else 0.0
        return CaptureStats(
            frames=frames_read,
            samples=sample_count,
            rms=round(rms, 6),
            peak=peak,
            nonzero_samples=nonzero_samples,
        )
    except BaseException as exc:
        active_error = exc
        if capture_started is not None:
            if previous_loop_started is not None:
                final_loop_gap = max(0.0, monotonic() - previous_loop_started)
                diagnostic_state.max_loop_gap_ms = max(
                    diagnostic_state.max_loop_gap_ms,
                    final_loop_gap * 1000,
                )
            diagnostic_state.frames_read = frames_read
            diagnostic_state.elapsed_seconds = max(0.0, monotonic() - capture_started)
        raise
    finally:
        try:
            close_result = backend.close(handle)
            if close_result < 0 and active_error is None:
                _raise_alsa_error(backend, "snd_pcm_close", close_result)
        except BaseException:
            if active_error is None:
                raise


def _duration_argument(value: str) -> int:
    try:
        duration = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("duration must be an integer number of seconds") from exc
    if not 1 <= duration <= MAX_DURATION_SECONDS:
        raise argparse.ArgumentTypeError(f"duration must be between 1 and {MAX_DURATION_SECONDS} seconds")
    return duration


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture a bounded ALSA test sample and print aggregate statistics only."
    )
    parser.add_argument("--device", required=True, help="explicit ALSA device name, such as hw:0,0")
    parser.add_argument("--duration", required=True, type=_duration_argument, help="capture length in seconds (1..600)")
    parser.add_argument(
        "--acknowledge-microphone-access",
        action="store_true",
        required=True,
        help="confirm that capture of the selected input is authorized",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    diagnostics = CaptureDiagnostics()
    try:
        backend = NativeAlsaBackend()
        stats = capture(backend, args.device, args.duration, diagnostics=diagnostics)
    except (CaptureError, ValueError) as exc:
        print(f"alsa_capture_probe: {exc}", file=sys.stderr)
        if diagnostics.stream_started:
            payload = {
                "frames_read": diagnostics.frames_read,
                "elapsed_seconds": round(diagnostics.elapsed_seconds, 6),
                "max_loop_gap_ms": round(diagnostics.max_loop_gap_ms, 3),
                "buffer_frames": diagnostics.buffer_frames,
                "period_frames": diagnostics.period_frames,
            }
            print(
                "alsa_capture_probe_diagnostics: "
                + json.dumps(payload, separators=(",", ":"), allow_nan=False),
                file=sys.stderr,
            )
        return 1
    print(json.dumps(asdict(stats), separators=(",", ":"), allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

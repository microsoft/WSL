import errno
import io
import json
import math
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

import alsa_capture_probe as probe
from alsa_capture_probe import CaptureDiagnostics, CaptureTimeout, XRunError, capture


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class MockBackend:
    def __init__(self, *, read_results=None, wait_result=1, clock=None, samples=(-3, 4), params=(96_000, 2_048)):
        self.read_results = list(read_results or [])
        self.wait_result = wait_result
        self.clock = clock
        self.samples = samples
        self.params = params
        self.opened_device = None
        self.params_set = False
        self.started = False
        self.closed = False
        self.read_calls = 0
        self.wait_calls = 0

    def open(self, device):
        self.opened_device = device
        return object()

    def set_params(self, handle):
        self.params_set = True
        return 0

    def get_params(self, handle):
        return self.params

    def start(self, handle):
        self.started = True
        return 0

    def wait(self, handle, timeout_ms):
        self.wait_calls += 1
        if self.clock is not None:
            self.clock.now += timeout_ms / 1000
        return self.wait_result

    def readi(self, handle, buffer, frames):
        self.read_calls += 1
        if self.read_results:
            result = self.read_results.pop(0)
            if result < 0:
                return result
        else:
            result = frames
        for index in range(result * 2):
            buffer[index] = self.samples[index % len(self.samples)]
        return result

    def close(self, handle):
        self.closed = True
        return 0

    def error_text(self, code):
        return f"mock errno {-code}"


class CaptureProbeTests(unittest.TestCase):
    def test_aggregates_stereo_samples_and_closes(self):
        backend = MockBackend()

        result = capture(backend, "hw:7,2", 1, chunk_frames=2_048)

        self.assertEqual(backend.opened_device, "hw:7,2")
        self.assertTrue(backend.params_set)
        self.assertTrue(backend.started)
        self.assertTrue(backend.closed)
        self.assertEqual(result.frames, 48_000)
        self.assertEqual(result.samples, 96_000)
        self.assertEqual(result.peak, 4)
        self.assertEqual(result.nonzero_samples, 96_000)
        self.assertEqual(result.rms, round(math.sqrt(12.5), 6))

    def test_read_eagain_retries_after_bounded_wait(self):
        backend = MockBackend(read_results=[-errno.EAGAIN])

        result = capture(backend, "hw:0,0", 1, chunk_frames=48_000)

        self.assertEqual(result.frames, 48_000)
        self.assertEqual(backend.read_calls, 2)
        self.assertGreaterEqual(backend.wait_calls, 2)
        self.assertTrue(backend.closed)

    def test_epipe_is_reported_as_xrun_without_recovery_and_closes(self):
        backend = MockBackend(read_results=[-errno.EPIPE])

        with self.assertRaisesRegex(XRunError, "XRUN.*no recovery attempted"):
            capture(backend, "hw:0,0", 1)

        self.assertEqual(backend.read_calls, 1)
        self.assertTrue(backend.closed)

    def test_xrun_diagnostics_track_frames_elapsed_loop_gap_and_params(self):
        clock = FakeClock()
        backend = MockBackend(read_results=[24_000, -errno.EPIPE], clock=clock)
        diagnostics = CaptureDiagnostics()

        with self.assertRaises(XRunError):
            capture(backend, "hw:0,0", 1, monotonic=clock, chunk_frames=24_000, diagnostics=diagnostics)

        self.assertEqual(diagnostics.frames_read, 24_000)
        self.assertEqual(diagnostics.elapsed_seconds, 0.5)
        self.assertEqual(diagnostics.max_loop_gap_ms, 250.0)
        self.assertEqual(diagnostics.buffer_frames, 96_000)
        self.assertEqual(diagnostics.period_frames, 2_048)
        self.assertTrue(diagnostics.stream_started)
        self.assertTrue(backend.closed)

    def test_main_emits_failure_diagnostics_only_to_stderr(self):
        backend = MockBackend(read_results=[1_024, -errno.EPIPE])
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patch.object(probe, "NativeAlsaBackend", return_value=backend):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                exit_code = probe.main(
                    ["--device", "hw:0,0", "--duration", "1", "--acknowledge-microphone-access"]
                )

        self.assertEqual(exit_code, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("ALSA XRUN during snd_pcm_readi", stderr.getvalue())
        diagnostic_line = next(
            line for line in stderr.getvalue().splitlines() if line.startswith("alsa_capture_probe_diagnostics: ")
        )
        payload = json.loads(diagnostic_line.split(": ", 1)[1])
        self.assertEqual(
            set(payload),
            {"frames_read", "elapsed_seconds", "max_loop_gap_ms", "buffer_frames", "period_frames"},
        )
        self.assertEqual(payload["frames_read"], 1_024)
        self.assertEqual(payload["buffer_frames"], 96_000)
        self.assertEqual(payload["period_frames"], 2_048)

    def test_main_success_json_fields_remain_unchanged(self):
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patch.object(probe, "NativeAlsaBackend", return_value=MockBackend()):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                exit_code = probe.main(
                    ["--device", "hw:0,0", "--duration", "1", "--acknowledge-microphone-access"]
                )

        payload = json.loads(stdout.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(set(payload), {"frames", "samples", "rms", "peak", "nonzero_samples"})
        self.assertEqual(stderr.getvalue(), "")

    def test_wait_timeout_hits_monotonic_deadline_and_closes(self):
        clock = FakeClock()
        backend = MockBackend(wait_result=0, clock=clock)

        with self.assertRaises(CaptureTimeout):
            capture(backend, "hw:0,0", 1, monotonic=clock)

        self.assertGreaterEqual(clock.now, 11.0)
        self.assertTrue(backend.closed)


if __name__ == "__main__":
    unittest.main()

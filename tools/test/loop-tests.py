"""Run the WSL TAEF tests in a loop, capturing per-iteration output and an ETL trace.

Usage:
    python loop-tests.py <iterations> [-- <test.bat args>...]
"""

import atexit
import re
import shutil
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from datetime import datetime, timedelta
from pathlib import Path

import click

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WPRP = REPO_ROOT / "diagnostics" / "wsl.wprp"

START_GROUP_RE = re.compile(r"StartGroup:\s+(\S+)")
END_GROUP_RE = re.compile(r"EndGroup:\s+(\S+)\s+\[(?P<verdict>\w+)\]")
PASS_VERDICTS = {"passed", "skipped"}

# Terminal control sequences (CSI = Control Sequence Introducer, "ESC [").
_CSI = "\x1b["
_ERASE_LINE = f"{_CSI}2K"       # EL  - erase the entire current line
_CURSOR_COL_1 = f"{_CSI}G"      # CHA - move cursor to column 1 (1-based)
_HIDE_CURSOR = f"{_CSI}?25l"    # DECTCEM - hide the cursor
_SHOW_CURSOR = f"{_CSI}?25h"    # DECTCEM - show the cursor
_SGR_RESET = f"{_CSI}0m"        # SGR   - reset all attributes (colors, bold, ...)
# Reset the current line: move cursor home then erase, so the next write
# starts from a clean slate regardless of what was there before.
_RESET_LINE = _CURSOR_COL_1 + _ERASE_LINE


class StatusLine:
    """Single-line status display reused across iterations and refreshed once per second."""

    def __init__(self, total: int):
        self._total = total
        self._iteration = 0
        self._iter_start = time.monotonic()
        self._test = "(initializing)"
        self._passed = 0
        self._failed = 0
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        # Only emit terminal control sequences when stdout is an interactive
        # terminal; otherwise we'd garble redirected output (logs, CI, etc.).
        self._enabled = sys.stdout.isatty()
        self._cursor_hidden = False
        self._thread = threading.Thread(target=self._loop, daemon=True)
        atexit.register(self._safe_restore)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        self._thread.join()
        with self._lock:
            self._clear_locked()
            self._restore_cursor_locked()
        self._safe_restore()

    def begin_iteration(self, iteration: int) -> None:
        with self._lock:
            self._iteration = iteration
            self._iter_start = time.monotonic()
            self._test = "(initializing)"
            self._render_locked()

    def update_test(self, name: str) -> None:
        with self._lock:
            self._test = name

    def record_result(self, passed: bool) -> None:
        with self._lock:
            if passed:
                self._passed += 1
            else:
                self._failed += 1

    def iter_elapsed(self) -> timedelta:
        with self._lock:
            return timedelta(seconds=int(time.monotonic() - self._iter_start))

    @contextmanager
    def pause(self):
        """Clear the status line and hold the refresh lock while the caller prints."""
        with self._lock:
            self._clear_locked()
            self._restore_cursor_locked()
            yield
            # Do not re-render here; the next 1-second tick will redraw below
            # whatever the caller wrote to stdout.

    def _loop(self) -> None:
        while not self._stop_event.is_set():
            with self._lock:
                self._render_locked()
            self._stop_event.wait(1.0)

    def _render_locked(self) -> None:
        if not self._enabled:
            return
        elapsed = timedelta(seconds=int(time.monotonic() - self._iter_start))
        stats_parts: list[str] = []
        if self._passed:
            stats_parts.append(click.style(f"{self._passed} passed", fg="green"))
        if self._failed:
            stats_parts.append(click.style(f"{self._failed} failed", fg="red"))
        stats = (" [" + ", ".join(stats_parts) + "]") if stats_parts else ""
        line = (
            f"Iteration {self._iteration}/{self._total}: "
            f"{self._test}, runtime: {elapsed}{stats}"
        )
        prefix = _RESET_LINE
        if not self._cursor_hidden:
            prefix = _HIDE_CURSOR + prefix
            self._cursor_hidden = True
        sys.stdout.write(prefix + line)
        sys.stdout.flush()

    def _clear_locked(self) -> None:
        if not self._enabled:
            return
        sys.stdout.write(_RESET_LINE)
        sys.stdout.flush()

    def _restore_cursor_locked(self) -> None:
        if self._cursor_hidden:
            sys.stdout.write(_SHOW_CURSOR)
            sys.stdout.flush()
            self._cursor_hidden = False

    def _safe_restore(self) -> None:
        if not self._enabled:
            return
        sys.stdout.write(_SHOW_CURSOR + _SGR_RESET)
        sys.stdout.flush()


def _run_wpr(args: list[str]) -> None:
    """Invoke wpr; raises subprocess.CalledProcessError on non-zero exit."""

    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = subprocess.SW_HIDE  # 0

    subprocess.run(
        ["wpr", *args],
        creationflags=subprocess.CREATE_NEW_CONSOLE,
        startupinfo=si,
        check=True,
    )


def _timestamp_prefix() -> str:
    """Return `[HH:MM:SS.mmm] ` for the current wall-clock time."""
    now = datetime.now()
    return f"[{now.strftime('%H:%M:%S')}.{now.microsecond // 1000:03d}] "


def _print_failed_tests(failed_tests: list[tuple[str, str, list[str]]]) -> None:
    """Print the captured output of every failed test in red."""
    for name, verdict, lines in failed_tests:
        click.secho(
            f"\n----- Failed test: {name} [{verdict}] -----",
            fg="red",
            bold=True,
        )
        click.secho("".join(lines).rstrip("\n"), fg="red")
        click.secho(f"----- end of {name} -----\n", fg="red", bold=True)


def _print_output_tail(output_file: Path, max_lines: int = 80) -> None:
    """Fallback: dump the tail of the captured log when no test-level failure was parsed."""
    try:
        lines = output_file.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as ex:
        click.secho(f"Could not read {output_file}: {ex}", fg="red")
        return

    tail = lines[-max_lines:]
    click.secho(
        f"\n----- Last {len(tail)} line(s) of {output_file.name} -----",
        fg="red",
        bold=True,
    )
    click.secho("\n".join(tail), fg="red")
    click.secho(f"----- end of {output_file.name} -----\n", fg="red", bold=True)


def _run_iteration(
    iteration: int,
    test_bat: Path,
    test_args: tuple[str, ...],
    iter_dir: Path,
    wprp: Path | None,
    status: StatusLine,
) -> bool:
    """Run one iteration. Returns True on success."""
    iter_dir.mkdir(parents=True, exist_ok=True)
    output_file = iter_dir / "test-output.log"
    etl_file = iter_dir / "trace.etl"

    status.begin_iteration(iteration)

    # Start ETL trace before launching tests.
    etl_started = False
    if wprp is not None:
        _run_wpr(["-start", str(wprp), "-filemode"])
        etl_started = True

    cmd = [str(test_bat), *test_args]
    return_code = 1
    current_test: str | None = None
    current_buffer: list[str] = []
    failed_tests: list[tuple[str, str, list[str]]] = []
    try:
        with output_file.open("w", encoding="utf-8", errors="replace") as out:
            out.write(f"{_timestamp_prefix()}$ {subprocess.list2cmdline(cmd)}\n\n")
            out.flush()
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                cwd=str(REPO_ROOT),
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                # Strip embedded NULs (UTF-16 remnants, etc.) so the log stays valid text.
                line = line.replace("\x00", "")
                stamped = _timestamp_prefix() + line
                out.write(stamped)
                out.flush()

                start = START_GROUP_RE.search(stamped)
                if start:
                    current_test = start.group(1)
                    current_buffer = [stamped]
                    status.update_test(current_test)
                    continue

                if current_test is not None:
                    current_buffer.append(stamped)

                end = END_GROUP_RE.search(stamped)
                if end:
                    name = end.group(1)
                    verdict = end.group("verdict")
                    if verdict.lower() not in PASS_VERDICTS:
                        failed_tests.append((name, verdict, current_buffer))
                    current_test = None
                    current_buffer = []
            return_code = proc.wait()
    finally:
        if etl_started:
            _run_wpr(["-stop", str(etl_file)])

    success = return_code == 0 and not failed_tests
    status.record_result(success)

    if not success:
        elapsed = status.iter_elapsed()
        detail = f"exit {return_code}"
        if failed_tests:
            detail += f", {len(failed_tests)} failed test(s)"
        verdict_str = click.style(f"FAILED ({detail})", fg="red", bold=True)
        with status.pause():
            click.echo(
                f"Iteration {iteration}: {verdict_str} in {elapsed} - logs at {iter_dir}"
            )
            if failed_tests:
                _print_failed_tests(failed_tests)
            else:
                _print_output_tail(output_file)

    return success


def _cancel_active_trace() -> None:
    """Best-effort cancel of any leftover wpr session (e.g. from a crashed iteration)."""
    subprocess.run(
        ["wpr", "-cancel"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


@click.command(
    context_settings={"ignore_unknown_options": True, "allow_extra_args": True},
    help=(
        "Run test.bat in a loop, capturing per-iteration output and an ETL trace. "
        "Any TEST_ARGS are forwarded verbatim to test.bat."
    ),
)
@click.argument("iterations", type=click.IntRange(min=1))
@click.argument("test_args", nargs=-1, type=click.UNPROCESSED)
@click.option(
    "--output-dir",
    "-o",
    default="test-loop",
    type=click.Path(file_okay=False),
    show_default=True,
    help="Parent directory; a timestamped subfolder is created for each run.",
)
@click.option(
    "--wprp",
    default=str(DEFAULT_WPRP),
    type=click.Path(dir_okay=False),
    show_default=True,
    help="ETL profile passed to `wpr -start` (passed verbatim; wpr validates it).",
)
@click.option(
    "--no-etl",
    is_flag=True,
    default=False,
    help="Skip ETL tracing (useful when wpr is not available or not needed).",
)
@click.option(
    "--platform",
    "platform_",
    type=click.Choice(["x64", "arm64"], case_sensitive=False),
    default="x64",
    show_default=True,
    help="Target platform subfolder under bin/ where test.bat lives.",
)
@click.option(
    "--target",
    type=click.Choice(["Debug", "Release"], case_sensitive=False),
    default="Debug",
    show_default=True,
    help="Build configuration subfolder under bin/<platform>/ where test.bat lives.",
)
@click.option(
    "--stop-on-failure/--continue-on-failure",
    default=True,
    show_default=True,
    help="Stop the loop on the first failing iteration.",
)
@click.option(
    "--keep-success-logs",
    is_flag=True,
    default=False,
    help="Keep per-iteration log folders even for successful iterations (by default they are deleted).",
)
def main(
    iterations: int,
    test_args: tuple[str, ...],
    output_dir: str,
    wprp: str,
    no_etl: bool,
    platform_: str,
    target: str,
    stop_on_failure: bool,
    keep_success_logs: bool,
) -> None:
    platform_ = platform_.lower()
    target = target.capitalize()
    test_bat_path = REPO_ROOT / "bin" / platform_ / target / "test.bat"

    if not test_bat_path.is_file():
        raise click.ClickException(f"test.bat not found at: {test_bat_path}")

    wprp_path: Path | None = None
    if not no_etl:
        wprp_path = Path(wprp).resolve()

    base_dir = Path(output_dir).resolve()
    run_dir = base_dir / datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    run_dir.mkdir(parents=True, exist_ok=True)

    digits = len(str(iterations))
    failed = 0
    print(f"Running {iterations} iteration(s) of: {subprocess.list2cmdline([str(test_bat_path), *test_args])}")
    print(f"Output: {run_dir}")
    if wprp_path is not None:
        print(f"ETL profile: {wprp_path}")
    else:
        print("ETL tracing: disabled")
    print()

    status = StatusLine(iterations)
    status.start()
    try:
        for i in range(1, iterations + 1):
            iter_dir = run_dir / f"iter-{i:0{digits}d}"
            try:
                ok = _run_iteration(i, test_bat_path, test_args, iter_dir, wprp_path, status)
            except KeyboardInterrupt:
                with status.pause():
                    print("\nInterrupted by user.")
                raise

            if ok and not keep_success_logs:
                shutil.rmtree(iter_dir, ignore_errors=True)

            if not ok:
                failed += 1
                if stop_on_failure:
                    with status.pause():
                        print(f"Stopping after first failure (iteration {i}).")
                    break
    finally:
        status.stop()
        # Make sure no ETL trace is left running from a crashed iteration.
        if wprp_path is not None:
            _cancel_active_trace()

    print()
    if failed:
        click.secho(
            f"Done: {failed} iteration(s) failed.", fg="red", bold=True
        )
        sys.exit(1)
    click.secho("Done: all iterations passed.", fg="green", bold=True)


if __name__ == "__main__":
    main()

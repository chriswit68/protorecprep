#!/usr/bin/env python3
"""End-to-end tests for the replay mode.

The tests record schema-free raw byte streams with the real binary, replay the
resulting captures into a local sink server and compare the received bytes.
They also cover multi-chunk sessions, pacing, unreachable targets, damaged
capture tails and direction filtering.
"""

import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_server(port: int, deadline: float) -> socket.socket:
    last_error = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            sock.settimeout(5.0)
            return sock
        except OSError as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server did not start: {last_error}")


class SinkServer:
    """Accepts any number of connections and records bytes per connection.

    `data()` returns the concatenation of every connection's payload; `payloads()`
    returns one buffer per accepted connection.
    """

    def __init__(self) -> None:
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(16)
        self._listener.settimeout(0.05)
        self.port = self._listener.getsockname()[1]
        self._lock = threading.Lock()
        self._data = bytearray()
        self._buffers: list[bytearray] = []
        self._connections = 0
        self._stop = False
        self._handlers: list[threading.Thread] = []
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def _accept_loop(self) -> None:
        while not self._stop:
            try:
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self._connections += 1
                buffer = bytearray()
                self._buffers.append(buffer)
            handler = threading.Thread(target=self._read_connection, args=(conn, buffer), daemon=True)
            self._handlers.append(handler)
            handler.start()

    def _read_connection(self, conn: socket.socket, buffer: bytearray) -> None:
        conn.settimeout(0.05)
        try:
            while not self._stop:
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                with self._lock:
                    buffer.extend(chunk)
                    self._data.extend(chunk)
        finally:
            conn.close()

    def wait_bytes(self, expected: int, deadline: float) -> None:
        while time.time() < deadline:
            with self._lock:
                if len(self._data) >= expected:
                    return
            time.sleep(0.02)

    def data(self) -> bytes:
        with self._lock:
            return bytes(self._data)

    def payloads(self) -> list[bytes]:
        """Per-connection payloads, in accept order."""
        with self._lock:
            return [bytes(buffer) for buffer in self._buffers]

    @property
    def connections(self) -> int:
        with self._lock:
            return self._connections

    def close(self) -> None:
        self._stop = True
        self._thread.join(timeout=2.0)
        try:
            self._listener.close()
        except OSError:
            pass
        for handler in self._handlers:
            handler.join(timeout=2.0)


def _start_recorder(
    binary: str,
    out_dir: str,
    port: int,
    chunk_interval: str,
) -> tuple[subprocess.Popen, object, str]:
    os.makedirs(out_dir, exist_ok=True)
    stderr_path = os.path.join(out_dir, "record-stderr.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--flat",
            "--framing", "raw",
            "--fsync-interval", "0",
            "--write-batch", "4KiB",
            "--chunk-interval", chunk_interval,
            "--log-level", "warn",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    return proc, stderr_file, stderr_path


def _finish_recorder(proc: subprocess.Popen, stderr_file, stderr_path: str) -> None:
    try:
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"recorder exited with {rc}: {open(stderr_path).read()}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def record_capture(
    binary: str,
    out_dir: str,
    port: int,
    steps: list[tuple[bytes, float]],
    chunk_interval: str = "1m",
) -> None:
    """Records raw bytes; `steps` contains (payload, sleep_after) pairs."""
    proc, stderr_file, stderr_path = _start_recorder(binary, out_dir, port, chunk_interval)
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        for payload, delay in steps:
            sock.sendall(payload)
            if delay > 0:
                time.sleep(delay)
        sock.close()
    finally:
        _finish_recorder(proc, stderr_file, stderr_path)


def record_capture_multi(
    binary: str,
    out_dir: str,
    port: int,
    streams: list[list[tuple[bytes, float]]],
    chunk_interval: str = "1m",
) -> None:
    """Records several simultaneous client connections.

    Each entry of `streams` describes one connection as a list of
    (payload, sleep_after) pairs. Rounds are sent in lockstep across the
    connections so the capture interleaves data from distinct stream ids.
    """
    proc, stderr_file, stderr_path = _start_recorder(binary, out_dir, port, chunk_interval)
    try:
        socks = [wait_for_server(port, time.time() + 10.0)]
        for _ in range(len(streams) - 1):
            extra = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            extra.settimeout(5.0)
            socks.append(extra)
        rounds = max(len(stream) for stream in streams)
        for index in range(rounds):
            for sock, stream in zip(socks, streams):
                if index >= len(stream):
                    continue
                payload, delay = stream[index]
                sock.sendall(payload)
                if delay > 0:
                    time.sleep(delay)
        for sock in socks:
            sock.close()
    finally:
        _finish_recorder(proc, stderr_file, stderr_path)


def run_replay(
    binary: str, args: list[str], timeout: float = 30.0, raw: bool = False
) -> subprocess.CompletedProcess:
    """Runs a replay; `raw` keeps stderr as bytes so '\r' is not normalised."""
    return subprocess.run(
        [binary, "replay", *args],
        capture_output=True,
        text=not raw,
        timeout=timeout,
    )


def capture_files(directory: str) -> list[str]:
    return sorted(
        os.path.join(directory, name)
        for name in os.listdir(directory)
        if name.startswith("capture-") and name.endswith(".pcr")
    )


def progress_lines(stderr: str) -> list[str]:
    """Returns newline-delimited progress lines.

    Splitting on '\\n' only (not str.splitlines) is deliberate: it verifies that
    non-TTY progress output is emitted as complete log lines instead of bare
    '\\r' updates, which line-oriented log consumers such as Docker Compose
    never display until the line is terminated.
    """
    return [
        line
        for line in stderr.split("\n")
        if re.fullmatch(r"(\[\d+/\d+\] )?\S+\.pcr\s+\d+%", line)
    ]


def replay_to_sink(
    binary: str, source_args: list[str], extra_args: list[str], raw: bool = False
) -> tuple[bytes, subprocess.CompletedProcess, int]:
    sink = SinkServer()
    try:
        result = run_replay(
            binary,
            [*source_args, "--target", f"127.0.0.1:{sink.port}", *extra_args],
            raw=raw,
        )
        # Give the sink a moment to drain the final bytes.
        time.sleep(0.1)
        return sink.data(), result, sink.connections
    finally:
        sink.close()


def test_single_file(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "single")
    payloads = [b"hello-", b"replay-" * 200, b"end"]
    record_capture(binary, out_dir, pick_port(), [(p, 0.0) for p in payloads])
    files = capture_files(out_dir)
    check(len(files) == 1, f"expected one capture file, got {files}")

    expected = b"".join(payloads)
    received, result, connections = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
        raw=True,
    )
    stderr = result.stderr.decode("utf-8", "replace")
    check(result.returncode == 0, f"replay failed: {stderr}")
    check(connections == 1, f"expected one replay connection, got {connections}")
    check(received == expected, f"byte mismatch: got {len(received)}, expected {len(expected)}")
    # stderr is a pipe here, so the default (auto) mode must emit plain lines.
    check("\r" not in stderr, f"non-TTY progress used carriage returns: {stderr!r}")
    lines = progress_lines(stderr)
    check(len(lines) >= 2, f"expected separate progress lines: {stderr!r}")
    check(files[0] in lines[-1], f"progress line missing capture path: {stderr!r}")
    check(lines[-1].endswith("100%"), f"progress did not reach 100%: {stderr!r}")

    # --progress off suppresses the progress output entirely.
    _, off_result, _ = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "0", "--direction", "in", "--log-level", "warn", "--progress", "off"],
        raw=True,
    )
    off_stderr = off_result.stderr.decode("utf-8", "replace")
    check(off_result.returncode == 0, f"--progress off replay failed: {off_stderr}")
    check(files[0] not in off_stderr, f"--progress off still printed the path: {off_stderr!r}")

    # --progress inline forces the single in-place line even without a TTY.
    _, inline_result, _ = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "0", "--direction", "in", "--log-level", "warn", "--progress", "inline"],
        raw=True,
    )
    inline_stderr = inline_result.stderr.decode("utf-8", "replace")
    check(inline_result.returncode == 0, f"--progress inline replay failed: {inline_stderr}")
    check("\r" in inline_stderr, f"inline progress missing carriage return: {inline_stderr!r}")


def test_multi_chunk(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "chunks")
    steps = [(b"first-" * 50, 0.25), (b"second-" * 50, 0.25), (b"third-" * 50, 0.0)]
    record_capture(binary, out_dir, pick_port(), steps, chunk_interval="100ms")
    files = capture_files(out_dir)
    check(len(files) >= 2, f"expected multiple capture chunks, got {files}")

    expected = b"".join(payload for payload, _ in steps)
    received, result, connections = replay_to_sink(
        binary,
        ["--dir", out_dir],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
        raw=True,
    )
    stderr = result.stderr.decode("utf-8", "replace")
    check(result.returncode == 0, f"replay failed: {stderr}")
    check(connections == 1, f"stream was split across {connections} connections")
    check(received == expected, f"multi-chunk byte mismatch: got {len(received)}, expected {len(expected)}")
    check(all(path in stderr for path in files), f"progress lines missing files: {stderr!r}")
    check(
        f"[1/{len(files)}]" in stderr and f"[{len(files)}/{len(files)}]" in stderr,
        f"multi-file progress prefix missing: {stderr!r}",
    )
    lines = progress_lines(stderr)
    check("\r" not in stderr, f"non-TTY progress used carriage returns: {stderr!r}")
    check(
        any(line.startswith(f"[1/{len(files)}] ") for line in lines),
        f"first chunk progress prefix missing: {stderr!r}",
    )
    check(
        any(line.startswith(f"[{len(files)}/{len(files)}] ") for line in lines),
        f"last chunk progress prefix missing: {stderr!r}",
    )
    check(len(lines) >= 2 * len(files), f"expected start/end lines per chunk: {stderr!r}")


def test_multi_chunk_pacing(binary: str, workdir: str) -> None:
    """Paced replays across chunk boundaries must not invent huge gaps."""
    out_dir = os.path.join(workdir, "chunk-pacing")
    steps = [(b"alpha-" * 40, 0.3), (b"beta-" * 40, 0.3), (b"gamma-" * 40, 0.0)]
    record_capture(binary, out_dir, pick_port(), steps, chunk_interval="200ms")
    files = capture_files(out_dir)
    check(len(files) >= 2, f"expected multiple capture chunks, got {files}")

    expected = b"".join(payload for payload, _ in steps)
    sink = SinkServer()
    start = time.monotonic()
    try:
        try:
            result = run_replay(
                binary,
                [
                    "--dir", out_dir,
                    "--target", f"127.0.0.1:{sink.port}",
                    "--speed", "1",
                    "--direction", "in",
                    "--log-level", "warn",
                ],
                timeout=10.0,
            )
        except subprocess.TimeoutExpired as exc:  # noqa: TRY302
            raise AssertionError(f"paced multi-chunk replay stalled: {exc}") from exc
        time.sleep(0.1)
        received = sink.data()
    finally:
        sink.close()
    elapsed = time.monotonic() - start

    check(result.returncode == 0, f"paced multi-chunk replay failed: {result.stderr}")
    check(received == expected, f"paced multi-chunk byte mismatch: got {len(received)}, expected {len(expected)}")
    check(elapsed < 8.0, f"paced multi-chunk replay was far too slow: {elapsed:.2f}s")


def test_multi_stream(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "multi-stream")
    stream_a = [(b"alpha-", 0.0), (b"one", 0.01), (b"-tail", 0.0)]
    stream_b = [(b"beta-", 0.0), (b"two", 0.01), (b"-tail", 0.0)]
    record_capture_multi(binary, out_dir, pick_port(), [stream_a, stream_b])
    files = capture_files(out_dir)
    check(bool(files), "multi-stream recording produced no capture files")

    expected_a = b"".join(payload for payload, _ in stream_a)
    expected_b = b"".join(payload for payload, _ in stream_b)

    sink = SinkServer()
    try:
        result = run_replay(
            binary,
            [
                "--dir", out_dir,
                "--target", f"127.0.0.1:{sink.port}",
                "--speed", "0",
                "--direction", "in",
                "--log-level", "warn",
            ],
        )
        time.sleep(0.1)
        payloads = sink.payloads()
        connections = sink.connections
    finally:
        sink.close()

    check(result.returncode == 0, f"multi-stream replay failed: {result.stderr}")
    check(connections == 2, f"expected two connections, got {connections}")
    check(len(payloads) == 2, f"expected two payload buffers, got {len(payloads)}")
    check(
        sorted(payloads) == sorted([expected_a, expected_b]),
        f"per-connection payloads differ: {payloads}",
    )


def test_pacing(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "pacing")
    gap = 1.0
    record_capture(binary, out_dir, pick_port(), [(b"early", 0.0), (b"late", gap)])
    files = capture_files(out_dir)
    check(len(files) == 1, f"expected one pacing capture, got {files}")

    expected = b"earlylate"
    start = time.monotonic()
    received, result, _ = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "2", "--direction", "in", "--log-level", "warn"],
    )
    elapsed_2x = time.monotonic() - start
    check(result.returncode == 0, f"2x replay failed: {result.stderr}")
    check(received == expected, "2x replay payload mismatch")
    check(elapsed_2x >= gap * 0.35, f"2x replay was too fast: {elapsed_2x:.3f}s")
    check(elapsed_2x < gap * 1.5, f"2x replay was too slow: {elapsed_2x:.3f}s")

    start = time.monotonic()
    received, result, _ = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
    )
    elapsed_max = time.monotonic() - start
    check(result.returncode == 0, f"max-speed replay failed: {result.stderr}")
    check(received == expected, "max-speed replay payload mismatch")
    check(elapsed_max < gap * 0.5, f"max-speed replay was throttled: {elapsed_max:.3f}s")


def test_unreachable_target(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "unreachable")
    record_capture(binary, out_dir, pick_port(), [(b"data", 0.0)])
    files = capture_files(out_dir)
    dead_port = pick_port()
    result = run_replay(
        binary,
        [
            "--rec", files[0],
            "--target", f"127.0.0.1:{dead_port}",
            "--speed", "0",
            "--connect-timeout", "100ms",
            "--connect-retries", "2",
            "--retry-interval", "50ms",
            "--log-level", "warn",
        ],
        timeout=20,
    )
    check(result.returncode == 1, f"unreachable target not detected: {result.returncode}")
    combined = result.stdout + result.stderr
    check("cannot connect" in combined or "connect" in combined, f"missing connect error: {combined}")


def test_damaged_tail(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "damaged")
    payloads = [b"valid-prefix-" * 40]
    record_capture(binary, out_dir, pick_port(), [(payloads[0], 0.0)])
    files = capture_files(out_dir)
    check(len(files) == 1, f"expected one capture, got {files}")
    with open(files[0], "ab") as fh:
        fh.write(b"\x02\x40\x00\x00\x00\x01\x02\x03\x04\x05")

    received, result, _ = replay_to_sink(
        binary, ["--rec", files[0]], ["--speed", "0", "--direction", "in", "--log-level", "warn"]
    )
    check(result.returncode == 0, f"non-strict damaged replay failed: {result.stderr}")
    check(received == payloads[0], "valid prefix was not replayed")
    check("damaged capture" in result.stderr, f"damage warning missing: {result.stderr}")

    sink = SinkServer()
    try:
        strict = run_replay(
            binary,
            [
                "--rec", files[0],
                "--target", f"127.0.0.1:{sink.port}",
                "--speed", "0",
                "--strict",
                "--log-level", "warn",
            ],
        )
    finally:
        sink.close()
    check(strict.returncode == 1, f"strict damaged replay should fail, got {strict.returncode}")
    check("damaged capture" in (strict.stdout + strict.stderr), strict.stderr)


def test_direction_out(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "direction")
    record_capture(binary, out_dir, pick_port(), [(b"inbound-only", 0.0)])
    files = capture_files(out_dir)
    received, result, connections = replay_to_sink(
        binary, ["--rec", files[0]], ["--speed", "0", "--direction", "out", "--log-level", "warn"]
    )
    check(result.returncode == 0, f"direction=out replay failed: {result.stderr}")
    check(connections == 0, f"direction=out opened {connections} connection(s)")
    check(received == b"", f"direction=out sent {len(received)} bytes")


def test_proto_validation(binary: str, proto: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "proto")
    record_capture(binary, out_dir, pick_port(), [(b"schema-free", 0.0)])
    files = capture_files(out_dir)

    received, result, _ = replay_to_sink(
        binary,
        ["--rec", files[0]],
        ["--speed", "0", "--direction", "in", "--proto", proto, "--log-level", "warn"],
    )
    check(result.returncode == 0, f"schema mismatch without --strict failed: {result.stderr}")
    check(received == b"schema-free", "schema mismatch changed replayed bytes")
    check("different schema" in result.stderr, f"schema warning missing: {result.stderr}")

    sink = SinkServer()
    try:
        strict = run_replay(
            binary,
            [
                "--rec", files[0],
                "--target", f"127.0.0.1:{sink.port}",
                "--speed", "0",
                "--proto", proto,
                "--strict",
                "--log-level", "warn",
            ],
        )
    finally:
        sink.close()
    check(strict.returncode == 1, f"strict schema mismatch should fail, got {strict.returncode}")
    check("different schema" in (strict.stdout + strict.stderr), strict.stderr)


def main() -> int:
    binary = os.path.abspath(sys.argv[1])
    proto = os.path.abspath(sys.argv[2])
    workdir = tempfile.mkdtemp(prefix="prp-replay-")

    try:
        test_single_file(binary, workdir)
        test_multi_chunk(binary, workdir)
        test_multi_chunk_pacing(binary, workdir)
        test_multi_stream(binary, workdir)
        test_pacing(binary, workdir)
        test_unreachable_target(binary, workdir)
        test_damaged_tail(binary, workdir)
        test_direction_out(binary, workdir)
        if not os.environ.get("PRP_SKIP_PROTO_TESTS"):
            test_proto_validation(binary, proto, workdir)
        print("replay integration test passed")
        return 0
    finally:
        if os.environ.get("PRP_KEEP_TEST_ARTIFACTS"):
            print(f"artifacts kept in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        sys.exit(1)

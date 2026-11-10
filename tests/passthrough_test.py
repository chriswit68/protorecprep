#!/usr/bin/env python3
"""End-to-end tests for the passthrough mode.

Passthrough records inbound client bytes and simultaneously forwards them to a
target server, then forwards the target's responses back to the client. These
tests use the real binary against local sink servers and verify both live
directions plus the recorded capture.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

from replay_test import (
    SinkServer,
    capture_files,
    pick_port,
    replay_to_sink,
    run_replay,
    wait_for_server,
)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class SlowSink(SinkServer):
    """A sink that delays reading so the passthrough queue fills up."""

    def __init__(self, initial_delay: float) -> None:
        self._initial_delay = initial_delay
        super().__init__()

    def _read_connection(self, conn: socket.socket, buffer: bytearray) -> None:
        time.sleep(self._initial_delay)
        super()._read_connection(conn, buffer)


class RespondingServer(SinkServer):
    """A sink that answers each connection once the peer has sent data.

    `response` is sent verbatim. With `close_after_response` the target
    connection closes right after the reply, which exercises the
    "target went away" path.
    """

    def __init__(self, response: bytes, close_after_response: bool = False) -> None:
        self._response = response
        self._close_after_response = close_after_response
        super().__init__()

    def _read_connection(self, conn: socket.socket, buffer: bytearray) -> None:
        conn.settimeout(0.05)
        sent = False
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
                if sent or not self._response:
                    continue
                conn.settimeout(None)  # block until a large reply is written out
                try:
                    conn.sendall(self._response)
                finally:
                    conn.settimeout(0.05)
                sent = True
                if self._close_after_response:
                    break
        finally:
            conn.close()


def recv_until(sock: socket.socket, expected: int, deadline: float) -> bytes:
    data = bytearray()
    sock.settimeout(0.2)
    while len(data) < expected and time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def start_passthrough(
    binary: str,
    out_dir: str,
    listen_port: int,
    target_port: int,
    extra_args: list[str] | None = None,
    framing: str = "raw",
    log_level: str = "warn",
) -> tuple[subprocess.Popen, object, str]:
    os.makedirs(out_dir, exist_ok=True)
    stderr_path = os.path.join(out_dir, "passthrough-stderr.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    args = [
        binary,
        "passthrough",
        "--listen", f"127.0.0.1:{listen_port}",
        "--out", out_dir,
        "--flat",
        "--framing", framing,
        "--fsync-interval", "0",
        "--write-batch", "4KiB",
        "--target", f"127.0.0.1:{target_port}",
        "--log-level", log_level,
    ]
    if extra_args:
        args.extend(extra_args)
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=stderr_file)
    return proc, stderr_file, stderr_path


def finish_passthrough(proc: subprocess.Popen, stderr_file, stderr_path: str) -> str:
    try:
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=30)
        with open(stderr_path, errors="replace") as handle:
            output = handle.read()
        check(rc == 0, f"passthrough exited with {rc}: {output}")
        return output
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def replay_payloads(binary: str, out_dir: str) -> list[bytes]:
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
    finally:
        sink.close()
    check(result.returncode == 0, f"replaying capture failed: {result.stderr}")
    return payloads


def replay_direction(binary: str, out_dir: str, direction: str) -> bytes:
    received, result, _ = replay_to_sink(
        binary,
        ["--dir", out_dir],
        ["--speed", "0", "--direction", direction, "--log-level", "warn"],
    )
    check(result.returncode == 0, f"replaying {direction} capture failed: {result.stderr}")
    return received


def verify_recording(binary: str, out_dir: str, expected: bytes) -> None:
    files = capture_files(out_dir)
    check(bool(files), f"no capture files in {out_dir}")
    received, result, _ = replay_to_sink(
        binary,
        ["--dir", out_dir],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
    )
    check(result.returncode == 0, f"replaying capture failed: {result.stderr}")
    check(received == expected, f"recorded bytes differ: got {len(received)}, expected {len(expected)}")


def test_basic(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "basic")
    sink = SinkServer()
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(binary, out_dir, listen_port, sink.port)
    payloads = [b"hello-", b"passthrough-" * 300, b"end"]
    expected = b"".join(payloads)
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        for payload in payloads:
            sock.sendall(payload)
        sock.close()
        sink.wait_bytes(len(expected), time.time() + 10.0)
        check(sink.data() == expected, f"forwarded bytes differ: got {len(sink.data())}")
        check(sink.connections == 1, f"expected one target connection, got {sink.connections}")
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    verify_recording(binary, out_dir, expected)


def test_multi_stream(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "multi-stream")
    sink = SinkServer()
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(binary, out_dir, listen_port, sink.port)
    payload_a = b"alpha-" * 100 + b"one"
    payload_b = b"beta-" * 100 + b"two"
    expected = payload_a + payload_b
    try:
        first = wait_for_server(listen_port, time.time() + 10.0)
        second = socket.create_connection(("127.0.0.1", listen_port), timeout=2.0)
        first.sendall(payload_a)
        second.sendall(payload_b)
        first.close()
        second.close()
        sink.wait_bytes(len(expected), time.time() + 10.0)
        time.sleep(0.1)
        check(sink.connections == 2, f"expected two target connections, got {sink.connections}")
        check(sorted(sink.payloads()) == sorted([payload_a, payload_b]),
              f"per-stream payloads differ: {sink.payloads()}")
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    recorded = replay_payloads(binary, out_dir)
    check(sorted(recorded) == sorted([payload_a, payload_b]),
          f"recorded per-stream payloads differ: {recorded}")


def test_unreachable_target(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "unreachable")
    dead_port = pick_port()
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(
        binary,
        out_dir,
        listen_port,
        dead_port,
        extra_args=[
            "--connect-timeout", "100ms",
            "--connect-retries", "1",
            "--retry-interval", "10ms",
        ],
    )
    payload = b"recorded-despite-forward-failure"
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.sendall(payload)
        time.sleep(0.3)
        sock.close()
        time.sleep(0.4)
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
    output = open(stderr_path, errors="replace").read()
    check("forwarding to" in output and "recording continues" in output,
          f"missing forward failure warning: {output}")
    verify_recording(binary, out_dir, payload)


def test_backpressure(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "backpressure")
    sink = SlowSink(initial_delay=0.75)
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(
        binary,
        out_dir,
        listen_port,
        sink.port,
        extra_args=["--forward-queue", "1MiB"],
    )
    payload = (b"backpressure-block-" * ((8 * 1024 * 1024) // 19 + 1))[: 8 * 1024 * 1024]
    sender_error: list[BaseException] = []

    def send_all() -> None:
        try:
            sock.sendall(payload)
        except BaseException as exc:  # noqa: BLE001
            sender_error.append(exc)

    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.settimeout(None)
        thread = threading.Thread(target=send_all, daemon=True)
        thread.start()
        thread.join(timeout=30.0)
        check(not thread.is_alive(), "sender did not finish; source reads never resumed")
        check(not sender_error, f"sender failed: {sender_error}")
        sock.close()
        sink.wait_bytes(len(payload), time.time() + 30.0)
        check(sink.data() == payload,
              f"backpressure lost bytes: got {len(sink.data())}, expected {len(payload)}")
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    verify_recording(binary, out_dir, payload)


def test_lazy_connection(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "lazy")
    sink = SinkServer()
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(binary, out_dir, listen_port, sink.port)
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.close()
        time.sleep(0.3)
        check(sink.connections == 0, f"target connected without inbound data ({sink.connections})")
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()


def test_response_forwarding(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "responses")
    response = b"target-response-" * 64 + b"ok"
    sink = RespondingServer(response)
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(
        binary, out_dir, listen_port, sink.port, log_level="info"
    )
    request = b"client-request"
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.sendall(request)
        received = recv_until(sock, len(response), time.time() + 10.0)
        check(received == response,
              f"response mismatch: got {len(received)}, expected {len(response)}")
        sock.close()
        sink.wait_bytes(len(request), time.time() + 10.0)
        check(sink.data() == request, f"target received {sink.data()!r}")
        time.sleep(0.2)
    finally:
        output = finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    check("responses to clients" in output,
          f"summary did not report response bytes: {output}")
    check(replay_direction(binary, out_dir, "out") == response,
          "captured responses differ from the live response")
    check(replay_direction(binary, out_dir, "in") == request,
          "captured requests differ from the live request")


def test_target_eof_keeps_client(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "target-eof")
    response = b"target-goodbye"
    sink = RespondingServer(response, close_after_response=True)
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(binary, out_dir, listen_port, sink.port)
    first = b"before-target-eof"
    trailing = [b"after-target-eof-1", b"after-target-eof-2"]
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.sendall(first)
        received = recv_until(sock, len(response), time.time() + 10.0)
        check(received == response, f"response before target EOF differs: {received!r}")
        for payload in trailing:
            sock.sendall(payload)
            time.sleep(0.2)
        # The client socket must still be usable after the target went away.
        sock.settimeout(2.0)
        sock.sendall(b"still-alive")
        trailing.append(b"still-alive")
        sock.close()
        time.sleep(0.3)
    finally:
        output = finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    check("recording continues" in output, f"missing target loss warning: {output}")
    expected = first + b"".join(trailing)
    recorded = replay_direction(binary, out_dir, "in")
    check(recorded == expected,
          f"inbound data after target EOF was lost: got {len(recorded)}, expected {len(expected)}")


def test_response_backpressure(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "response-backpressure")
    chunk = b"server-response-block-"
    response = (chunk * ((8 * 1024 * 1024) // len(chunk) + 1))[: 8 * 1024 * 1024]
    sink = RespondingServer(response)
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(
        binary,
        out_dir,
        listen_port,
        sink.port,
        extra_args=["--forward-queue", "1MiB"],
    )
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.sendall(b"go")
        time.sleep(0.75)  # let the client write backlog hit the queue limit
        received = recv_until(sock, len(response), time.time() + 30.0)
        check(len(received) == len(response),
              f"response truncated under backpressure: got {len(received)}")
        check(received == response, "response bytes differ under backpressure")
        sock.close()
        time.sleep(0.3)
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()
    recorded = replay_direction(binary, out_dir, "out")
    check(recorded == response,
          f"captured response truncated: got {len(recorded)}, expected {len(response)}")


TEST_RECORD_REQUEST = b"\x06\x08\x01\x12\x03req"       # TestRecord(seq=1, name="req")
TEST_RECORD_RESPONSE = b"\x08\x08\x07\x12\x04resp"    # TestRecord(seq=7, name="resp")


def test_response_json(binary: str, proto: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "response-json")
    sink = RespondingServer(TEST_RECORD_RESPONSE)
    listen_port = pick_port()
    proc, stderr_file, stderr_path = start_passthrough(
        binary,
        out_dir,
        listen_port,
        sink.port,
        extra_args=["--proto", proto, "--message-type", "demo.TestRecord", "--json=jsonl"],
        framing="varint-length",
    )
    try:
        sock = wait_for_server(listen_port, time.time() + 10.0)
        sock.sendall(TEST_RECORD_REQUEST)
        received = recv_until(sock, len(TEST_RECORD_RESPONSE), time.time() + 10.0)
        check(received == TEST_RECORD_RESPONSE, f"typed response differs: {received!r}")
        sock.close()
        time.sleep(0.2)
    finally:
        finish_passthrough(proc, stderr_file, stderr_path)
        sink.close()

    events_path = os.path.join(out_dir, "events.jsonl")
    check(os.path.exists(events_path), f"missing JSONL output in {out_dir}")
    out_events = []
    with open(events_path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            if event.get("dir") == "out" and "message" in event:
                out_events.append(event)
    check(bool(out_events), f"no outbound JSON event in {events_path}")
    check(any(event["type"] == "demo.TestRecord" and event["message"].get("name") == "resp"
              for event in out_events),
          f"unexpected outbound JSON events: {out_events}")
    check(replay_direction(binary, out_dir, "out") == TEST_RECORD_RESPONSE,
          "typed response capture differs from the live response")


def test_send_options_rejected(binary: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "send-rejected")
    sink = SinkServer()
    listen_port = pick_port()
    try:
        result = subprocess.run(
            [
                binary,
                "passthrough",
                "--listen", f"127.0.0.1:{listen_port}",
                "--out", out_dir,
                "--flat",
                "--target", f"127.0.0.1:{sink.port}",
                "--send-type", "demo.TestRecord",
                "--log-level", "warn",
            ],
            capture_output=True,
            text=True,
            timeout=30.0,
        )
    finally:
        sink.close()
    check(result.returncode != 0, "passthrough accepted --send-type")
    combined = result.stdout + result.stderr
    check("--send-*" in combined and "passthrough" in combined,
          f"unexpected --send-* rejection message: {combined}")


def main() -> int:
    binary = os.path.abspath(sys.argv[1])
    proto = os.path.abspath(sys.argv[2])
    workdir = tempfile.mkdtemp(prefix="prp-passthrough-")

    try:
        test_basic(binary, workdir)
        test_multi_stream(binary, workdir)
        test_response_forwarding(binary, workdir)
        test_target_eof_keeps_client(binary, workdir)
        test_response_backpressure(binary, workdir)
        test_unreachable_target(binary, workdir)
        test_backpressure(binary, workdir)
        test_lazy_connection(binary, workdir)
        test_send_options_rejected(binary, workdir)
        if not os.environ.get("PRP_SKIP_PROTO_TESTS"):
            test_response_json(binary, proto, workdir)
        print("passthrough integration test passed")
        return 0
    finally:
        if os.environ.get("PRP_KEEP_TEST_ARTIFACTS"):
            print(f"artifacts kept in {workdir}")
        else:
            import shutil

            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        sys.exit(1)

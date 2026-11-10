#!/usr/bin/env python3
"""End-to-end tests for the 8-byte typed framing.

The wire header is [u32 LE message type id][u32 LE payload length]. The tests
record typed streams, verify JSON type resolution (including an unknown id),
check that passthrough forwarding stays byte-exact and exercise typed sends.
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from integration_test import enc_record
from replay_test import (
    SinkServer,
    pick_port,
    replay_to_sink,
    wait_for_server,
)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def typed_frame(type_id: int, payload: bytes) -> bytes:
    return struct.pack("<II", type_id, len(payload)) + payload


def recv_exact(sock: socket.socket, count: int, deadline: float) -> bytes:
    buffer = bytearray()
    while len(buffer) < count and time.time() < deadline:
        chunk = sock.recv(count - len(buffer))
        if not chunk:
            break
        buffer.extend(chunk)
    return bytes(buffer)


def recv_typed_frame(sock: socket.socket, deadline: float) -> tuple[int, bytes]:
    header = recv_exact(sock, 8, deadline)
    check(len(header) == 8, f"short typed header: {header!r}")
    type_id, length = struct.unpack("<II", header)
    payload = recv_exact(sock, length, deadline)
    check(len(payload) == length, f"short typed payload: {len(payload)}/{length}")
    return type_id, payload


def start(binary: str, mode: str, out_dir: str, port: int, extra: list[str]) -> tuple[subprocess.Popen, object, str]:
    os.makedirs(out_dir, exist_ok=True)
    stderr_path = os.path.join(out_dir, "stderr.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    args = [
        binary,
        mode,
        "--listen", f"127.0.0.1:{port}",
        "--out", out_dir,
        "--flat",
        "--fsync-interval", "0",
        "--write-batch", "4KiB",
        "--log-level", "info",
        "--framing", "typed-length-le",
        "--proto-package", "demo",
        "--type-map", "0=VersionInformation",
        "--type-map", "1=TestRecord",
        "--type-map", "2=Nested",
        *extra,
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=stderr_file)
    return proc, stderr_file, stderr_path


def stop(proc: subprocess.Popen, stderr_file, stderr_path: str) -> None:
    try:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"process exited with {rc}:\n{open(stderr_path).read()}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def read_events(out_dir: str) -> list[dict]:
    events = []
    with open(os.path.join(out_dir, "events.jsonl")) as fh:
        for line in fh:
            events.append(json.loads(line))
    return events


def test_record_typed(binary: str, proto: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "record")
    port = pick_port()
    proc, stderr_file, stderr_path = start(
        binary, "record", out_dir, port,
        ["--proto", proto, "--json"],
    )
    record = typed_frame(1, enc_record(7, "typed-in"))
    nested = typed_frame(2, b"\x0a\x0cnested-typed")
    unknown = typed_frame(9, b"not-mapped")
    wire = record + unknown + nested
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        sock.sendall(wire)
        sock.close()
        time.sleep(0.2)
    finally:
        stop(proc, stderr_file, stderr_path)

    events = read_events(out_dir)
    messages = [e for e in events if "message" in e]
    check(len(messages) == 2, messages)
    by_type = {m["type"]: m for m in messages}
    check("demo.TestRecord" in by_type, by_type)
    check(by_type["demo.TestRecord"]["type_id"] == 1, by_type["demo.TestRecord"])
    check(by_type["demo.TestRecord"]["message"]["name"] == "typed-in", by_type)
    check("demo.Nested" in by_type, by_type)
    check(by_type["demo.Nested"]["type_id"] == 2, by_type["demo.Nested"])

    unparsed = [e for e in events if e.get("unparsed")]
    check(len(unparsed) == 1, unparsed)
    check(unparsed[0]["type_id"] == 9, unparsed[0])
    check("unknown message type id 9" in unparsed[0]["reason"], unparsed[0])

    # The binary capture keeps the complete wire bytes including headers.
    with open(os.path.join(out_dir, "summary.json")) as fh:
        summary = json.load(fh)
    check(summary["data_bytes"] == len(wire), summary)

    received, result, connections = replay_to_sink(
        binary,
        ["--dir", out_dir],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
    )
    check(result.returncode == 0, f"replay failed: {result.stderr}")
    check(connections == 1, f"expected one replay connection, got {connections}")
    check(received == wire, f"replayed bytes differ: {len(received)}/{len(wire)}")


def test_passthrough_typed(binary: str, proto: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "passthrough")
    port = pick_port()
    sink = SinkServer()
    proc, stderr_file, stderr_path = start(
        binary, "passthrough", out_dir, port,
        [
            "--proto", proto, "--json",
            "--target", f"127.0.0.1:{sink.port}",
        ],
    )
    wire = typed_frame(1, enc_record(1, "pass-one")) + typed_frame(2, b"\x0a\x08pass-two")
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        sock.sendall(wire)
        sink.wait_bytes(len(wire), time.time() + 10.0)
        check(sink.data() == wire, "passthrough did not forward typed bytes exactly")
        sock.close()
        time.sleep(0.2)
    finally:
        stop(proc, stderr_file, stderr_path)
        sink.close()

    messages = [e for e in read_events(out_dir) if "message" in e]
    check(len(messages) == 2, messages)
    check({m["type_id"] for m in messages} == {1, 2}, messages)

    received, result, _ = replay_to_sink(
        binary,
        ["--dir", out_dir],
        ["--speed", "0", "--direction", "in", "--log-level", "warn"],
    )
    check(result.returncode == 0, f"replay failed: {result.stderr}")
    check(received == wire, f"captured typed bytes differ: {len(received)}/{len(wire)}")


def test_send_typed(binary: str, proto: str, workdir: str) -> None:
    out_dir = os.path.join(workdir, "send")
    port = pick_port()
    proc, stderr_file, stderr_path = start(
        binary, "record", out_dir, port,
        [
            "--proto", proto, "--json",
            "--send-type", "TestRecord",
            "--send-content", '{"seq":99,"name":"typed-out"}',
            "--send-once-after", "0ms",
        ],
    )
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        type_id, payload = recv_typed_frame(sock, time.time() + 10.0)
        check(type_id == 1, f"unexpected send type id {type_id}")
        check(b"typed-out" in payload, payload)
        sock.close()
        time.sleep(0.2)
    finally:
        stop(proc, stderr_file, stderr_path)

    events = read_events(out_dir)
    outbound = [e for e in events if e.get("dir") == "out" and "message" in e]
    check(len(outbound) >= 1, events)
    check(outbound[0]["type"] == "demo.TestRecord", outbound[0])
    check(outbound[0]["type_id"] == 1, outbound[0])
    check(outbound[0]["message"]["name"] == "typed-out", outbound[0])


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: typed_test.py <binary> <sample.proto>", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    proto = sys.argv[2]
    workdir = tempfile.mkdtemp(prefix="prp-typed-")
    try:
        test_record_typed(binary, proto, workdir)
        test_passthrough_typed(binary, proto, workdir)
        test_send_typed(binary, proto, workdir)
    finally:
        import shutil

        shutil.rmtree(workdir, ignore_errors=True)
    print("typed tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

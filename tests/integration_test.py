#!/usr/bin/env python3
"""End-to-end test for the recording mode.

Starts the real binary, sends length-prefixed protobuf frames over TCP, stops
it with SIGINT and verifies the capture, JSONL sidecar, inspect output and the
crash-recovery path.
"""

import base64
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


def enc_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def enc_record(seq: int, name: str) -> bytes:
    # demo.TestRecord: field 1 = uint64 seq, field 2 = string name
    name_bytes = name.encode()
    body = b"\x08" + enc_varint(seq)
    body += b"\x12" + enc_varint(len(name_bytes)) + name_bytes
    return body


def frame(payload: bytes) -> bytes:
    return enc_varint(len(payload)) + payload


def dec_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise AssertionError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


def dec_record(payload: bytes) -> dict:
    """Minimal decoder for demo.TestRecord (seq=1 uint64, name=2 string)."""
    out: dict = {}
    offset = 0
    while offset < len(payload):
        key, offset = dec_varint(payload, offset)
        field = key >> 3
        wire_type = key & 0x7
        if field == 1 and wire_type == 0:
            out["seq"], offset = dec_varint(payload, offset)
        elif field == 2 and wire_type == 2:
            length, offset = dec_varint(payload, offset)
            out["name"] = payload[offset : offset + length].decode()
            offset += length
        else:
            raise AssertionError(f"unexpected field {field} wire type {wire_type}")
    return out


def read_frames(sock: socket.socket, count: int, deadline: float) -> list[bytes]:
    buf = b""
    frames: list[bytes] = []
    while len(frames) < count and time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        while len(frames) < count:
            if not buf:
                break
            # Only consume a frame once its full payload has arrived.
            probe = 0
            try:
                length, probe = dec_varint(buf, 0)
            except AssertionError:
                break
            if len(buf) < probe + length:
                break
            frames.append(buf[probe : probe + length])
            buf = buf[probe + length :]
    return frames


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


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_outbound(binary: str, proto: str, workdir: str, port: int) -> None:
    """Verifies configured server->client messages reach the wire and the capture."""
    out_dir = os.path.join(workdir, "out-send")
    stderr_path = os.path.join(workdir, "stderr-send.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--proto", proto,
            "--message-type", "demo.TestRecord",
            "--json",
            "--fsync-interval", "0",
            "--write-batch", "4KiB",
            "--send-type", "demo.TestRecord",
            "--send-content", '{"seq":999,"name":"srv"}',
            "--send-once-after", "50ms",
            "--send-every", "100ms",
            "--send-count", "3",
            "--log-level", "info",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        sock.settimeout(0.2)
        received = read_frames(sock, 3, time.time() + 5.0)
        check(len(received) == 3, f"expected 3 outbound frames, got {len(received)}")
        for payload in received:
            record = dec_record(payload)
            check(record.get("seq") == 999, f"unexpected outbound seq: {record}")
            check(record.get("name") == "srv", f"unexpected outbound name: {record}")

        # Keep the connection busy so the outbound data is also captured.
        sock.sendall(frame(enc_record(1, "client")))
        sock.close()
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"send recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        with open(os.path.join(session_dir, "meta.json")) as fh:
            meta = json.load(fh)
        check(meta["send"]["type"] == "demo.TestRecord", meta["send"])
        check(meta["send"]["count"] == 3, meta["send"])
        check(meta["send"]["every_ms"] == 100, meta["send"])

        out_messages = []
        with open(os.path.join(session_dir, "events.jsonl")) as fh:
            for line in fh:
                obj = json.loads(line)
                if obj.get("dir") == "out":
                    out_messages.append(obj)
        check(len(out_messages) == 3, f"expected 3 recorded outbound messages, got {len(out_messages)}")
        for obj in out_messages:
            check(obj["type"] == "demo.TestRecord", obj)
            check(str(obj["message"]["seq"]) == "999", obj)
            check(obj["message"]["name"] == "srv", obj)

        inspect = subprocess.run(
            [binary, "inspect", "--dir", session_dir], capture_output=True, text=True, timeout=30
        )
        check(inspect.returncode == 0, f"inspect failed: {inspect.stdout}\n{inspect.stderr}")
        check("status       : OK" in inspect.stdout, inspect.stdout)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def test_chunking_and_json_file(binary: str, proto: str, workdir: str) -> None:
    """Time-based capture chunks plus a configurable JSON data file."""
    port = pick_port()
    out_dir = os.path.join(workdir, "out-chunks")
    stderr_path = os.path.join(workdir, "chunks.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--proto", proto,
            "--message-type", "demo.TestRecord",
            "--json",
            "--json-file", "custom-events.jsonl",
            "--chunk-interval", "200ms",
            "--chunk-size", "0",
            "--fsync-interval", "0",
            "--write-batch", "4KiB",
            "--log-level", "info",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    sent = 0
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        deadline = time.time() + 1.2
        while time.time() < deadline:
            sock.sendall(frame(enc_record(sent, f"chunk-{sent}")))
            sent += 1
            time.sleep(0.1)
        sock.close()
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"chunk recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        json_path = os.path.join(session_dir, "custom-events.jsonl")
        check(os.path.exists(json_path), "custom JSON file was not created")
        check(
            not os.path.exists(os.path.join(session_dir, "events.jsonl")),
            "default JSON file must not exist when --json-file is set",
        )
        messages = []
        with open(json_path) as fh:
            for line in fh:
                obj = json.loads(line)
                if "message" in obj:
                    messages.append(obj)
        check(len(messages) == sent, f"expected {sent} JSON messages, got {len(messages)}")
        check(messages[0]["message"]["name"] == "chunk-0", messages[0])

        captures = sorted(
            f for f in os.listdir(session_dir) if f.startswith("capture-") and f.endswith(".pcr")
        )
        check(len(captures) >= 2, f"expected multiple time-based chunks, got {captures}")
        for capture in captures:
            inspect = subprocess.run(
                [binary, "inspect", "--rec", os.path.join(session_dir, capture)],
                capture_output=True, text=True, timeout=30,
            )
            check(inspect.returncode == 0, f"inspect failed for {capture}: {inspect.stdout}")
            check("status       : OK" in inspect.stdout, inspect.stdout)

        with open(os.path.join(session_dir, "meta.json")) as fh:
            meta = json.load(fh)
        check(meta["chunk_interval_ms"] == 200, meta)
        check(meta["chunk_bytes"] == 0, meta)
        check(meta["json_file"] == json_path, meta)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def test_config_file_recording(binary: str, proto: str, workdir: str) -> None:
    """Starts a recording entirely from an INI config file."""
    port = pick_port()
    out_dir = os.path.join(workdir, "out-config")
    config_path = os.path.join(workdir, "record.ini")
    stderr_path = os.path.join(workdir, "config.log")
    with open(config_path, "w") as fh:
        fh.write(
            "# recording configuration\n"
            "[record]\n"
            f"listen = 127.0.0.1:{port}\n"
            f"out = {out_dir}\n"
            f"proto = {proto}\n"
            "message_type = demo.TestRecord\n"
            "json = true\n"
            "json_file = config-events.jsonl\n"
            "chunk_interval = 1m\n"
            "fsync_interval = 0\n"
            "write_batch = 4KiB\n"
            "log_level = info\n"
        )

    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [binary, "record", "--config", config_path],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        for i in range(4):
            sock.sendall(frame(enc_record(i, f"config-{i}")))
        sock.close()
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"config recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        with open(os.path.join(session_dir, "meta.json")) as fh:
            meta = json.load(fh)
        check(meta["proto"] == proto, meta)
        check(meta["message_type"] == "demo.TestRecord", meta)
        check(meta["chunk_interval_ms"] == 60000, meta)
        check(meta["json_file"] == os.path.join(session_dir, "config-events.jsonl"), meta)

        json_path = os.path.join(session_dir, "config-events.jsonl")
        messages = []
        with open(json_path) as fh:
            for line in fh:
                obj = json.loads(line)
                if "message" in obj:
                    messages.append(obj)
        check(len(messages) == 4, messages)
        check(messages[3]["message"]["name"] == "config-3", messages)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def test_raw_json(binary: str, proto: str, workdir: str, port: int) -> None:
    """Raw framing has no message boundaries, so JSON must fall back to base64."""
    out_dir = os.path.join(workdir, "out-raw-json")
    stderr_path = os.path.join(workdir, "stderr-raw-json.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--framing", "raw",
            "--proto", proto,
            "--json",
            "--fsync-interval", "0",
            "--write-batch", "4KiB",
            "--log-level", "info",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        payload = bytes(range(256)) * 40
        sock.sendall(payload)
        sock.close()
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"raw json recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        raw_events = []
        with open(os.path.join(session_dir, "events.jsonl")) as fh:
            for line in fh:
                obj = json.loads(line)
                if obj.get("unparsed") and "raw framing" in obj.get("reason", ""):
                    raw_events.append(obj)
        check(bool(raw_events), "no raw framing fallback event")
        reassembled = b"".join(base64.b64decode(event["bytes_b64"]) for event in raw_events)
        check(reassembled == payload, f"raw JSON bytes mismatch: {len(reassembled)} != {len(payload)}")

        with open(os.path.join(session_dir, "json-summary.json")) as fh:
            stats = json.load(fh)
        check(stats["unparsed"] == len(raw_events), stats)
        check(stats["failed"] is False, stats)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def test_raw_without_schema(binary: str, workdir: str, port: int) -> None:
    """Schema-free scenario used by sanitizer runs (protobuf crashes under ASan)."""
    out_dir = os.path.join(workdir, "out-raw")
    stderr_path = os.path.join(workdir, "stderr-raw.log")
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--framing", "raw",
            "--fsync-interval", "0",
            "--write-batch", "4KiB",
            "--log-level", "info",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )
    try:
        sock = wait_for_server(port, time.time() + 10.0)
        payload = b"raw-bytes-" * 4000
        sock.sendall(payload)
        sock.close()
        time.sleep(0.2)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"raw recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        with open(os.path.join(session_dir, "summary.json")) as fh:
            summary = json.load(fh)
        check(summary["clean"] is True, f"summary not clean: {summary}")
        check(summary["data_bytes"] == len(payload), f"data mismatch: {summary}")
        check(summary["records"] >= 2, f"too few records: {summary}")

        inspect = subprocess.run(
            [binary, "inspect", "--dir", session_dir], capture_output=True, text=True, timeout=30
        )
        check(inspect.returncode == 0, f"inspect failed: {inspect.stdout}\n{inspect.stderr}")
        check("status       : OK" in inspect.stdout, inspect.stdout)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()


def main() -> int:
    binary = os.path.abspath(sys.argv[1])
    proto = os.path.abspath(sys.argv[2])
    workdir = tempfile.mkdtemp(prefix="prp-it-")

    if os.environ.get("PRP_SKIP_PROTO_TESTS"):
        try:
            test_raw_without_schema(binary, workdir, pick_port())
            print("integration test passed (schema-free sanitizer path)")
            return 0
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    out_dir = os.path.join(workdir, "out")
    stderr_path = os.path.join(workdir, "stderr.log")
    port = pick_port()

    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(
        [
            binary,
            "record",
            "--listen", f"127.0.0.1:{port}",
            "--out", out_dir,
            "--proto", proto,
            "--message-type", "demo.TestRecord",
            "--json",
            "--fsync-interval", "0",
            "--rotate-size", "4KiB",
            "--max-frame", "4KiB",
            "--write-batch", "4KiB",
            "--log-level", "info",
        ],
        stdout=subprocess.DEVNULL,
        stderr=stderr_file,
    )

    try:
        sock = wait_for_server(port, time.time() + 10.0)
        name = "x" * 200
        for seq in range(50):
            sock.sendall(frame(enc_record(seq, f"{name}-{seq}")))
        # Oversized frame: must be rejected and close the connection.
        sock.sendall(enc_varint(8192) + b"\x00" * 32)
        sock.settimeout(5.0)
        try:
            while sock.recv(4096):
                pass
        except OSError:
            pass
        sock.close()

        time.sleep(0.3)
        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=20)
        check(rc == 0, f"recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        check(len(sessions) == 1, f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])

        for required in ("meta.json", "summary.json", "events.jsonl", "json-summary.json"):
            check(os.path.exists(os.path.join(session_dir, required)), f"missing {required}")

        captures = sorted(
            f for f in os.listdir(session_dir) if f.startswith("capture-") and f.endswith(".pcr")
        )
        check(len(captures) >= 2, f"expected rotation into multiple segments, got {captures}")
        check(all(os.path.getsize(os.path.join(session_dir, f)) > 0 for f in captures), "empty capture")

        with open(os.path.join(session_dir, "summary.json")) as fh:
            summary = json.load(fh)
        check(summary["clean"] is True, f"summary not clean: {summary}")
        check(summary["data_bytes"] > 50 * 200, "not all message bytes recorded")
        check(summary["records"] >= 10, "too few records")
        check(summary["dropped_items"] == 0, "items were dropped")

        messages = []
        events = 0
        with open(os.path.join(session_dir, "events.jsonl")) as fh:
            for line in fh:
                obj = json.loads(line)
                events += 1
                if "message" in obj:
                    messages.append(obj)
        check(len(messages) == 50, f"expected 50 JSON messages, got {len(messages)}")
        types = {m["type"] for m in messages}
        check(types == {"demo.TestRecord"}, f"unexpected types: {types}")
        seqs = {str(m["message"].get("seq", 0)) for m in messages}
        check(seqs == {str(i) for i in range(50)}, "message sequence mismatch")
        check(any(m["message"]["name"].endswith("-49") for m in messages), "payload mismatch")
        reasons = []
        with open(os.path.join(session_dir, "events.jsonl")) as fh:
            for line in fh:
                obj = json.loads(line)
                if obj.get("event") == "stream_close":
                    reasons.append(obj.get("reason"))
        check("protocol_error" in reasons, f"oversized frame not rejected: {reasons}")

        inspect = subprocess.run(
            [binary, "inspect", "--dir", session_dir], capture_output=True, text=True, timeout=30
        )
        check(inspect.returncode == 0, f"inspect failed: {inspect.stdout}\n{inspect.stderr}")
        check(inspect.stdout.count("status       : OK") == len(captures), inspect.stdout)
        check("trailer      : clean" in inspect.stdout, inspect.stdout)

        # Crash-recovery path: append a partial record to the last segment.
        victim = os.path.join(session_dir, captures[-1])
        original_size = os.path.getsize(victim)
        with open(victim, "ab") as fh:
            fh.write(b"\x02\x40\x00\x00\x00\x01\x02\x03")
        damaged = subprocess.run(
            [binary, "inspect", "--dir", session_dir], capture_output=True, text=True, timeout=30
        )
        check(damaged.returncode == 1, "damaged capture not detected")
        check("DAMAGED" in damaged.stdout, damaged.stdout)
        check("use --truncate to repair" in damaged.stdout, damaged.stdout)

        repaired = subprocess.run(
            [binary, "inspect", "--dir", session_dir, "--truncate"],
            capture_output=True, text=True, timeout=30,
        )
        check(repaired.returncode == 0, f"truncate failed: {repaired.stdout}\n{repaired.stderr}")
        check(os.path.getsize(victim) == original_size, "truncate did not restore file size")
        final = subprocess.run(
            [binary, "inspect", "--dir", session_dir], capture_output=True, text=True, timeout=30
        )
        check(final.returncode == 0, f"post-repair inspect failed: {final.stdout}")

        test_raw_json(binary, proto, workdir, pick_port())
        test_outbound(binary, proto, workdir, pick_port())
        test_chunking_and_json_file(binary, proto, workdir)
        test_config_file_recording(binary, proto, workdir)

        print(f"integration test passed ({events} events, {len(captures)} segments, outbound verified)")
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()
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

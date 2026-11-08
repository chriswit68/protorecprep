#!/usr/bin/env python3
"""Lightweight end-to-end throughput benchmark for the recording mode.

Sends a continuous stream of length-prefixed `demo.TestRecord` messages over
loopback to the real binary and measures how many wire bytes make it through
the capture pipeline until the recorder has flushed and exited.

Two scenarios are compared by default: binary capture only, and binary capture
plus the JSONL sidecar, which gives the JSON overhead.

Example:
    python3 bench/benchmark.py --binary build/default/protorecprep \
        --proto proto/sample.proto --seconds 5 --message-size 32768
"""

from __future__ import annotations

import argparse
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


def build_stream(message_size: int, batch_messages: int) -> bytes:
    """Pre-frames `batch_messages` valid demo.TestRecord messages."""
    # name = field 2, wire type 2. Pad to roughly message_size bytes.
    payload_size = max(1, message_size)
    name_len = max(0, payload_size - 1 - len(enc_varint(payload_size)))
    name_len = max(0, payload_size - 1 - len(enc_varint(name_len)))
    name = b"x" * name_len
    payload = b"\x12" + enc_varint(len(name)) + name
    frame = enc_varint(len(payload)) + payload
    return frame * batch_messages


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_server(port: int, deadline: float) -> socket.socket:
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            sock.settimeout(5.0)
            return sock
        except OSError as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.02)
    raise RuntimeError(f"server did not start: {last_error}")


def run_once(args: argparse.Namespace, with_json: bool) -> dict:
    workdir = tempfile.mkdtemp(prefix="prp-bench-")
    out_dir = os.path.join(args.out or workdir, "out-json" if with_json else "out-bin")
    os.makedirs(out_dir, exist_ok=True)
    stderr_path = os.path.join(workdir, "stderr.log")
    port = pick_port()

    command = [
        args.binary,
        "record",
        "--listen", f"127.0.0.1:{port}",
        "--out", out_dir,
        "--proto", args.proto,
        "--message-type", "demo.TestRecord",
        "--fsync-interval", "0",
        "--rotate-size", args.rotate_size,
        "--queue-high-water", args.queue_high_water,
        "--recv-buffer", args.recv_buffer,
        "--write-batch", args.write_batch,
        "--max-frame", str(max(args.message_size * 2, 4096)),
        "--log-level", "warn",
    ]
    if with_json:
        command.append("--json")

    stream = build_stream(args.message_size, args.batch_messages)
    stderr_file = open(stderr_path, "wb")  # noqa: SIM115
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=stderr_file)

    try:
        sock = wait_for_server(port, time.time() + 10.0)
        sock.settimeout(5.0)
        deadline = time.perf_counter() + args.seconds
        sent = 0
        started = time.perf_counter()
        while time.perf_counter() < deadline:
            sock.sendall(stream)
            sent += len(stream)
            if args.target_mbps > 0:
                expected = sent / (args.target_mbps * 1e6)
                lag = expected - (time.perf_counter() - started)
                if lag > 0:
                    time.sleep(min(lag, 0.1))
        send_elapsed = time.perf_counter() - started
        sock.close()

        proc.send_signal(signal.SIGINT)
        rc = proc.wait(timeout=max(30.0, args.seconds * 4))
        total_elapsed = time.perf_counter() - started
        if rc != 0:
            raise RuntimeError(f"recorder exited with {rc}\n{open(stderr_path).read()}")

        sessions = [d for d in os.listdir(out_dir) if d.startswith("session-")]
        if len(sessions) != 1:
            raise RuntimeError(f"expected one session dir, got {sessions}")
        session_dir = os.path.join(out_dir, sessions[0])
        with open(os.path.join(session_dir, "summary.json")) as fh:
            summary = json.load(fh)
        if not summary["clean"]:
            raise RuntimeError(f"unclean recording: {summary}")
        if summary["dropped_items"]:
            raise RuntimeError(f"capture queue dropped items: {summary}")

        result = {
            "json": with_json,
            "wire_bytes": sent,
            "data_bytes": summary["data_bytes"],
            "records": summary["records"],
            "send_seconds": send_elapsed,
            "total_seconds": total_elapsed,
            "send_mbps": sent / send_elapsed / 1e6,
            "total_mbps": sent / total_elapsed / 1e6,
            "session_dir": session_dir,
        }
        if with_json:
            with open(os.path.join(session_dir, "json-summary.json")) as fh:
                result["json_stats"] = json.load(fh)
        return result
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        stderr_file.close()
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/default/protorecprep")
    parser.add_argument("--proto", default="proto/sample.proto")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument(
        "--target-mbps",
        type=float,
        default=0.0,
        help="throttle to this wire rate in MB/s (0 = as fast as possible)",
    )
    parser.add_argument("--message-size", type=int, default=32768)
    parser.add_argument("--batch-messages", type=int, default=64)
    parser.add_argument("--queue-high-water", default="256MiB")
    parser.add_argument("--recv-buffer", default="256KiB")
    parser.add_argument("--write-batch", default="4MiB")
    parser.add_argument("--rotate-size", default="0")
    parser.add_argument("--out", help="benchmark output directory (default: temp)")
    parser.add_argument("--keep", action="store_true", help="keep temp artifacts")
    parser.add_argument("--bin-only", action="store_true", help="skip the JSON comparison")
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        print(f"binary not found: {args.binary}", file=sys.stderr)
        return 2
    if not os.path.exists(args.proto):
        print(f"proto not found: {args.proto}", file=sys.stderr)
        return 2

    results = [run_once(args, with_json=False)]
    if not args.bin_only:
        results.append(run_once(args, with_json=True))

    print(f"message size : {args.message_size} bytes")
    print(f"duration     : {args.seconds:.1f} s per scenario")
    print()
    print(f"{'scenario':<14}{'wire':>10}{'capture':>12}{'end-to-end':>14}{'records':>10}")
    for result in results:
        label = "binary+json" if result["json"] else "binary only"
        print(
            f"{label:<14}"
            f"{result['wire_bytes'] / 1e6:>9.1f}MB"
            f"{result['send_mbps']:>10.1f}MB/s"
            f"{result['total_mbps']:>11.1f}MB/s"
            f"{result['records']:>10}"
        )
        if result["json"]:
            stats = result.get("json_stats", {})
            print(
                f"{'  json sidecar':<14}"
                f"events={stats.get('events', 0)} messages={stats.get('messages', 0)} "
                f"unparsed={stats.get('unparsed', 0)} dropped={stats.get('dropped_items', 0)} "
                f"desynced={stats.get('desynced_streams', 0)}"
            )

    if len(results) == 2:
        stats = results[1].get("json_stats", {})
        dropped = stats.get("dropped_items", 0)
        desynced = stats.get("desynced_streams", 0)
        print()
        if dropped or desynced:
            # Once the sidecar sheds load it stops decoding the affected streams,
            # so a throughput comparison at this rate would be meaningless.
            print(
                "JSON overhead: not measured at this rate "
                f"({dropped} items dropped, {desynced} streams desynchronized)"
            )
            print("use --target-mbps to benchmark a sustainable rate")
        else:
            base_send = results[0]["send_mbps"]
            json_send = results[1]["send_mbps"]
            base_total = results[0]["total_mbps"]
            json_total = results[1]["total_mbps"]
            send_overhead = (base_send - json_send) / base_send * 100.0 if base_send > 0 else 0.0
            total_overhead = (base_total - json_total) / base_total * 100.0 if base_total > 0 else 0.0
            print(
                f"JSON overhead: {send_overhead:.1f}% send-rate, "
                f"{total_overhead:.1f}% end-to-end (target < 5% at 500 MB/s)"
            )

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except RuntimeError as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        sys.exit(1)

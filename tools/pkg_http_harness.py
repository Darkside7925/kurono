#!/usr/bin/env python3
"""
Host-side harness for the `kpkg sync` HTTP fetch path.

What it exercises:
- repository host/IP candidate selection
- raw TCP connect on port 80
- the same HTTP/1.1 GET shape used by `src/packages/pkgmgr.cpp`
- chunked or content-length response parsing
- optional JSON validation of `/packages/index.json`

What it does not exercise:
- guest E1000 detection/link state
- guest ARP/IPv4/TCP state machines
- scheduler-driven kernel socket polling

Use this to separate "the package repository is reachable" from
"the guest network stack can reach it on this boot".
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import sys
import time
from typing import Dict, Iterable, List, Sequence, Tuple

DEFAULT_HOST = "kurono.satorut.com"
DEFAULT_PATH = "/packages/index.json"
DEFAULT_PORT = 80
DEFAULT_TIMEOUT = 5.0
DEFAULT_MAX_BYTES = 16 * 1024 * 1024
USER_AGENT = "Kurono-kpkg/1.0"

# Mirrors the explicit fallbacks in `src/packages/pkgmgr.cpp`.
FALLBACK_IPS = (
    "104.21.44.39",
    "172.67.194.167",
)


def unique(values: Iterable[str]) -> List[str]:
    seen = set()
    out: List[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def build_candidates(host: str, explicit_ip: str | None) -> List[str]:
    if explicit_ip:
        return [explicit_ip]

    resolved: List[str] = []
    try:
        infos = socket.getaddrinfo(
            host,
            DEFAULT_PORT,
            family=socket.AF_INET,
            type=socket.SOCK_STREAM,
        )
        for info in infos:
            resolved.append(info[4][0])
    except socket.gaierror as exc:
        print(f"[warn] DNS resolution for {host} failed: {exc}", file=sys.stderr)

    return unique([*resolved, *FALLBACK_IPS])


def build_request(host: str, path: str) -> bytes:
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"User-Agent: {USER_AGENT}\r\n"
        "Connection: close\r\n"
        "Accept: */*\r\n"
        "\r\n"
    )
    return request.encode("ascii")


def recv_all(sock: socket.socket, max_bytes: int, timeout: float) -> bytes:
    chunks: List[bytes] = []
    total = 0
    sock.settimeout(timeout)
    while total < max_bytes:
        try:
            chunk = sock.recv(min(65536, max_bytes - total))
        except socket.timeout:
            break
        if not chunk:
            break
        chunks.append(chunk)
        total += len(chunk)
    return b"".join(chunks)


def decode_chunked(body: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(body):
        while body[pos:pos + 2] == b"\r\n":
            pos += 2
        line_end = body.find(b"\r\n", pos)
        if line_end < 0:
            raise ValueError("chunked body was missing a size line terminator")
        size_text = body[pos:line_end].split(b";", 1)[0].strip()
        if not size_text:
            raise ValueError("chunked body had an empty size line")
        size = int(size_text, 16)
        pos = line_end + 2
        if size == 0:
            return bytes(out)
        end = pos + size
        if end > len(body):
            raise ValueError("chunked body ended before the chunk payload")
        out.extend(body[pos:end])
        pos = end
        if body[pos:pos + 2] != b"\r\n":
            raise ValueError("chunked body was missing the chunk trailer")
        pos += 2
    raise ValueError("chunked body never reached the terminating 0-size chunk")


def parse_http_response(raw: bytes) -> Tuple[int, Dict[str, List[str]], bytes]:
    header_end = raw.find(b"\r\n\r\n")
    if header_end < 0:
        raise ValueError("HTTP response was missing the header terminator")

    header_blob = raw[:header_end].decode("iso-8859-1")
    lines = header_blob.split("\r\n")
    if not lines:
        raise ValueError("HTTP response had an empty status line")

    status_parts = lines[0].split()
    if len(status_parts) < 2 or not status_parts[1].isdigit():
        raise ValueError(f"could not parse HTTP status line: {lines[0]!r}")
    status = int(status_parts[1])

    headers: Dict[str, List[str]] = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        headers.setdefault(key.strip().lower(), []).append(value.strip())

    body = raw[header_end + 4:]
    transfer_encodings = ",".join(headers.get("transfer-encoding", [])).lower()
    if "chunked" in transfer_encodings:
        body = decode_chunked(body)
    elif "content-length" in headers:
        content_length = int(headers["content-length"][0])
        body = body[:content_length]

    return status, headers, body


def fetch_once(
    host: str,
    path: str,
    ip: str,
    timeout: float,
    max_bytes: int,
    verbose: bool,
) -> Tuple[int, Dict[str, List[str]], bytes, float]:
    request = build_request(host, path)
    start = time.perf_counter()
    with socket.create_connection((ip, DEFAULT_PORT), timeout=timeout) as sock:
        if verbose:
            print(f"[info] connected to {ip}:{DEFAULT_PORT}")
        sock.sendall(request)
        raw = recv_all(sock, max_bytes=max_bytes, timeout=timeout)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    status, headers, body = parse_http_response(raw)
    return status, headers, body, elapsed_ms


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Exercise the kpkg repository HTTP fetch path from the host."
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="Repository host header.")
    parser.add_argument("--path", default=DEFAULT_PATH, help="Repository path to fetch.")
    parser.add_argument("--ip", help="Specific IPv4 address to test instead of trying all candidates.")
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="Per-connect and per-recv timeout in seconds.",
    )
    parser.add_argument(
        "--max-bytes",
        type=int,
        default=DEFAULT_MAX_BYTES,
        help="Maximum response size to buffer.",
    )
    parser.add_argument(
        "--no-json-check",
        action="store_true",
        help="Skip JSON parsing validation of the fetched body.",
    )
    parser.add_argument(
        "--preview-bytes",
        type=int,
        default=160,
        help="Number of body bytes to print as a preview.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print candidate attempts and low-level details.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    candidates = build_candidates(args.host, args.ip)
    if not candidates:
        print("[error] no candidate IPv4 addresses were available", file=sys.stderr)
        return 2

    print(f"Host: {args.host}")
    print(f"Path: {args.path}")
    print(f"Candidates: {', '.join(candidates)}")

    errors: List[str] = []
    for ip in candidates:
        if args.verbose:
            print(f"[info] trying {ip}:{DEFAULT_PORT}")
        try:
            status, headers, body, elapsed_ms = fetch_once(
                args.host,
                args.path,
                ip,
                timeout=args.timeout,
                max_bytes=args.max_bytes,
                verbose=args.verbose,
            )
            print(f"Connected IP: {ip}")
            print(f"HTTP status: {status}")
            print(f"Elapsed ms: {elapsed_ms:.1f}")
            print(f"Body bytes: {len(body)}")
            print(f"Body sha256: {hashlib.sha256(body).hexdigest()}")
            content_type = ", ".join(headers.get("content-type", [])) or "<none>"
            print(f"Content-Type: {content_type}")

            if not args.no_json_check:
                parsed = json.loads(body.decode("utf-8"))
                if isinstance(parsed, dict):
                    summary = f"dict keys={len(parsed)}"
                elif isinstance(parsed, list):
                    summary = f"list items={len(parsed)}"
                else:
                    summary = type(parsed).__name__
                print(f"JSON parse: ok ({summary})")

            preview = body[: max(args.preview_bytes, 0)]
            if preview:
                print("Body preview:")
                print(preview.decode("utf-8", errors="replace"))

            return 0 if status == 200 else 1
        except Exception as exc:  # noqa: BLE001 - keep harness dependency-free.
            errors.append(f"{ip}: {exc}")
            if args.verbose:
                print(f"[warn] {ip} failed: {exc}", file=sys.stderr)

    print("[error] all candidate IPs failed", file=sys.stderr)
    for error in errors:
        print(f"  - {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Minimal RFB/VNC client that completes the handshake with Bochs RFB server.
Just connects, does protocol version + security handshake, then idles.
This lets Bochs proceed past its 'waiting for VNC client' gate."""

import socket
import struct
import sys
import time

def connect_rfb(host="127.0.0.1", port=5900, idle_secs=60):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((host, port))
    except Exception as e:
        print(f"Cannot connect to {host}:{port}: {e}", file=sys.stderr)
        return

    # 1. Read server protocol version (e.g. "RFB 003.008\n")
    ver = sock.recv(12)
    print(f"Server version: {ver}", file=sys.stderr)

    # 2. Reply with same version
    sock.sendall(b"RFB 003.008\n")

    # 3. Read security types
    data = sock.recv(256)
    if len(data) > 0:
        num_types = data[0]
        print(f"Security types: {num_types} -> {list(data[1:1+num_types])}", file=sys.stderr)
        # Select type 1 (None = no authentication)
        sock.sendall(bytes([1]))

    # 4. Read security result (should be 0 = OK)
    result = sock.recv(4)
    if len(result) >= 4:
        code = struct.unpack(">I", result[:4])[0]
        print(f"Security result: {code}", file=sys.stderr)

    # 5. Send ClientInit (shared flag = 1)
    sock.sendall(bytes([1]))

    # 6. Read ServerInit (framebuffer dimensions, pixel format, name)
    server_init = sock.recv(4096)
    if len(server_init) >= 24:
        w, h = struct.unpack(">HH", server_init[:4])
        print(f"Framebuffer: {w}x{h}", file=sys.stderr)

    # 7. Idle - keep connection alive until killed
    print(f"Connected. Idling for {idle_secs}s...", file=sys.stderr)
    sock.settimeout(1)
    start = time.time()
    while time.time() - start < idle_secs:
        try:
            d = sock.recv(4096)
            if not d:
                break
        except socket.timeout:
            pass
        except Exception:
            break

    sock.close()
    print("VNC client disconnected.", file=sys.stderr)

if __name__ == "__main__":
    idle = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    connect_rfb(idle_secs=idle)

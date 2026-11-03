#!/usr/bin/env python3
"""Run commands on the QNX VM over its serial console.

    scripts/qnx-serial.py [--port 5555] [--user root] [--timeout 60] -- 'cmd' ['cmd' ...]

Logs in if a login prompt is showing, runs each command, prints its output
and exits with the last command's exit status. Each command's output is
delimited with a unique marker so prompts and echoes are not confused with
output.
"""
import argparse
import re
import socket
import sys
import time
import uuid


def read_until(sock, pattern, timeout):
    buf = b""
    end = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < end:
        sock.settimeout(max(0.1, end - time.time()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        m = rx.search(buf)
        if m:
            return buf, m
    return buf, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="root")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()

    s = socket.create_connection(("127.0.0.1", a.port), timeout=10)
    s.sendall(b"\r")
    buf, m = read_until(s, rb"(login: ?$|[#$] ?$|Password: ?$)", 20)
    if m and b"login" in m.group(0):
        s.sendall(a.user.encode() + b"\r")
        buf, m = read_until(s, rb"(Password: ?$|[#$] ?$)", 10)
        if m and b"Password" in m.group(0):
            s.sendall(a.password.encode() + b"\r")
            read_until(s, rb"[#$] ?$", 10)
    rc = 0
    for cmd in a.cmds:
        tag = uuid.uuid4().hex[:12]
        line = f"echo BEGIN{tag}; {cmd}; echo END{tag} $?\r"
        s.sendall(line.encode())
        buf, m = read_until(s, (rf"END{tag} (\d+)").encode(), a.timeout)
        if not m:
            sys.stdout.write(buf.decode(errors="replace"))
            print(f"\n[timeout after {a.timeout}s: {cmd}]", file=sys.stderr)
            return 124
        text = buf.decode(errors="replace")
        start = text.rfind(f"BEGIN{tag}")
        out = text[start + len("BEGIN") + len(tag):text.rfind(f"END{tag}")]
        sys.stdout.write(out.lstrip("\r\n"))
        rc = int(m.group(1))
    return rc


if __name__ == "__main__":
    sys.exit(main())

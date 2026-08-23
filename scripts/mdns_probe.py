#!/usr/bin/env python3
"""Probe a device's mDNS responder directly, bypassing the host resolver.

`ping smolbase-XXXX.local` failing proves nothing about the firmware: it could
equally be the host, and on this project it was blamed on the host for weeks.
This asks the DEVICE — send a standard mDNS query to 224.0.0.251:5353 and see
whether it answers, and with what.

Two receive strategies, because either can be blocked independently:
  1. QU (unicast-response) bit set, listening on an ephemeral port. Cheapest,
     and no conflict with a local mDNS service already holding 5353.
  2. Join the multicast group on 5353 with SO_REUSEADDR and read the normal
     multicast answer.

Answers from other hosts on the LAN show up too (printers are enthusiastic
responders); the source IP column is how you tell them apart.

Usage:
    uv run scripts/mdns_probe.py <name> [expected-ip]
    uv run scripts/mdns_probe.py smolbase-2e00 10.0.0.32

A healthy device answers three records: an A for <name>.local, a PTR for
_http._tcp.local, and an SRV pointing at <name>.local:80.
"""
import socket
import struct
import sys
import time

MCAST = "224.0.0.251"
PORT = 5353


def build_query(labels: list[bytes], qtype: int, unicast: bool) -> bytes:
    # id=0 (mDNS ignores it), flags=0 (standard query), 1 question
    pkt = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    for l in labels:
        pkt += bytes([len(l)]) + l
    pkt += b"\x00"
    qclass = 0x8001 if unicast else 0x0001  # top bit = QU, "please answer unicast"
    pkt += struct.pack("!HH", qtype, qclass)
    return pkt


def parse_name(buf: bytes, off: int) -> tuple[str, int]:
    parts, jumped, ret = [], False, off
    while True:
        if off >= len(buf):
            break
        n = buf[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:  # compression pointer
            ptr = struct.unpack("!H", buf[off:off + 2])[0] & 0x3FFF
            if not jumped:
                ret = off + 2
                jumped = True
            off = ptr
            continue
        parts.append(buf[off + 1:off + 1 + n].decode("utf-8", "replace"))
        off += 1 + n
    return ".".join(parts), (ret if jumped else off)


def decode(buf: bytes) -> list[str]:
    """Return human-readable answer records."""
    if len(buf) < 12:
        return []
    _, _, qd, an, ns, ar = struct.unpack("!HHHHHH", buf[:12])
    off = 12
    for _ in range(qd):
        _, off = parse_name(buf, off)
        off += 4
    out = []
    for _ in range(an + ns + ar):
        name, off = parse_name(buf, off)
        if off + 10 > len(buf):
            break
        rtype, _, _, rdlen = struct.unpack("!HHIH", buf[off:off + 10])
        off += 10
        rdata = buf[off:off + rdlen]
        off += rdlen
        if rtype == 1 and rdlen == 4:
            out.append(f"A     {name} -> {socket.inet_ntoa(rdata)}")
        elif rtype == 12:
            tgt, _ = parse_name(buf, off - rdlen)
            out.append(f"PTR   {name} -> {tgt}")
        elif rtype == 33 and rdlen >= 6:
            port = struct.unpack("!HHH", rdata[:6])[2]
            tgt, _ = parse_name(buf, off - rdlen + 6)
            out.append(f"SRV   {name} -> {tgt}:{port}")
        elif rtype == 16:
            out.append(f"TXT   {name} -> {rdata[1:].decode('utf-8', 'replace')!r}")
        else:
            out.append(f"type{rtype} {name} ({rdlen} B)")
    return out


def probe_unicast(queries, timeout=4.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    s.settimeout(0.5)
    got = []
    for labels, qtype, _ in queries:
        s.sendto(build_query(labels, qtype, unicast=True), (MCAST, PORT))
    end = time.time() + timeout
    while time.time() < end:
        try:
            buf, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        for r in decode(buf):
            got.append((addr[0], r))
    s.close()
    return got


def probe_multicast(queries, timeout=4.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", PORT))
    except OSError as e:
        return [("-", f"could not bind {PORT}: {e}")]
    # Windows refuses IP_ADD_MEMBERSHIP with INADDR_ANY as the interface
    # (WinError 10065), so name the interface explicitly.
    local = socket.gethostbyname(socket.gethostname())
    mreq = struct.pack("4s4s", socket.inet_aton(MCAST), socket.inet_aton(local))
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    except OSError as e:
        return [("-", f"could not join {MCAST} on {local}: {e}")]
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    s.settimeout(0.5)
    for labels, qtype, _ in queries:
        s.sendto(build_query(labels, qtype, unicast=False), (MCAST, PORT))
    got = []
    end = time.time() + timeout
    while time.time() < end:
        try:
            buf, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        for r in decode(buf):
            got.append((addr[0], r))
    s.close()
    return got


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "smolbase-2e00"
    expect = sys.argv[2] if len(sys.argv) > 2 else None
    queries = [
        ([name.encode(), b"local"], 1, "A"),
        ([b"_http", b"_tcp", b"local"], 12, "PTR _http._tcp"),
    ]
    for how, fn in (("unicast (QU bit)", probe_unicast), ("multicast :5353", probe_multicast)):
        print(f"--- {how} ---")
        rows = fn(queries)
        if not rows:
            print("  (no answers)")
        for src, r in rows:
            mark = ""
            if expect and expect in r:
                mark = "  <== expected IP"
            print(f"  from {src:<15} {r}{mark}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

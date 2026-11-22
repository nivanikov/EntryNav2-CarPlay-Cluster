#!/usr/bin/env python3
# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Summarise a pcap without libpcap/Wireshark: packet count, duration, and a flow histogram, plus a SOME/IP peek.

  python pcapstat.py <file.pcap> [--someip]

A quick first look at a stocknav-*.pcap capture: which hosts/ports talk, how many packets, and (for SOME/IP, the
default automotive service protocol) the service/method ids in the first bytes of each UDP/TCP payload.
"""
import struct
import sys
from collections import Counter


def ip4(b):
    return "%d.%d.%d.%d" % (b[0], b[1], b[2], b[3])


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    path = sys.argv[1]
    want_someip = "--someip" in sys.argv[2:]
    data = open(path, "rb").read()
    if len(data) < 24 or data[:4] not in (b"\xa1\xb2\xc3\xd4", b"\xd4\xc3\xb2\xa1"):
        print("not a pcap (magic %r)" % data[:4]); return
    le = data[:4] == b"\xd4\xc3\xb2\xa1"
    u32 = (lambda o: struct.unpack_from("<I" if le else ">I", data, o)[0])
    off = 24
    n = 0
    t0 = t1 = None
    flows = Counter()
    bytes_by_flow = Counter()
    someip = Counter()
    while off + 16 <= len(data):
        ts, us, incl, orig = u32(off), u32(off + 4), u32(off + 8), u32(off + 12)
        off += 16
        if off + incl > len(data):
            break
        pkt = data[off:off + incl]
        off += incl
        n += 1
        t0 = ts if t0 is None else t0
        t1 = ts
        if len(pkt) < 14:
            continue
        et = (pkt[12] << 8) | pkt[13]
        ipoff = 14
        if et == 0x8100 and len(pkt) >= 18:
            et = (pkt[16] << 8) | pkt[17]; ipoff = 18
        if et != 0x0800 or len(pkt) < ipoff + 20:
            flows[("non-ip", "ethertype=0x%04x" % et, "")] += 1
            continue
        ihl = (pkt[ipoff] & 0x0f) * 4
        proto = pkt[ipoff + 9]
        src = ip4(pkt[ipoff + 12:ipoff + 16]); dst = ip4(pkt[ipoff + 16:ipoff + 20])
        l4 = ipoff + ihl
        sport = dport = 0
        pname = {6: "tcp", 17: "udp"}.get(proto, "ip%d" % proto)
        payoff = l4
        if proto in (6, 17) and len(pkt) >= l4 + 4:
            sport = (pkt[l4] << 8) | pkt[l4 + 1]
            dport = (pkt[l4 + 2] << 8) | pkt[l4 + 3]
            payoff = l4 + (20 if proto == 6 else 8)
        key = (pname, "%s:%d" % (src, sport), "%s:%d" % (dst, dport))
        flows[key] += 1
        bytes_by_flow[key] += orig
        if want_someip and proto in (6, 17) and len(pkt) >= payoff + 8:
            svc = (pkt[payoff] << 8) | pkt[payoff + 1]
            meth = (pkt[payoff + 2] << 8) | pkt[payoff + 3]
            someip[(dport, "svc=0x%04x" % svc, "meth=0x%04x" % meth)] += 1

    dur = (t1 - t0) if (t0 is not None and t1 is not None) else 0
    print("file: %s" % path)
    print("packets: %d   duration: %d s   total bytes: %d" % (n, dur, len(data)))
    print("\n=== flows (count, bytes) ===")
    for key, c in flows.most_common(25):
        print("  %5d  %8d B  %-4s %-22s -> %-22s" % (c, bytes_by_flow.get(key, 0), key[0], key[1], key[2]))
    if want_someip and someip:
        print("\n=== SOME/IP service/method (by dst port) ===")
        for key, c in someip.most_common(25):
            print("  %5d  dport=%-6d %s %s" % (c, key[0], key[1], key[2]))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Decode the SOME/IP cluster-guidance messages from a stocknav pcap (what the factory nav sends to draw on DKOMBI2).

Reassembles the TCP stream to/from the cluster guidance service and parses SOME/IP messages (service/method/type +
payload). The payload carries the graphic "models" (maneuver, lane, description, distance, state) -- the same shapes
carplay-kombi builds in kombi_graph.cpp -- so this is how we learn the real lane/junction and secondary-maneuver format.

  python someip_decode.py <file.pcap> [host:port] [--to-only] [--max N] [--payload]
    host:port  cluster guidance endpoint (default 160.48.199.96:30502)
    --to-only  only messages sent TO the cluster (nav -> cluster: the render requests)
    --payload  dump full payload hex (default: first 48 bytes + the model-type byte)
"""
import struct
import sys
from collections import defaultdict

MODEL = {3: "TEXT", 15: "DESCRIPTION", 17: "DESTINATION", 27: "LANE", 29: "DISTANCE", 33: "STATE"}


def ip4(b):
    return "%d.%d.%d.%d" % (b[0], b[1], b[2], b[3])


def read_pcap(path):
    data = open(path, "rb").read()
    le = data[:4] == b"\xd4\xc3\xb2\xa1"
    u32 = lambda o: struct.unpack_from("<I" if le else ">I", data, o)[0]
    off, pkts = 24, []
    while off + 16 <= len(data):
        incl = u32(off + 8)
        off += 16
        pkts.append(data[off:off + incl])
        off += incl
    return pkts


def tcp_of(pkt):
    if len(pkt) < 14:
        return None
    et = (pkt[12] << 8) | pkt[13]; ipoff = 14
    if et == 0x8100 and len(pkt) >= 18:
        et = (pkt[16] << 8) | pkt[17]; ipoff = 18
    if et != 0x0800 or len(pkt) < ipoff + 20:
        return None
    ihl = (pkt[ipoff] & 0x0f) * 4
    if pkt[ipoff + 9] != 6:
        return None
    src, dst = ip4(pkt[ipoff + 12:ipoff + 16]), ip4(pkt[ipoff + 16:ipoff + 20])
    l4 = ipoff + ihl
    if len(pkt) < l4 + 20:
        return None
    sport = (pkt[l4] << 8) | pkt[l4 + 1]; dport = (pkt[l4 + 2] << 8) | pkt[l4 + 3]
    seq = struct.unpack_from(">I", pkt, l4 + 4)[0]
    doff = (pkt[l4 + 12] >> 4) * 4
    payload = pkt[l4 + doff:]
    return (src, sport, dst, dport, seq, payload)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    path = sys.argv[1]
    hostport = next((a for a in sys.argv[2:] if ":" in a and not a.startswith("--")), "160.48.199.96:30502")
    chost, cport = hostport.split(":"); cport = int(cport)
    to_only = "--to-only" in sys.argv
    full = "--payload" in sys.argv
    maxn = next((int(sys.argv[i + 1]) for i, a in enumerate(sys.argv) if a == "--max"), 60)

    # reassemble per TCP flow (by seq)
    streams = defaultdict(dict)  # flow -> {seq: payload}
    for pkt in read_pcap(path):
        t = tcp_of(pkt)
        if not t:
            continue
        src, sport, dst, dport, seq, payload = t
        if not payload:
            continue
        if (dst == chost and dport == cport) or (src == chost and sport == cport):
            streams[(src, sport, dst, dport)][seq] = payload

    print("file: %s   cluster %s:%d   flows: %d" % (path, chost, cport, len(streams)))
    shown = 0
    for flow, segs in streams.items():
        src, sport, dst, dport = flow
        to_cluster = (dst == chost and dport == cport)
        if to_only and not to_cluster:
            continue
        buf = b"".join(segs[s] for s in sorted(segs))
        o = 0
        print("\n--- %s:%d -> %s:%d (%s, %d bytes) ---" % (src, sport, dst, dport,
              "TO cluster" if to_cluster else "from cluster", len(buf)))
        while o + 16 <= len(buf):
            svc, meth = struct.unpack_from(">HH", buf, o)
            length = struct.unpack_from(">I", buf, o + 4)[0]
            if length < 8 or o + 8 + length > len(buf) or length > 0x4000:
                o += 1; continue  # resync
            mtype, rc = buf[o + 14], buf[o + 15]
            payload = buf[o + 16:o + 8 + length]
            o += 8 + length
            mt = payload[8] if len(payload) > 8 else -1
            tag = MODEL.get(mt, "")
            head = payload[:48 if not full else len(payload)].hex()
            print("  svc=0x%04x meth=0x%04x mtype=0x%02x len=%d model=%s[%d] %s"
                  % (svc, meth, mtype, len(payload), tag, mt, head))
            shown += 1
            if shown >= maxn:
                print("  ... (--max %d reached)" % maxn); return


if __name__ == "__main__":
    main()

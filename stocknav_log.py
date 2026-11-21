#!/usr/bin/env python3
# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Capture what the FACTORY navigation sends to the instrument cluster (DKOMBI2), for offline analysis.

Cluster guidance travels over SOME/IP on the head unit's internal network; the guidance server lives on the cluster
host (default 160.48.199.96, the same one carplay-kombi talks to). This sniffs the traffic to/from that host into a
pcap you open in Wireshark (it has a SOME/IP dissector) to see the stock guidance PDUs -- the groundwork for drawing
the stock-style arrow/map from CarPlay later.

Capture it with the FACTORY nav guiding and NO CarPlay route active (so only the stock source is talking to the
cluster). Needs tcpdump on the HU; --probe tells you whether it is there.

  python stocknav_log.py --probe          # interfaces, tcpdump presence, a 5 s peek at cluster traffic
  python stocknav_log.py --start          # start a background capture on the HU, print the PID
  python stocknav_log.py --stop           # stop it and pull the pcap here (stocknav-<ts>.pcap)

Env: HU_HOST (ENET addr), HU_PW/.secrets (password), CP_KOMBI_ADDR (cluster host, default 160.48.199.96).
"""
import os
import sys
import time

import paramiko

from huauth import USER, hu_password

HOST = os.environ.get("HU_HOST", "169.254.1.0")
CLUSTER = os.environ.get("CP_KOMBI_ADDR", "160.48.199.96")
REMOTE_PCAP = "/var/local/stocknav.pcap"
REMOTE_PID = "/var/local/stocknav.tcpdump.pid"
TCPDUMP_CANDIDATES = ["tcpdump", "/usr/sbin/tcpdump", "/usr/bin/tcpdump", "/sbin/tcpdump", "/opt/app/usr/bin/tcpdump"]


def asc(s):
    return "".join(ch if ord(ch) < 128 else "?" for ch in str(s))


def connect():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, 22, USER, hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=14)
    return c


def run(c, cmd, t=60):
    i, o, e = c.exec_command(cmd, timeout=t)
    return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")


def find_tcpdump(c):
    for cand in TCPDUMP_CANDIDATES:
        out = run(c, "command -v %s 2>/dev/null || ([ -x %s ] && echo %s)" % (cand, cand, cand)).strip()
        if out:
            return out.splitlines()[0].strip()
    return ""


def iface_for_cluster(c):
    # the interface whose subnet matches the cluster (160.48.199.x by default)
    net = ".".join(CLUSTER.split(".")[:3]) + "."
    out = run(c, "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'")
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].startswith(net):
            return parts[0]
    # fallback: any non-loopback iface
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and not parts[1].startswith("127."):
            return parts[0]
    return ""


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "--probe"
    try:
        c = connect()
    except Exception as e:
        print("NO-CONNECT (HU unreachable):", asc(e)); sys.exit(1)
    try:
        td = find_tcpdump(c)
        iface = iface_for_cluster(c)
        if arg == "--probe":
            print("=== interfaces ==="); print(asc(run(c, "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'")))
            print("cluster host:", CLUSTER, "| matched iface:", iface or "(none)")
            print("tcpdump:", td or "NOT FOUND -- need a static ARM tcpdump uploaded to /var/local (ask me to build one)")
            if td and iface:
                print("=== 5 s peek at traffic to/from the cluster (counts) ===")
                print(asc(run(c, "timeout 5 %s -nn -i %s -c 40 host %s 2>&1 | tail -20" % (td, iface, CLUSTER), t=20)))
            return
        if not td:
            print("tcpdump NOT on the HU -- cannot capture. Ask me to cross-build a static ARM tcpdump for /var/local.")
            return
        if not iface:
            print("no interface on the cluster subnet found; run --probe."); return
        if arg == "--start":
            cmd = ("nohup %s -nn -i %s -s 0 -w %s host %s >/var/local/stocknav.tcpdump.out 2>&1 & echo $! > %s; cat %s"
                   % (td, iface, REMOTE_PCAP, CLUSTER, REMOTE_PID, REMOTE_PID))
            pid = run(c, cmd).strip()
            print("started tcpdump pid=%s on %s capturing host %s -> %s" % (pid, iface, CLUSTER, REMOTE_PCAP))
            print("Now drive with the FACTORY nav guiding (no CarPlay route). Then run: python stocknav_log.py --stop")
            return
        if arg == "--stop":
            run(c, "P=$(cat %s 2>/dev/null); [ -n \"$P\" ] && kill $P 2>/dev/null; sleep 1; sync" % REMOTE_PID)
            sz = run(c, "ls -l %s 2>/dev/null" % REMOTE_PCAP).strip()
            print("stopped;", asc(sz) or "(no pcap?)")
            local = "stocknav-%s.pcap" % time.strftime("%Y%m%d-%H%M%S")
            try:
                sf = c.open_sftp(); sf.get(REMOTE_PCAP, local); sf.close()
                print("pulled ->", os.path.abspath(local), "-- open in Wireshark, filter 'someip', look at host", CLUSTER)
            except Exception as e:
                print("pull failed:", asc(e))
            return
        print(__doc__)
    finally:
        c.close()


if __name__ == "__main__":
    main()

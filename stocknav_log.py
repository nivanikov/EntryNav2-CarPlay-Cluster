#!/usr/bin/env python3
# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Capture what the FACTORY navigation sends to the instrument cluster (DKOMBI2), for offline analysis.

Cluster guidance travels over SOME/IP on the head unit's internal network; the guidance server lives on the cluster
host (default 160.48.199.96, the same one carplay-kombi talks to). This sniffs the traffic to/from that host into a
pcap you open in Wireshark (it has a SOME/IP dissector) to see the stock guidance PDUs -- the groundwork for drawing
the stock-style arrow/map from CarPlay later.

Capture it with the FACTORY nav guiding and NO CarPlay route active (so only the stock source is talking to the
cluster). Uses the head unit's tcpdump if present; otherwise it uploads our static pcapdump (build/pcapdump) to the
exec partition /opt/app/cisu and captures with that. /var/local is noexec, so the tool runs from /opt/app/cisu and
writes the pcap to /var/local.

  python stocknav_log.py --probe          # interfaces, which capture tool is available, a short peek at cluster traffic
  python stocknav_log.py --start          # start a background capture on the HU, print the PID
  python stocknav_log.py --stop [label]   # stop + pull here as stocknav-<label>-<ts>.pcap (label e.g. before / after)

Env: HU_HOST (ENET addr), HU_PW/.secrets (password), CP_KOMBI_ADDR (cluster host, default 160.48.199.96).
"""
import os
import sys
import time

import paramiko

from huauth import USER, hu_password

HOST = os.environ.get("HU_HOST", "169.254.1.0")
CLUSTER = os.environ.get("CP_KOMBI_ADDR", "160.48.199.96")
HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL_PCAPDUMP = os.path.join(HERE, "build", "pcapdump")
REMOTE_PCAPDUMP = "/opt/app/cisu/pcapdump"   # exec partition (/var/local is noexec)
REMOTE_PCAP = "/var/local/stocknav.pcap"
REMOTE_PID = "/var/local/stocknav.cap.pid"
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


def ensure_pcapdump(c):
    """Return the remote pcapdump path, uploading build/pcapdump to /opt/app/cisu if it is not already there."""
    if run(c, "[ -x %s ] && echo yes" % REMOTE_PCAPDUMP).strip() == "yes":
        return REMOTE_PCAPDUMP
    if not os.path.exists(LOCAL_PCAPDUMP):
        print("MISSING local build/pcapdump -- build it first (arm ... -static pcapdump.c -o build/pcapdump)."); return ""
    run(c, "mkdir -p /opt/app/cisu")
    sf = c.open_sftp(); sf.put(LOCAL_PCAPDUMP, REMOTE_PCAPDUMP); sf.chmod(REMOTE_PCAPDUMP, 0o755); sf.close()
    print("uploaded pcapdump ->", REMOTE_PCAPDUMP)
    return REMOTE_PCAPDUMP


def capture_tool(c):
    """(kind, path): prefer the HU's tcpdump, else our uploaded pcapdump."""
    td = find_tcpdump(c)
    if td:
        return "tcpdump", td
    pd = ensure_pcapdump(c)
    return ("pcapdump", pd) if pd else ("", "")


def iface_for_cluster(c):
    net = ".".join(CLUSTER.split(".")[:3]) + "."
    out = run(c, "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'")
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[1].startswith(net):
            return p[0]
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and not p[1].startswith("127."):
            return p[0]
    return ""


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "--probe"
    try:
        c = connect()
    except Exception as e:
        print("NO-CONNECT (HU unreachable):", asc(e)); sys.exit(1)
    try:
        iface = iface_for_cluster(c)
        if arg == "--probe":
            print("=== interfaces ==="); print(asc(run(c, "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'")))
            print("cluster host:", CLUSTER, "| matched iface:", iface or "(none)")
            td = find_tcpdump(c)
            has_pd = run(c, "[ -x %s ] && echo yes" % REMOTE_PCAPDUMP).strip() == "yes"
            print("tcpdump on HU:", td or "no", "| pcapdump uploaded:", "yes" if has_pd else "no",
                  "| local build/pcapdump:", "yes" if os.path.exists(LOCAL_PCAPDUMP) else "no")
            if iface:
                kind, path = capture_tool(c)
                if kind == "tcpdump":
                    print("=== 5 s peek (tcpdump) ===")
                    print(asc(run(c, "timeout 5 %s -nn -i %s -c 40 host %s 2>&1 | tail -15" % (path, iface, CLUSTER), t=20)))
                elif kind == "pcapdump":
                    print("=== 5 s peek (pcapdump, max 50 pkts) ===")
                    print(asc(run(c, "timeout 6 %s %s /var/local/peek.pcap %s 50 2>&1 | tail -3; rm -f /var/local/peek.pcap"
                                     % (path, iface, CLUSTER), t=12)))
            return
        if not iface:
            print("no interface on the cluster subnet found; run --probe."); return
        if arg == "--start":
            kind, path = capture_tool(c)
            if not kind:
                print("no capture tool (no tcpdump and no pcapdump); build build/pcapdump first."); return
            if kind == "tcpdump":
                cmd = "%s -nn -i %s -s 0 -w %s host %s" % (path, iface, REMOTE_PCAP, CLUSTER)
            else:
                cmd = "%s %s %s %s" % (path, iface, REMOTE_PCAP, CLUSTER)
            pid = run(c, "nohup %s >/var/local/stocknav.cap.out 2>&1 & echo $! > %s; cat %s"
                         % (cmd, REMOTE_PID, REMOTE_PID)).strip()
            print("started %s pid=%s on %s capturing host %s -> %s" % (kind, pid, iface, CLUSTER, REMOTE_PCAP))
            print("Now drive with the FACTORY nav guiding (no CarPlay route). Then: python stocknav_log.py --stop")
            return
        if arg == "--stop":
            run(c, "P=$(cat %s 2>/dev/null); [ -n \"$P\" ] && kill $P 2>/dev/null; sleep 1; sync" % REMOTE_PID)
            print("stopped;", asc(run(c, "ls -l %s 2>/dev/null; tail -1 /var/local/stocknav.cap.out 2>/dev/null" % REMOTE_PCAP).strip()))
            label = (sys.argv[2] + "-") if len(sys.argv) > 2 and sys.argv[2] else ""
            local = "stocknav-%s%s.pcap" % (label, time.strftime("%Y%m%d-%H%M%S"))
            try:
                sf = c.open_sftp(); sf.get(REMOTE_PCAP, local); sf.close()
                print("pulled ->", os.path.abspath(local), "-- open in Wireshark, filter 'someip', host", CLUSTER)
            except Exception as e:
                print("pull failed:", asc(e))
            return
        print(__doc__)
    finally:
        c.close()


if __name__ == "__main__":
    main()

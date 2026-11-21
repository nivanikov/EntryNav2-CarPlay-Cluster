#!/usr/bin/env python3
# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Turn on full shim logging for a drive and pull the trace back -- to confirm the maneuver mapping and calibrate lanes.

The shim hot-reloads /var/local/cpshim.conf (~1x/s, no CARPLAY restart). With CP_FWD_LOG + CP_RGD_DUMP on it logs, per
update: MAP (apple type + side -> android-auto type + roundabout exit + the L= lanes it built), LANE (raw per-lane
status/highlight/angles from 0x5204), NEXT (the following "then..." maneuver), and the raw 0x52xx TLV dumps.

  python drivelog.py --on       # enable CP_FWD_LOG + CP_RGD_DUMP (keeps subscribe + forward on). No restart.
  python drivelog.py --off      # quiet logging again (forward stays on)
  python drivelog.py --pull     # pull /var/local/cpshim.log here (cpshim-<ts>.log) + print a MAP/LANE/NEXT summary
  python drivelog.py --tail 60  # show the last N shim log lines

Env: HU_HOST (ENET addr), HU_PW/.secrets (password).
Workflow tomorrow: python deploy_sub.py  ->  python drivelog.py --on  ->  drive past turns/lanes/roundabouts  ->
                   python drivelog.py --pull
"""
import os
import sys
import time

import paramiko

from huauth import USER, hu_password

HOST = os.environ.get("HU_HOST", "169.254.1.0")
CONF = "/var/local/cpshim.conf"
LOG = "/var/local/cpshim.log"

ON = ("CP_RGD_PATCH=1\nCP_RGD_SUBSCRIBE=1\nCP_FORWARD=1\nCP_FWD_LOG=1\nCP_RGD_DUMP=1\n"
      "CP_MAX_CRASHES=3\nCP_STABLE_S=45\n")
OFF = ("CP_RGD_PATCH=1\nCP_RGD_SUBSCRIBE=1\nCP_FORWARD=1\nCP_FWD_LOG=0\nCP_RGD_DUMP=0\n"
       "CP_MAX_CRASHES=3\nCP_STABLE_S=45\n")


def asc(s):
    return "".join(ch if ord(ch) < 128 else "?" for ch in str(s))


def connect():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, 22, USER, hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=14)
    return c


def run(c, cmd, t=30):
    i, o, e = c.exec_command(cmd, timeout=t)
    return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")


def write_conf(c, text):
    sf = c.open_sftp()
    with sf.open(CONF, "w") as f:
        f.write(text)
    sf.chmod(CONF, 0o666)
    sf.close()


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "--pull"
    try:
        c = connect()
    except Exception as e:
        print("NO-CONNECT (HU unreachable):", asc(e)); sys.exit(1)
    try:
        if arg == "--on":
            write_conf(c, ON)
            run(c, "touch %s; chmod 666 %s" % (LOG, LOG))
            print("logging ON (CP_FWD_LOG=1 CP_RGD_DUMP=1). Shim picks it up in ~1-2 s. Drive, then --pull.")
        elif arg == "--off":
            write_conf(c, OFF)
            print("logging OFF (forward stays on).")
        elif arg == "--tail":
            n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
            print(asc(run(c, "tail -n %d %s 2>/dev/null || echo '(no log yet)'" % (n, LOG))))
        else:  # --pull
            local = "cpshim-%s.log" % time.strftime("%Y%m%d-%H%M%S")
            try:
                sf = c.open_sftp(); sf.get(LOG, local); sf.close()
                print("pulled ->", os.path.abspath(local))
            except Exception as e:
                print("pull failed:", asc(e)); return
            print("=== MAP / LANE / NEXT summary (last 40) ===")
            print(asc(run(c, "grep -E 'MAP idx=|LANE idx=|  lane\\[|NEXT idx=' %s 2>/dev/null | tail -40" % LOG)))
            print("=== 0x5202 / 0x5204 seen + apple-type histogram ===")
            print(asc(run(c, "grep -coE '0x5202 maneuver|0x5204 lane' %s 2>/dev/null; echo ---; "
                             "grep -oE 'apple=[0-9]+' %s 2>/dev/null | sort | uniq -c | sort -rn | head" % (LOG, LOG))))
    finally:
        c.close()


if __name__ == "__main__":
    main()

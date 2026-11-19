#!/usr/bin/env python3
"""EMERGENCY RECOVERY: user reports black screen, CARPLAY left stopped after a deploy was interrupted
between `systemctl stop` and `systemctl start`. Revert to fully STOCK (remove our drop-in) and bring
CARPLAY back up cleanly so the screen returns. Graceful only -- no kill -9."""
import os, time, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=40):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace")
print("=== removing our drop-in (revert to stock) ===")
print(r("rm -f /run/systemd/system/CARPLAY.service.d/zz-shim.conf; rmdir /run/systemd/system/CARPLAY.service.d 2>/dev/null; systemctl daemon-reload; echo reverted"))
print("=== restart CARPLAY clean ===")
print(r("systemctl reset-failed CARPLAY 2>/dev/null; systemctl stop CARPLAY 2>/dev/null; sleep 2; systemctl start CARPLAY; sleep 4; echo started"))
print("=== verify ===")
print(r("systemctl is-active CARPLAY; echo pid=$(pidof CARPLAY); grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null || echo 'shim_mapped=0(good,stock)'"))
c.close()

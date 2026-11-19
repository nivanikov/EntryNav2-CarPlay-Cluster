#!/usr/bin/env python3
"""Read-only HU state probe: is CARPLAY/HMI alive, did it reboot, any fresh core, is our drop-in present.
Runs NOTHING destructive -- only reads status, so we can see why the screen went black while radio plays."""
import os, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=8, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd):
    i,o,e = c.exec_command(cmd, timeout=20); return o.read().decode("utf-8","replace")
print("=== uptime (reboot check) ==="); print(r("uptime; cat /proc/uptime"))
print("=== CARPLAY pid ==="); print(r("pidof CARPLAY || echo '(CARPLAY NOT running)'"))
print("=== systemctl CARPLAY ==="); print(r("systemctl is-active CARPLAY; systemctl status CARPLAY --no-pager 2>/dev/null | head -12"))
print("=== HMI / main UI procs ==="); print(r("ps w | grep -iE 'hmi|cinemo|surface|wayland|weston|qt|browser' | grep -v grep | head -20"))
print("=== our drop-in present? ==="); print(r("ls -la /run/systemd/system/CARPLAY.service.d/ 2>/dev/null; echo '---'; cat /run/systemd/system/CARPLAY.service.d/zz-shim.conf 2>/dev/null || echo '(no drop-in)'"))
print("=== shim mapped? ==="); print(r("grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null || echo 0"))
print("=== fresh cores ==="); print(r("ls -lat /var/local/data/ResetMgr/CoreDump/ 2>/dev/null | head; echo '---deep---'; find /var/local/data/ResetMgr/CoreDump/ -type f -mmin -15 2>/dev/null"))
print("=== shim log tail ==="); print(r("tail -n 25 /var/local/cpshim.log 2>/dev/null || echo '(no log)'"))
print("=== conf ==="); print(r("cat /var/local/cpshim.conf 2>/dev/null || echo '(no conf)'"))
c.close()

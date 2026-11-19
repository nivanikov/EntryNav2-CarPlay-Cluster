#!/usr/bin/env python3
"""Read-only stability probe after recovery: is STOCK CARPLAY stable or crash-looping?
Samples pid several times, lists newest cores by mtime, and pulls the service journal reason.
Reads only -- changes nothing."""
import os, time, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=30):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace").strip()
print("pid samples (same pid = stable; changing = crash-loop):")
print(r("for i in 1 2 3 4 5; do echo -n \"$(date +%H:%M:%S) pid=\"; pidof CARPLAY || echo none; sleep 2; done"))
print("=== is-active now ==="); print(r("systemctl is-active CARPLAY"))
print("=== newest cores (mtime) ==="); print(r("ls -lat --full-time /var/local/data/ResetMgr/CoreDump/*/core.* 2>/dev/null | head -5"))
print("=== journal: why CARPLAY failed ==="); print(r("journalctl -u CARPLAY --no-pager -n 25 2>/dev/null || (systemctl status CARPLAY --no-pager | head -20)"))
c.close()

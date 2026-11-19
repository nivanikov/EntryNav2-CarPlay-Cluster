#!/usr/bin/env python3
"""Force the shimmed CARPLAY back to STOCK. The drop-in is already removed, but the launcher-managed CARPLAY process
still has the shim mapped (systemctl stop did not reach it). SIGTERM the real pid (graceful -- destructor runs, no UAF
core) so the launcher respawns it clean. Then verify a NEW pid with shim NOT mapped. Never kill -9."""
import os, time, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=30):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace")
print("before:", r("echo pid=$(pidof CARPLAY) shim=$(grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null)").strip())
print("SIGTERM:", r("kill $(pidof CARPLAY) 2>/dev/null; echo sent").strip())
time.sleep(6)
print("after :", r("echo pid=$(pidof CARPLAY) shim=$(grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null) active=$(systemctl is-active CARPLAY)").strip())
# sample stability
print("stable:", r("for i in 1 2 3 4; do echo -n \"$(pidof CARPLAY) \"; sleep 2; done").strip())
c.close()

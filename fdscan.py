#!/usr/bin/env python3
"""Read-only: map CARPLAY's open sockets to the iPhone (172.16.222.x) so v8 knows which fd(s) carry the iAP2
control channel. Shows /proc/<pid>/fd socket inodes joined to /proc/net/tcp peers. Changes nothing."""
import os, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=25):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace")
print("=== CARPLAY pid ==="); print(r("pidof CARPLAY"))
print("=== socket fds (fd -> inode) ==="); print(r("P=$(pidof CARPLAY); ls -l /proc/$P/fd 2>/dev/null | grep socket | head -40"))
print("=== tcp peers (iPhone 172.16.222.x = AC10DE..) ==="); print(r("cat /proc/net/tcp 2>/dev/null | awk 'NR==1 || $3 ~ /^AC10DE/ || $2 ~ /^AC10DE/ {print $2, $3, $4, $10}'"))
print("=== count established to iPhone ==="); print(r("grep -c ' 01 ' /proc/net/tcp 2>/dev/null"))
c.close()

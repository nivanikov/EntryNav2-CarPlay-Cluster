#!/usr/bin/env python3
"""Read-only: summarise the iAP2 sniffer output from /var/local/cpshim.log. Shows the v6 banner, every IAP line,
and a histogram of msgids seen (looking specifically for 0x52xx route-guidance). Reads only; changes nothing."""
import os, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=25):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace")
print("=== banner (latest) ==="); print(r("grep -E 'cpshim v[0-9]+ loaded' /var/local/cpshim.log | tail -2"))
print("=== RGD / identification metapool ==="); print(r("grep -E 'RGD|identification|CinemoCreateMetapool.*ident=1' /var/local/cpshim.log | tail -25"))
print("=== sniffer control frames (v7: 'IAP rd/wr fd=.. off=..') ==="); print(r("grep -E 'IAP (rd|wr) fd=[0-9]+ off=' /var/local/cpshim.log 2>/dev/null | tail -30 || echo '(none)'"))
print("=== msgid histogram ==="); print(r("grep -oE 'msgid=0x[0-9a-fA-F]+' /var/local/cpshim.log 2>/dev/null | sort | uniq -c | sort -rn | head -40"))
print("=== any 0x52xx route-guidance? ==="); print(r("grep -E 'msgid=0x52' /var/local/cpshim.log 2>/dev/null | tail -20 || echo '(none yet)'"))
print("=== RAW framing dumps (count) ==="); print(r("grep -c 'RAW ' /var/local/cpshim.log 2>/dev/null || echo 0"))
print("=== RAW by fd ==="); print(r("grep -oE 'RAW (rd|wr) fd=[0-9]+' /var/local/cpshim.log 2>/dev/null | sort | uniq -c | sort -rn | head"))
print("=== RAW sample (last 30) ==="); print(r("grep 'RAW ' /var/local/cpshim.log 2>/dev/null | tail -30 || echo '(none)'"))
print("=== raw tail 15 ==="); print(r("tail -n 15 /var/local/cpshim.log"))
print("=== CarPlay link ==="); print(r("ip -4 addr show 2>/dev/null | grep -E 'wlan0|usb|172\\.' | head"))
c.close()

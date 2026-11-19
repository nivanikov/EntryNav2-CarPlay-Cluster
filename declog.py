#!/usr/bin/env python3
"""Read-only: show the v12 Encode/Decode shim events from /var/local/cpshim.log -- the handshake (0x1D02/0xAA05),
transport capture, patch, injection readiness, and any 0x52xx. Reads only."""
import os, paramiko
HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect(HOST, 22, "root", hu_password(), timeout=10, allow_agent=False, look_for_keys=False, banner_timeout=12)
def r(cmd, t=25):
    i,o,e = c.exec_command(cmd, timeout=t); return o.read().decode("utf-8","replace")
print("=== banner ==="); print(r("grep -E 'cpshim v1[0-9]' /var/local/cpshim.log | tail -1"))
print("=== FULL Identify (last one) ==="); print(r("grep -nA 32 'ENC Identify FULL' /var/local/cpshim.log | tail -34"))
print("=== handshake + transport + patch + inject ==="); print(r("grep -E '0x1D02|0x1D03|0xAA05|captured NmeTransport|ENC PATCH|inject' /var/local/cpshim.log | tail -20"))
print("=== any 0x52xx ==="); print(r("grep -E '0x520|0x5201|0x5202|0x5204' /var/local/cpshim.log | tail -10 || echo none"))
print("=== DEC raw tail ==="); print(r("grep 'DEC raw' /var/local/cpshim.log | tail -14"))
c.close()

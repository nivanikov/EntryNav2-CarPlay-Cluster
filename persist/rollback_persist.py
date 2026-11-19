#!/usr/bin/env python3
"""Undo the cpshim persistent install (install_persist.py) and return CARPLAY to stock.

Removes the AUFS update layer + marker + our files, unmounts the overlay if it is live (post-reboot), removes the
/run drop-in, and restarts CARPLAY clean. Safe to run anytime; recovers a HU where a bad persistent install stopped
CarPlay from starting. After running, reboot once to be fully back to stock (the layer marker is gone, so
aufs_mount.sh will not re-mount the overlay).
"""
import os, time
import paramiko

HOST = os.environ.get("HU_HOST", "169.254.1.0")
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from huauth import hu_password
USER, PW = "root", hu_password()
CP = "/opt/app/cisu/cpshim"
LAYER = "/opt/app/cisu/HeadUnitInstaller/install/UpdateLayer1"
MARKER = "/opt/app/cisu/HeadUnitInstaller/install/layer1.filestore"
RUN_DROPIN = "/run/systemd/system/CARPLAY.service.d/zz-cpshim-wrap.conf"


def asc(s):
    return "".join(ch if ord(ch) < 128 else "?" for ch in str(s))


def main():
    c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        c.connect(HOST, 22, USER, PW, timeout=12, allow_agent=False, look_for_keys=False, banner_timeout=14)
    except Exception as e:
        print("NO-CONNECT:", asc(e)); return

    def run(cmd, t=90):
        i, o, e = c.exec_command(cmd, timeout=t)
        return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")

    print("=== rollback: remove layer marker FIRST (so a reboot can't re-mount), then files ===")
    # 1) kill the persistence trigger immediately
    run("rm -f %s; sync" % MARKER)
    # 2) remove the live drop-in
    run("rm -f %s" % RUN_DROPIN)
    # 3) unmount the overlay if it is live (only after a reboot), then remove layer + our files
    print(asc(run(r"""
if mount | grep -i aufs | grep -q /opt/app/usr; then umount /opt/app/usr 2>&1 && echo 'aufs unmounted' || echo 'aufs umount FAILED (busy) -- reboot clears it'; else echo 'no live aufs overlay'; fi
rm -rf %s %s
echo 'removed layer + cpshim files'
""" % (LAYER, CP))))
    # 4) stop kombi
    run("systemctl stop carplay-kombi 2>/dev/null; systemctl reset-failed carplay-kombi 2>/dev/null; for p in $(pidof carplay-kombi); do kill -9 $p; done")
    # 5) restart CARPLAY clean (single instance) -> stock
    print(asc(run(r"""systemctl daemon-reload
OLD=$(pidof CARPLAY); [ -n "$OLD" ] && kill $OLD 2>/dev/null
for i in $(seq 1 20); do [ -z "$(pidof CARPLAY)" ] && break; sleep 1; done
systemctl start CARPLAY 2>/dev/null
for i in $(seq 1 15); do [ -n "$(pidof CARPLAY)" ] && break; sleep 1; done
echo "stock pid=$(pidof CARPLAY) shim_mapped=$(grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null||echo 0)"
echo "NOTE: reboot once to fully clear the overlay if it was live."
""", t=90)))
    c.close()


if __name__ == "__main__":
    main()

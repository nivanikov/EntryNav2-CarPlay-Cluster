#!/usr/bin/env python3
"""Install the cpshim CarPlay->cluster stack PERSISTENTLY on the EntryNav2 head unit, surviving reboots.

Why this works (see memory g21-papa-entrynav2.md): the HU rootfs/etc and every systemd unit dir are read-only; the
only writable+exec+persistent partitions are /opt/app/cisu (p6) and /opt/navd (p7). The stock boot script
/usr/local/bin/aufs_mount.sh (aufs_mount.service, early boot) AUFS-unions
/opt/app/cisu/HeadUnitInstaller/install/UpdateLayer1 OVER /opt/app/usr IF a layer1.filestore marker exists -- and the
whole CarPlay/Cinemo stack lives under /opt/app/usr (/usr/local/bin/CARPLAY -> /opt/app/usr/MediaPlayer/bin/CARPLAY).
So we:
  * put our binaries on /opt/app/cisu/cpshim (persistent, exec),
  * copy the real CARPLAY binary to /opt/app/cisu/cpshim/CARPLAY.real,
  * place a tiny shell WRAPPER in the update layer at MediaPlayer/bin/CARPLAY (shadows the stock binary): it starts
    kombi, LD_PRELOADs the shim, and exec's CARPLAY.real. Only a *script* is in the overlay, so no exec from AUFS is
    needed. At boot aufs_mount.sh mounts the layer -> CARPLAY path becomes our wrapper -> shim+kombi load automatically.

Modes:
  python install_persist.py            # STAGE only: files + update layer + marker + conf. Activates at next REBOOT.
  python install_persist.py --activate # also run it THIS session (via a /run drop-in) + restart CARPLAY + verify.
  python install_persist.py --status   # show what's installed.
Rollback any time: python rollback_persist.py   (removes the layer + files, restarts stock CARPLAY).

SAFETY: a bad persistent install can only stop CarPlay from starting (the HU still boots); recover over SSH with
rollback_persist.py then reboot. The v24 shim has an anti-boot-loop crash-counter so a subscribe regression can't loop.
"""
import sys, os, time
import paramiko

HOST = os.environ.get("HU_HOST", "169.254.1.0")
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from huauth import hu_password
USER, PW = "root", hu_password()
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")
LOCAL_SHIM = os.path.join(BUILD, "carplay-shim.so")
LOCAL_KOMBI = os.path.join(BUILD, "carplay-kombi")
LOCAL_WRAP = os.path.join(HERE, "carplay-wrapper.sh")
LOCAL_CONF = os.path.join(HERE, "cpshim.conf")

CP = "/opt/app/cisu/cpshim"                                   # our persistent exec dir (p6)
STOCK_CARPLAY = "/opt/app/usr/MediaPlayer/bin/CARPLAY"        # the real binary (ro rootfs)
LAYER = "/opt/app/cisu/HeadUnitInstaller/install/UpdateLayer1"
MARKER = "/opt/app/cisu/HeadUnitInstaller/install/layer1.filestore"
OVL_CARPLAY = LAYER + "/MediaPlayer/bin/CARPLAY"              # the wrapper, overlaid onto the CARPLAY path
CONF = "/var/local/cpshim.conf"
RUN_DROPIN = "/run/systemd/system/CARPLAY.service.d/zz-cpshim-wrap.conf"


def connect(t=12):
    c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, 22, USER, PW, timeout=t, allow_agent=False, look_for_keys=False, banner_timeout=t + 2)
    return c


def run(c, cmd, t=60):
    i, o, e = c.exec_command(cmd, timeout=t)
    out = o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")
    o.channel.recv_exit_status()
    return out


def asc(s):
    return "".join(ch if ord(ch) < 128 else "?" for ch in str(s))


def put(sf, local, remote, mode):
    sf.put(local, remote); sf.chmod(remote, mode)


def status(c):
    print(asc(run(c, "echo 'cpshim dir:'; ls -la %s 2>&1; echo; echo 'update layer:'; ls -la %s/MediaPlayer/bin 2>&1; "
                     "echo; echo 'marker:'; ls -la %s 2>&1; echo; echo 'conf:'; cat %s 2>&1; echo; "
                     "echo 'crash-counter:'; cat /var/local/cpshim.boot 2>&1; echo; "
                     "echo 'aufs on /opt/app/usr:'; mount | grep -i aufs | grep -i /opt/app/usr || echo '(not mounted -- normal until reboot)'"
                     % (CP, LAYER, MARKER, CONF))))


def stage(c):
    print("=== staging persistent install ===")
    run(c, "mkdir -p %s %s/MediaPlayer/bin /run/systemd/system/CARPLAY.service.d" % (CP, LAYER))
    sf = c.open_sftp()
    for lf in (LOCAL_SHIM, LOCAL_KOMBI, LOCAL_WRAP, LOCAL_CONF):
        if not os.path.exists(lf):
            print("MISSING local file:", lf); sf.close(); sys.exit(2)
    put(sf, LOCAL_SHIM, CP + "/carplay-shim.so", 0o755)
    put(sf, LOCAL_KOMBI, CP + "/carplay-kombi", 0o755)
    put(sf, LOCAL_WRAP, CP + "/carplay-wrapper.sh", 0o755)
    put(sf, LOCAL_CONF, CONF, 0o664)
    # the overlay wrapper (what shadows the CARPLAY binary) -- same content as the staged wrapper
    put(sf, LOCAL_WRAP, OVL_CARPLAY, 0o755)
    sf.close()
    print("uploaded shim/kombi/wrapper/conf")
    # copy the REAL CARPLAY binary next to our files (on-car cp; it's readable)
    out = run(c, "cp -f %s %s/CARPLAY.real && chmod 755 %s/CARPLAY.real && echo OK; ls -la %s/CARPLAY.real"
                 % (STOCK_CARPLAY, CP, CP, CP))
    print(asc("CARPLAY.real: " + out.strip()))
    # the marker that makes aufs_mount.sh mount the layer at boot
    run(c, "echo -n > %s; sync" % MARKER)
    print("marker written:", MARKER)
    print(">>> STAGED. It activates automatically at the next REBOOT (aufs_mount.sh mounts the layer).")


def activate_now(c):
    """Run the wrapper THIS session via a /run drop-in (no overlay-over-live-/opt/app/usr), restart CARPLAY, verify."""
    print("=== activating for THIS session (via /run drop-in) + verifying ===")
    dropin = ('[Service]\nExecStart=\nExecStart=%s/carplay-wrapper.sh\n' % CP)
    sf = c.open_sftp()
    with sf.open(RUN_DROPIN, "w") as fp:
        fp.write(dropin)
    sf.close()
    run(c, "touch /var/local/cpshim.log; chmod 666 /var/local/cpshim.log")
    # clean single-instance restart (avoid the two-instance abort): SIGTERM old, wait gone, start one
    print(asc(run(c, r"""systemctl daemon-reload
OLD=$(pidof CARPLAY); [ -n "$OLD" ] && kill $OLD 2>/dev/null
for i in $(seq 1 20); do [ -z "$(pidof CARPLAY)" ] && break; sleep 1; done
systemctl start CARPLAY 2>/dev/null
for i in $(seq 1 15); do [ -n "$(pidof CARPLAY)" ] && break; sleep 1; done
echo started pid=$(pidof CARPLAY)""", t=90)))
    time.sleep(2)
    pids = []
    for _ in range(6):
        pids.append(run(c, "pidof CARPLAY||echo none").strip()); time.sleep(2)
    shim = run(c, "grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null||echo 0").strip()
    kombi = run(c, "pidof carplay-kombi").strip()
    core = run(c, "find /var/local/data/ResetMgr/CoreDump -name 'core.CARPLAY.*' -mmin -2 2>/dev/null").strip()
    up = [p for p in pids if p and p != "none"]
    stable = len(up) == len(pids) and len(set(up)) == 1
    ok = stable and shim != "0" and not core
    print("verify: pids=%s shim_mapped=%s kombi=%s fresh_core=%s" % (pids, shim, kombi or "DOWN", core or "(none)"))
    if ok:
        print(">>> ACTIVE this session AND staged for reboot. Drive + check the cluster.")
    else:
        print("!!! verify FAILED -> removing the live drop-in (overlay files stay; run rollback_persist.py to remove them)")
        run(c, "rm -f %s; systemctl daemon-reload; OLD=$(pidof CARPLAY); [ -n \"$OLD\" ] && kill $OLD; sleep 2; systemctl start CARPLAY" % RUN_DROPIN)


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    try:
        print("local shim:", os.path.getsize(LOCAL_SHIM), "bytes;", "kombi:", os.path.getsize(LOCAL_KOMBI), "bytes")
    except OSError as e:
        print("MISSING local build:", e); sys.exit(2)
    try:
        c = connect()
    except Exception as e:
        print("NO-CONNECT (HU unreachable):", asc(e)); sys.exit(1)
    try:
        if arg == "--status":
            status(c)
        else:
            stage(c)
            if arg == "--activate":
                activate_now(c)
            status(c)
    finally:
        c.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Deploy the CARPLAY LD_PRELOAD shim to the EntryNav2 head unit -- SINGLE-SHOT with VERIFY + AUTO-REVERT.

Why this shape (2026-11-17): the old 40x retry loop turned a shim crash into a REBOOT LOOP -- CARPLAY crash ->
ResetMgr reboots the HU -> SSH drops mid-exec -> the loop re-deployed -> crash again. Two HU reboots + black screen.
Now:
  * ONE connect attempt, foreground. No retry loop. (If the HU is unreachable it just prints and exits -- CARPLAY
    is never touched, so the screen stays up.)
  * ResetMgr-safe restart: graceful SIGTERM only, never `kill -9`.
  * After restart we VERIFY CARPLAY actually came up and STAYS up (stable pid, active, no fresh core) for ~12 s.
  * If it did NOT come up (stuck / crashed without reboot), we AUTO-REVERT to stock (remove the drop-in, restart
    clean) in the same run, so we never leave a broken drop-in or a black screen.
  * If the shim crash triggers a full HU reboot, /tmp (shim) and /run (drop-in) are tmpfs and get wiped, so CARPLAY
    comes back STOCK on its own -- single-shot can't loop. We detect the dropped connection, reconnect, and confirm
    stock.

Usage (env picks the shim behaviour baked into the drop-in; CP_IAP_SNIFF is LOAD-TIME in v6, set it here):
    CP_ENV='"CP_IDENT_RECON=1"'                     python deploy_sub.py   # safe identification recon (default)
    CP_ENV='"CP_RGD=1" "CP_IAP_SNIFF=1"'            python deploy_sub.py   # declare RGD + passive iAP2 sniffer
The drop-in auto-clears on reboot, so a deploy is fully reversible.
"""
import sys, time, os, paramiko

HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
USER, PW = "root", hu_password()
LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "carplay-shim.so")
REMOTE = "/tmp/carplay-shim.so"
EXTRA = os.environ.get("CP_ENV", '"CP_IDENT_RECON=1"')

DROPIN = (
    "[Service]\n"
    'Environment="LD_PRELOAD=/tmp/carplay-shim.so" "CP_SHIM_LOG=/var/local/cpshim.log" '
    '"CP_CONF=/var/local/cpshim.conf" '
    + EXTRA + "\n"
)
RESTART = r"""
mkdir -p /run/systemd/system/CARPLAY.service.d
cat > /run/systemd/system/CARPLAY.service.d/zz-shim.conf <<'CONF'
%s
CONF
touch /var/local/cpshim.log;  chmod 666 /var/local/cpshim.log
touch /var/local/cpshim.gen;  chmod 666 /var/local/cpshim.gen
OLD=$(pidof CARPLAY)
systemctl daemon-reload
# Graceful cycle. CARPLAY is launcher/systemd-respawned, so the reliable signal that the NEW (shimmed) instance is up
# is "pidof differs from OLD", not "pidof is empty" (the old flaky sleep-4 check often saw the respawn and bailed, or
# killed an instance mid-handshake -> teardown core). SIGTERM only, never -9.
systemctl restart CARPLAY 2>/dev/null &
[ -n "$OLD" ] && kill "$OLD" 2>/dev/null
NEW=""
for i in $(seq 1 24); do
    sleep 1
    NEW=$(pidof CARPLAY)
    if [ -n "$NEW" ] && [ "$NEW" != "$OLD" ]; then break; fi
    if [ "$i" = "10" ] && [ "$NEW" = "$OLD" ]; then kill "$OLD" 2>/dev/null; fi   # re-nudge once, still graceful
done
echo "restart done; old=$OLD new=$(pidof CARPLAY)"
""" % DROPIN

REVERT = r"""
rm -f /run/systemd/system/CARPLAY.service.d/zz-shim.conf
rmdir /run/systemd/system/CARPLAY.service.d 2>/dev/null
systemctl daemon-reload
systemctl reset-failed CARPLAY 2>/dev/null
systemctl stop CARPLAY 2>/dev/null
sleep 2
systemctl start CARPLAY
sleep 3
echo "reverted-to-stock pid=$(pidof CARPLAY) active=$(systemctl is-active CARPLAY)"
"""


def connect(timeout=10):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, 22, USER, PW, timeout=timeout, allow_agent=False, look_for_keys=False, banner_timeout=14)
    return c


def run(c, cmd, t=60):
    i, o, e = c.exec_command(cmd, timeout=t)
    out = o.read().decode("utf-8", "replace")
    o.channel.recv_exit_status()
    return out


def reconnect(tries=20):
    """After a possible HU reboot, wait for SSH to come back (bounded)."""
    for _ in range(tries):
        try:
            return connect(timeout=8)
        except Exception:
            time.sleep(3)
    return None


def verify_stable(c):
    """Sample CARPLAY pid over ~12 s. Return (ok, detail). ok=True iff a single pid stays up the whole window."""
    pids = []
    for _ in range(6):
        pids.append(run(c, "pidof CARPLAY || echo none", t=15).strip())
        time.sleep(2)
    active = run(c, "systemctl is-active CARPLAY", t=15).strip()
    fresh = run(c, "find /var/local/data/ResetMgr/CoreDump -type f -name 'core.CARPLAY.*' -mmin -2 2>/dev/null", t=15).strip()
    up = [p for p in pids if p and p != "none"]
    stable = len(up) == len(pids) and len(set(up)) == 1
    ok = stable and not fresh
    return ok, "pids=%s active=%s fresh_core=%s" % (pids, active, fresh or "(none)")


def main():
    print("=== shim stat ===")
    try:
        print("local shim:", os.path.getsize(LOCAL), "bytes")
    except OSError as e:
        print("MISSING local shim:", e); sys.exit(2)

    try:
        c = connect()
    except Exception as e:
        print("NO-CONNECT (HU unreachable) -- CARPLAY untouched, screen safe:", e); sys.exit(1)

    try:
        sf = c.open_sftp(); sf.put(LOCAL, REMOTE); sf.chmod(REMOTE, 0o644); sf.close()
        print("uploaded shim ->", REMOTE)
    except Exception as e:
        print("upload failed -- CARPLAY untouched:", e); c.close(); sys.exit(1)

    rebooted = False
    try:
        print(run(c, RESTART, t=60))
    except Exception as e:
        # Connection dropped during restart => shim likely crashed CARPLAY -> HU reboot. /tmp+/run are tmpfs, wiped on
        # reboot, so CARPLAY will come back STOCK by itself. Reconnect and confirm, then report.
        print("RESTART connection dropped (probable crash+reboot):", e)
        rebooted = True
        try: c.close()
        except Exception: pass
        c = reconnect()
        if not c:
            print("DEPLOY_FAILED: HU did not return on SSH after reboot -- check the car."); sys.exit(1)

    if rebooted:
        st = run(c, "pidof CARPLAY || echo none; echo active=$(systemctl is-active CARPLAY); "
                    "grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null || echo shim_mapped=0", t=20)
        print("post-reboot (auto-reverted to stock by tmpfs wipe):\n", st)
        print("DEPLOY_FAILED: shim crashed CARPLAY -> HU rebooted -> back to STOCK. Fix shim before retrying.")
        c.close(); sys.exit(1)

    ok, detail = verify_stable(c)
    print("verify:", detail)
    if ok:
        print(run(c, "grep -c carplay-shim /proc/$(pidof CARPLAY)/maps 2>/dev/null && echo '(shim mapped)'; "
                    "tail -n 3 /var/local/cpshim.log 2>/dev/null", t=20))
        print("DEPLOY_OK: CARPLAY up and stable with shim.")
        c.close(); sys.exit(0)
    else:
        print("CARPLAY not stable -> AUTO-REVERT to stock ...")
        print(run(c, REVERT, t=40))
        print("DEPLOY_FAILED: shim did not run cleanly; reverted to stock.")
        c.close(); sys.exit(1)


if __name__ == "__main__":
    main()

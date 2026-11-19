#!/usr/bin/env python3
"""Hot-reload driver for the v4 CARPLAY shim: write /var/local/cpshim.conf over SSH WITHOUT restarting CARPLAY.

The v4 shim re-reads this file (mtime-gated, ~1x/s) so changing a knob takes effect within ~1-2 s with NO CARPLAY
restart -> no SIGTERM, no ResetMgr reboot, no iPhone unpair. This is what makes on-car subscribe tuning safe.

Each run REPLACES the whole conf with the KEY=VALUE pairs you pass (one per arg, or a single space/newline string).
Read it back with no args.

Examples:
    python cpconf.py                                   # print current conf
    python cpconf.py CP_RGD=1 CP_IDENT_RECON=1         # RGD on, recon on, subscribe still off
    python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=1            # arm ONE subscribe attempt (gen 1)
    python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=2 CP_CTX_BUF=1   # retry with a non-null ctx (new gen)
    python cpconf.py CP_RGD=1 CP_SUBSCRIBE=1 CP_SUB_GEN=1 CP_NO_CALL=1   # safe channel recon (does NOT spend the gen)

Reset the crash-loop marker (so CP_SUB_GEN=1 arms again) with:  python cpconf.py --reset-gen <then write conf>
"""
import sys, os, paramiko

HOST = os.environ.get("HU_HOST", "169.254.1.0")
from huauth import hu_password
USER, PW = "root", hu_password()
CONF = os.environ.get("CP_CONF_PATH", "/var/local/cpshim.conf")
GEN = os.environ.get("CP_GEN_PATH", "/var/local/cpshim.gen")


def connect():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=22, username=USER, password=PW, timeout=8,
              allow_agent=False, look_for_keys=False, banner_timeout=12)
    return c


def run(c, cmd):
    i, o, e = c.exec_command(cmd, timeout=20)
    out = o.read().decode("utf-8", "replace")
    o.channel.recv_exit_status()
    return out


def main():
    args = sys.argv[1:]
    reset_gen = False
    if args and args[0] == "--reset-gen":
        reset_gen = True
        args = args[1:]

    c = connect()
    if reset_gen:
        # Write "0" (not rm): /var/local is root-only, so if we delete it the carplay user can't recreate it and the
        # guard breaks. Keep the file present and world-writable at 0.
        print("reset gen marker:", run(c, "printf '0\\n' > %s && chmod 666 %s && echo 'reset to 0'" % (GEN, GEN)).strip())

    if not args:
        print("=== current %s ===" % CONF)
        print(run(c, "cat %s 2>/dev/null || echo '(absent)'" % CONF))
        print("=== gen marker ===")
        print(run(c, "cat %s 2>/dev/null || echo '(absent=0)'" % GEN))
        c.close()
        return

    # Build conf text: args may be individual KEY=VAL tokens or one combined string.
    toks = []
    for a in args:
        toks += a.replace("\n", " ").split()
    text = "\n".join(t for t in toks if "=" in t) + "\n"

    sf = c.open_sftp()
    with sf.open(CONF, "w") as f:
        f.write(text)
    sf.chmod(CONF, 0o666)
    sf.close()
    print("wrote %s (no CARPLAY restart; shim picks it up within ~1-2 s):" % CONF)
    print(text)
    print("verify on HU:", run(c, "cat %s" % CONF).strip())
    c.close()


if __name__ == "__main__":
    main()

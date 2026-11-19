#!/bin/sh
# cpshim persistent CARPLAY wrapper.
#
# Installed (via the AUFS update layer) at /opt/app/usr/MediaPlayer/bin/CARPLAY, shadowing the stock binary. Systemd's
# ExecStart=/usr/local/bin/CARPLAY -> symlink -> here. We:
#   1. start the cluster client (carplay-kombi) once -- it's a singleton; CARPLAY.service is KillMode=none, so it
#      survives CARPLAY restarts and we never spawn a second (the pidof guard),
#   2. LD_PRELOAD the shim,
#   3. exec the REAL CARPLAY binary so the SAME pid continues -> systemd's watchdog/KillMode are unaffected.
#
# All our files live on the persistent, exec-capable partition /opt/app/cisu (p6); only THIS shell script sits in the
# AUFS overlay (a script is read by /bin/sh, so no exec-mmap from the overlay is needed). Rollback = remove the layer.
CP=/opt/app/cisu/cpshim

if ! pidof carplay-kombi >/dev/null 2>&1; then
    "$CP/carplay-kombi" >/tmp/carplay-kombi.log 2>&1 &
fi

export LD_PRELOAD="$CP/carplay-shim.so"
export CP_SHIM_LOG=/var/local/cpshim.log
export CP_CONF=/var/local/cpshim.conf

exec "$CP/CARPLAY.real" "$@"

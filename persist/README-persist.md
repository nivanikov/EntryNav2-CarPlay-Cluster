# cpshim persistence (survives reboot) — EntryNav2

Makes the CarPlay→cluster arrow stack load automatically on every boot, instead of a per-connect redeploy.

## How it works
The HU is locked down: `/`, `/etc`, and all systemd unit dirs are read-only; `/var/local` is persistent but **noexec**;
`/run` is tmpfs (wiped). The **only** writable + exec + persistent partition is `/opt/app/cisu` (`/dev/mmcblk0p6`).

The stock boot script `/usr/local/bin/aufs_mount.sh` (`aufs_mount.service`, early boot) AUFS-unions
`/opt/app/cisu/HeadUnitInstaller/install/UpdateLayer1` **over** `/opt/app/usr` — **if** a `layer1.filestore` marker
exists. The whole CarPlay/Cinemo stack lives under `/opt/app/usr`
(`/usr/local/bin/CARPLAY` → `/opt/app/usr/MediaPlayer/bin/CARPLAY`).

So we:
- put `carplay-shim.so`, `carplay-kombi`, a copy of the real binary (`CARPLAY.real`), and `cpshim.conf` on
  `/opt/app/cisu/cpshim` (persistent, exec);
- put a tiny **shell wrapper** in the update layer at `MediaPlayer/bin/CARPLAY` (shadows the stock binary). It starts
  kombi (singleton), `LD_PRELOAD`s the shim, and `exec`s `CARPLAY.real`. Only a *script* is in the overlay → no exec
  from AUFS needed;
- drop the `layer1.filestore` marker.

At boot, `aufs_mount.sh` mounts the layer → the CARPLAY path becomes our wrapper → shim + kombi load automatically.

## De-risking already done (no reboot needed to learn these)
- AUFS works: a live `mount -t aufs` test succeeded; overlay shadows lower + adds files.
- The wrapper only needs `/bin/sh`, not AUFS-exec; kombi/shim run from `/opt/app/cisu` (confirmed exec).
- v24 shim has an **anti-boot-loop crash-counter** (`/var/local/cpshim.boot`): bumps before the first subscribe each
  boot, resets after `CP_STABLE_S` of stable running; at `CP_MAX_CRASHES` consecutive boot-crashes it stops
  subscribing — CarPlay keeps working, no arrows — so a regression can never loop the car.

## Install (from the laptop, HU connected)
```
cd carplay-cluster/persist
python install_persist.py --activate   # stage + run THIS session via a /run drop-in + verify
```
`--activate` validates the exact wrapper behaviour live (shim loads, kombi runs, CarPlay stable) without touching the
live `/opt/app/usr`. If verify fails it auto-removes the live drop-in and tells you to roll back.

Then **reboot once** to confirm the automatic path (aufs_mount.sh mounts the overlay). After boot:
```
python install_persist.py --status     # shim mapped? kombi up? aufs on /opt/app/usr?
```
Expect CarPlay to work with cluster arrows with no laptop attached.

- `python install_persist.py` (no flag) = stage only; activates at next reboot.

## Rollback (recovery)
```
python rollback_persist.py             # removes marker+layer+files, unmounts overlay, restarts stock CARPLAY
```
Then reboot once. A bad install can only *stop CarPlay from starting* (the HU still boots and SSH still works, since
sshd is not under `/opt/app/usr`) — never a brick.

## Updating the shim later
Rebuild `build/carplay-shim.so` (and `build/carplay-kombi` if changed), then `python install_persist.py` again
(re-stages the files). Reboot or `--activate` to apply.

# Field test plan (on-car)

Order: **factory navigation first** (learn what it draws on the cluster), then the CarPlay maneuver/lane checks.
Reach the head unit over the ENET (OBD→Ethernet) cable and set the environment once:

```sh
export HU_HOST=169.254.1.0        # the car's ENET address
export HU_PW='…'                  # head unit root password (or put it in .secrets/hu-password.txt)
```

## Phase A — factory navigation → cluster (do this first)

Goal: capture the SOME/IP guidance the built-in navigation sends to the DKOMBI2 cluster — groundwork for a
stock-style arrow/map from CarPlay. Capture with **a route in the built-in nav and NO CarPlay route active**, so only
the factory source is talking to the cluster.

1. `python stocknav_log.py --probe` — confirm the interface on the cluster subnet and which capture tool is available
   (HU tcpdump, else our uploaded `pcapdump`).
2. **Streckenübersicht OFF** (current `NAVI_STRECKENUEBERSICHT_ENABLE = nicht_aktiv [00]`):
   - `python stocknav_log.py --start`
   - drive a stretch with a few turns + a roundabout; let the factory nav guide the cluster
   - `python stocknav_log.py --stop before`   → `stocknav-before-<ts>.pcap`
3. **Enable the coding** `NAVI_STRECKENUEBERSICHT_ENABLE = aktiv [01]` — **back up the current coding/CAFD first**,
   change only this one value, recode/reboot as your tool needs.
4. **Streckenübersicht ON** — repeat the same drive:
   - `python stocknav_log.py --start` → drive a similar stretch → `python stocknav_log.py --stop after`
     → `stocknav-after-<ts>.pcap`
5. Compare `before` vs `after` in Wireshark (filter `someip`, host `160.48.199.96`): note any new/changed service,
   method or PDU. That tells us whether the coding adds cluster content and exposes more of the guidance protocol.
   - If anything misbehaves, recode `NAVI_STRECKENUEBERSICHT_ENABLE` back to `[00]`.

## Phase B — CarPlay maneuvers + lanes

1. `python deploy_sub.py` — upload the **fixed** shim (`build/carplay-shim.so`), restart CARPLAY under it; it verifies
   and auto-reverts to stock if anything is wrong. (A reboot also falls back to stock.)
2. `python drivelog.py --on` — full logging (`CP_FWD_LOG` + `CP_RGD_DUMP`), hot, no restart.
3. Navigate with the iPhone over CarPlay and drive past, deliberately: a left turn, a right turn, a U-turn, a
   roundabout (note which exit), an **off-ramp** and an **on-ramp**, a **lane** junction, and arrive at the destination.
4. `python drivelog.py --pull` → `cpshim-<ts>.log`, then read the summary:
   - **MAP** lines — `apple=<type> side=<n> -> aa=<type> exit=<n>`: is each arrow correct now? (fixes 1/2/4/5)
   - **LANE** lines — raw per-lane `status/highlight/angles` vs what the cluster drew: calibrate the angle→shape map.
   - **NEXT** lines — the "then…" maneuver (data only for now).
5. If stable and correct, make it permanent: `python persist/install_persist.py` (then reboot once to confirm).

## Recovery (any time)

- `python recover.py` — black-screen rescue (revert to stock, restart CARPLAY).
- `python kstock.py` — force the shimmed CARPLAY back to stock.
- `python persist/rollback_persist.py` — remove the persistent install.

Safety throughout: graceful `SIGTERM` only, single-instance deploy, auto-revert, reboot falls back to stock.

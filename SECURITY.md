# Security policy

## Reporting a vulnerability

Please report security issues privately, through GitHub: the **Security** tab of this repository, then
**Report a vulnerability**. Do not open a public issue for them.

## What this project touches

This project runs on a car's head unit (BMW EntryNav2 / EntryEvo, Cinemo). It is installed by hand over SSH on the
car's ENET (OBD-to-Ethernet) cable. Things worth knowing when assessing a report:

| Area | Behaviour |
|---|---|
| Head unit root password | Not handled, stored or changed by this project. The scripts read it from `$HU_PW` or the git-ignored `.secrets/hu-password.txt`; it is never committed, and `tools/verify_repo.py` (a pre-push hook) refuses to push any blob that contains it. EntryNav2 units are known to share a default root password: treat SSH on the unit as unprotected and keep the ENET port physically controlled. |
| Changes on the car | An `LD_PRELOAD` library (`carplay-shim.so`) is loaded into the stock `CARPLAY` process, and optionally an AUFS update-layer makes that automatic at boot (see `persist/`). No BMW file is modified; the system partition is writable only during the copy. A bounded crash-counter stops the shim re-subscribing after repeated boot-crashes, so a regression can never loop the car — CarPlay keeps working, with or without cluster arrows. |
| Network on the car | The shim sends a small UDP datagram to the forwarder on `127.0.0.1:42042` (loopback only). Guidance reaches the instrument cluster over SOME/IP on the car's internal network. Nothing listens on an external interface. |
| Deploy safety | Restarts are graceful (`SIGTERM` only, never `kill -9`); a transient deploy auto-reverts to stock if the shim does not come up cleanly, and a reboot always falls back to stock (the transient drop-in lives on tmpfs). |

## Scope

In scope: this repository's code and its installation procedure.

Out of scope: vulnerabilities in BMW's own software, in Apple CarPlay, in the Cinemo stack or in the phone, unless this
project makes them reachable or worse. Please report those to their vendors.

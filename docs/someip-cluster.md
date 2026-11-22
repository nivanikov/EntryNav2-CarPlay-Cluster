# Cluster guidance SOME/IP protocol (DKOMBI2) — reverse-engineering notes

Decoded from a factory-navigation capture (`stocknav_log.py` → `someip_decode.py`). This is what the head unit sends to
the instrument cluster to draw guidance. carplay-kombi already speaks a subset of it (`kombi_graph.cpp`); these notes
fill in what it does **not** yet do (the secondary "then…" maneuver and the lane/junction graphic).

## Transport

- Cluster guidance server: **`160.48.199.96:30502`** (TCP), SOME/IP **service `0xb006`**, instance handle **`0xC8`**.
- Service discovery / keepalive on UDP `:30490` (`svc 0xffff meth 0x8100`), magic-cookie `svc 0xffff meth 0x0000`.
- carplay-kombi connects here as a client and renders the same way (so our path is the factory path).

## Methods (what each carries)

| method | carplay-kombi name | carries |
|---|---|---|
| `0x0003` | `kTextMethod` | TEXT model[3] — road / street name (UTF-16) |
| `0x0009` | `kRouteMethod` | DESTINATION model[17] (car position, distance counts down), STATE model[33], model[30] |
| `0x000A` | `kManeuverMethod` | the maneuver: DESCRIPTION model[15], DISTANCE model[29], geometry model[34] |
| `0x000B` | `kGuidingMethod` | model[7] — guiding/junction container |
| **`0x000C`** | **(not handled!)** | **model[32] — the MANEUVER LIST (ordered refs to the description models)** |
| `0x000E` | (not handled) | model[40] |
| `0x0010` | `kLaneMethod` | lane models (`setLaneGuidingData`) — kombi builds these, but see Lanes below |

## Model framing

Each model body starts: `handle(4=000000c8) | modelId(4) | type(1) | length(4=body after modelAt) | ... | modelAt@17:
modelId echo + type echo | fields`. (kombi_graph.cpp: `kModelIdAt=4, kModelTypeAt=8, kLengthAt=9, kModelAt=17,
kTypeEchoAt=21, kManeuverAt=23`.) The low byte of `modelId` is the **instance index** (0 = current maneuver, 1/2 = the
following ones).

## The secondary maneuver (issue #2)

The DESCRIPTION model[15] (method `0x000A`) ties a maneuver's parts together by **referencing other model ids**:
`… 27000000 22000000 07000000 1d000000` = refs to model `0x27`(method 0x000E), `0x22`=34 (geometry), `0x07`=7 (guiding),
`0x1d`=29 (distance).

The **maneuver-list model[32] (method `0x000C`)** lists the description models in order and **reorders them as you
progress**: captured instances were `[0f00,0f01,0f02] → [0f01,0f02,0f00] → [0f02,0f00,…]`. **The first entry is the
current maneuver, the second is the "then…" (secondary) maneuver the cluster previews.**

carplay-kombi keeps only maneuver index 0 (`kombi_graph.cpp:389`) and never rebuilds the `0x000C` list, so the
secondary arrow stays exactly as captured in the template → a **stale, unchanging arrow** (the reported bug).

**Fix path:** carplay-kombi must own the `0x000C` maneuver list. Minimum (blank the stale arrow): emit a list with only
the current maneuver. Full (#2): also build a maneuver index-1 (description[15] + geometry[34]) from the **next**
maneuver (the shim already parses it and logs `NEXT`), and put it second in the list. The geometry model[34] holds f64
values = the arrow shape; the description references it.

## Lanes / junction graphic

The factory can draw a full lane/junction diagram on the cluster (observed on-car). In the captured drive there was
**no LANE model** (no lane junction was driven), so the exact lane-model format is still to be captured. kombi expects
lane models on method **`0x0010`**, but the factory stream here used `0x000B`/`0x000C` for the maneuver-list/guiding —
**to confirm the lane method and format, capture a stock drive through a junction that shows lanes** (`stocknav_log.py
--capture`) and decode the `0x000B`/`0x0010` models.

## Tools

- `someip_decode.py <pcap> --to-only` — reassembles the TCP stream to the cluster and lists the models.
- `python someip_decode.py <pcap> --to-only --payload | grep "svc=0xb006 meth=0x000c"` — the maneuver list over time.

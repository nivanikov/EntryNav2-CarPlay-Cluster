# iAP2 maneuver decode — interoperability notes

Reverse-engineering notes on how the stock head unit turns an Apple iAP2 maneuver (from the iPhone over CarPlay) into
BMW cluster guidance. This is interoperability analysis only; no proprietary binary is included in this repository.

## ManeuverUpdate — field layout (key for the shim)

The stock maneuver converter reads a maneuver as:
- **`maneuverType` = u32 at offset `+0x30`**
- **valid flag (byte) at `+0x34`** — if 0, the event is dropped ("Invalid maneuverType")
- `maneuverType` domain is **0..53** (`cmp [+0x30],0x35; ja default`)
- an extra sub-field for some types is read at `+0x98`

This matches the EntryNav2 Cinemo `ManeuverUpdate::store` = memcpy of 68 bytes (the type field lives inside the POD).
The shim reads the type at `+0x30` and validity at `+0x34`.

## 54 Apple types → 20 handlers → 6 builder functions

`maneuverType` indexes a jump table of 54 int32 entries; a builder is then called with a direction code.

| builder | role | Apple maneuverType → code |
|---|---|---|
| **A** | main turn / junction, code = direction 1..8 | 0,3,5,6,7,11→**1**; 1,20→**2**; 49→**3**; 47→**4**; 2,21→**5**; 50→**6**; 48→**7**; 4,18,26→**8** (reads +0x98); 51→**2** |
| **B** | event variant, code 1..3 | 24→**1**; 10,12,27→**2**; 25→**3** |
| **C** | conditional event (reads a byte, `cmovne`) | 13,52→**1**; 14,53→**2**; 22→(cond?6:1); 23→(cond?7:2); 8→{fields 7,6} |
| **D** | standalone type, code = raw | 15,16,17 |
| **E** | catch-all extended types (ADAS / cameras / school zones — NOT turn arrows) | 19, 28..46 (20 types) |
| **F** → C | compound (roundabout-like: fields {4,3}) | 9 |

Types 28–46 (group E) are SpeedLimitAssist / SpeedCamera / DangerousCurve / SchoolZone style informational events, not
turns.

## Direction enum (for lanes / logs)

`UTURN_LEFT, HARD_LEFT, SLIGHT_LEFT, STRAIGHT_ON, SLIGHT_RIGHT, HARD_RIGHT, UTURN_RIGHT` (plus "Unknown"). Lane markers:
`DASHED / SOLID`.

## Route-guidance identification

The head unit declares its Route Guidance component through the Cinemo metapool machinery, with meta-names such as
`COMPONENT_NAME, MAX_GUIDANCE_MANEUVER_CAPACITY, MAX_MANEUVER_DESCRIPTION_LENGTH, MAX_AFTER_MANEUVER_ROAD_NAME_LENGTH,
MAX_CURRENT_ROAD_NAME_LENGTH, MAX_DESTINATION_NAME_LENGTH, MAX_LANE_GUIDANCE_DESCRIPTION_LENGTH,
MAX_LANE_GUIDANCE_STORAGE_CAPACITY`. The capacities/lengths come from iAP2-layer defaults; specification values
(capacity 8–16, string lengths 64–128) are enough. The key point: the component must be *declared* — which the stock
CARPLAY on EntryNav2 does not do, so the shim declares it.

## What this gives the shim

1. Read `maneuverType` at `ManeuverUpdate+0x30` (valid at `+0x34`).
2. Collapse the 54 Apple types into groups (turns → direction, roundabout, ignore the informational E group), then map
   to the cluster datagram types.
3. Declare the Route Guidance component in identification so the phone sends guidance at all.

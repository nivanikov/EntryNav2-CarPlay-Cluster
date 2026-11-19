/* iap2_rgd.h -- portable iAP2 Route Guidance TLV builder/parser for the EntryNav2 CarPlay->cluster shim.
 *
 * Pure, dependency-free C (freestanding-friendly: only string.h + stdint.h). The SAME file is compiled into:
 *   (1) the host unit-test harness (test_iap2_rgd.c) -- "verify without the car", and
 *   (2) the ARM LD_PRELOAD shim (NmeIAP2Message::Encode/Decode hooks) -- so the wire bytes are identical.
 *
 * Layout facts (verified against kamgurgul/mib2q-carplay rgd_tlv.c + iap2_protocol.c, Audi MHI2Q = same Cinemo stack):
 *   iAP2 session (control) message = SYNC 0x40 0x40 | u16 BE total_len(=6+payload) | u16 BE msgid | params(TLVs).
 *   TLV = u16 BE len(=4+value_len, i.e. INCLUDES the 4-byte header) | u16 BE id | value_len bytes.
 *   RouteGuidanceDisplayComponent is iAP2 Identification TLV id 0x001E, containing inner TLVs id 0x0000..0x0008.
 */
#ifndef IAP2_RGD_H
#define IAP2_RGD_H

#include <stdint.h>
#include <stddef.h>

/* ---- message ids ---- */
#define IAP2_MSG_IDENTIFY            0x1D01u  /* IdentificationInformation (accessory -> device) */
#define IAP2_MSG_IDENTIFY_ACCEPTED   0x1D02u
#define IAP2_MSG_IDENTIFY_REJECTED   0x1D03u
#define IAP2_MSG_AUTH_SUCCEEDED      0xAA05u
#define IAP2_MSG_RG_START            0x5200u  /* StartRouteGuidanceUpdates (sent) */
#define IAP2_MSG_RG_UPDATE           0x5201u  /* RouteGuidanceUpdate (received) */
#define IAP2_MSG_RG_MANEUVER         0x5202u  /* RouteGuidanceManeuverUpdate (received) */
#define IAP2_MSG_RG_STOP             0x5203u  /* StopRouteGuidanceUpdates (sent) */
#define IAP2_MSG_RG_LANE             0x5204u  /* LaneGuidanceInformation (received) */

/* ---- Identification component ---- */
#define IDENT_TLV_RG_COMPONENT       0x001Eu

/* RouteGuidanceDisplayComponent inner TLV ids (order matters; iOS rejects the group silently if shifted) */
#define RGD_COMP_COMPONENT_ID              0x0000u
#define RGD_COMP_NAME                      0x0001u
#define RGD_COMP_MAX_CURRENT_ROAD_NAME     0x0002u
#define RGD_COMP_MAX_DEST_NAME             0x0003u
#define RGD_COMP_MAX_AFTER_MANEUVER_NAME   0x0004u
#define RGD_COMP_MAX_MANEUVER_DESC         0x0005u
#define RGD_COMP_MAX_MANEUVER_CAPACITY     0x0006u
#define RGD_COMP_MAX_LANE_GUIDANCE_DESC    0x0007u
#define RGD_COMP_MAX_LANE_GUIDANCE_CAPACITY 0x0008u

#define RGD_COMPONENT_NAME   "RouteGuidanceDisplayComponent"
#define RGD_DEFAULT_COMP_ID  0x0F0Eu   /* unique; avoids the known 0x0010 collision (ftojke/mib2q) */

/* ---- 0x5200 StartRouteGuidanceUpdates TLV ids ---- */
#define RGD_START_TLV_COMPONENT_ID        0x0000u
#define RGD_START_TLV_SOURCE_NAME         0x0001u  /* empty, presence-only */
#define RGD_START_TLV_SOURCE_SUPPORTS_RG  0x0002u  /* empty */
#define RGD_START_TLV_SUPPORTS_EXIT_INFO  0x0003u  /* empty */

/* ---- parsed receive structures ---- */
#define RG_ROAD_MAX 128
#define RG_MAX_LANES 8
#define RG_MAX_ANGLES 8

typedef struct {
    int      have;
    uint8_t  route_state;      /* 0x01 RouteGuidanceState (0..6) */
    uint8_t  maneuver_state;   /* 0x02 */
    char     road[RG_ROAD_MAX];/* 0x03 CurrentRoadName */
    char     dest[RG_ROAD_MAX];/* 0x04 DestinationName */
    uint32_t dist_to_maneuver; /* 0x0A */
    uint8_t  dist_units;       /* 0x0C DistToManeuverUnits */
    uint16_t maneuver_count;   /* 0x0E */
    uint16_t cur_maneuver_idx; /* first of 0x0D ManeuverList (current) */
    char     source[64];       /* 0x13 SourceName */
} rg_update_t;

typedef struct {
    int      have;
    uint16_t index;            /* 0x01 */
    char     desc[RG_ROAD_MAX];/* 0x02 Description */
    uint8_t  type;             /* 0x03 maneuver type 0..53 */
    char     after_road[RG_ROAD_MAX]; /* 0x04 */
    uint32_t dist_between;     /* 0x05 DistanceBetween */
    uint8_t  dist_units;       /* 0x07 */
    uint8_t  driving_side;     /* 0x08 (0 right, 1 left) */
    uint8_t  junction_type;    /* 0x09 (0 intersection, 1 roundabout) */
    int      angle_n;          /* 0x0A JunctionAngles count */
    int16_t  angles[RG_MAX_ANGLES];
    int16_t  exit_angle;       /* 0x0B (1000 = absent/unknown) */
} rg_maneuver_t;

typedef struct { uint16_t position; uint8_t status; int16_t highlight; int ang_n; int16_t ang[RG_MAX_ANGLES]; } rg_lane_t;
typedef struct { int have; uint16_t index; int n; rg_lane_t lanes[RG_MAX_LANES]; } rg_laneinfo_t;

/* ---- byte helpers ---- */
static inline void     wbe16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static inline void     wbe32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static inline uint16_t rbe16(const uint8_t* p){ return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t rbe32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* ---- TLV builders (return bytes written, or 0 on overflow) ---- */
int iap2_tlv(uint8_t* out, size_t cap, uint16_t id, const void* val, size_t vlen);
int iap2_tlv_u16(uint8_t* out, size_t cap, uint16_t id, uint16_t v);
int iap2_tlv_str(uint8_t* out, size_t cap, uint16_t id, const char* s); /* includes NUL */

/* Build the 0x001E RouteGuidanceDisplayComponent TLV. Returns total bytes (outer len field = this value). */
int rgd_build_component(uint8_t* out, size_t cap, uint16_t comp_id);

/* Identification message-id list TLVs (verified on EntryNav2): 0x0006 = MessagesSentByAccessory,
 * 0x0007 = MessagesReceivedFromDevice; each value is a list of u16 BE message ids. */
#define IDENT_TLV_MSGS_SENT      0x0006u
#define IDENT_TLV_MSGS_RECEIVED  0x0007u

/* Rebuild an iAP2 Identification (0x1D01) message DECLARING Route Guidance: append 0x5200/0x5203 to the
 * MessagesSentByAccessory list (0x0006), append 0x5201/0x5202/0x5204 to MessagesReceivedFromDevice (0x0007), and add
 * the 0x001E component if absent -- all dup-skipped. Writes the new message to dst and fixes its total-length field.
 * Returns the new total length, or 0 on error. src..src+n is the stock Identify (starting at its 40 40 header). */
int rebuild_identify_with_rgd(uint8_t* dst, size_t cap, const uint8_t* src, size_t n, uint16_t comp_id);

/* Build a complete session frame (40 40 | len | msgid | payload). Returns total bytes. */
int iap2_build_frame(uint8_t* out, size_t cap, uint16_t msgid, const uint8_t* payload, size_t plen);

/* Wrap a session message in an iAP2 FF5A LINK frame with the given control/seq/ack/session, computing both the 9-byte
 * header checksum (byte[8]) and the trailing payload checksum. Layout: ff 5a | len16 | ctrl | seq | ack | session |
 * hdr-cksum | <session bytes> | payload-cksum. Returns total length, or 0 on overflow. */
int iap2_link_frame(uint8_t* out, size_t cap, uint8_t ctrl, uint8_t seq, uint8_t ack, uint8_t session,
                    const uint8_t* sess, size_t slen);
/* Build the 0x5200 and 0x5203 frames. */
int rgd_build_start_frame(uint8_t* out, size_t cap, uint16_t comp_id);
int rgd_build_stop_frame(uint8_t* out, size_t cap, uint16_t comp_id);

/* Validate + split a session frame: checks 40 40 + length, sets *msgid, *payload, *plen. Returns 1 ok, 0 bad. */
int iap2_frame_parse(const uint8_t* buf, size_t n, uint16_t* msgid, const uint8_t** payload, size_t* plen);

/* TLV iterator: find id in a params buffer; returns pointer to value + sets *vlen, or NULL. */
const uint8_t* iap2_tlv_find(const uint8_t* params, size_t n, uint16_t id, size_t* vlen);

/* Parsers (payload = params after the 6-byte 40 40 header). Return 1 ok, 0 malformed. */
int rgd_parse_update(const uint8_t* params, size_t n, rg_update_t* out);
int rgd_parse_maneuver(const uint8_t* params, size_t n, rg_maneuver_t* out);
int rgd_parse_lane(const uint8_t* params, size_t n, rg_laneinfo_t* out);

/* Apple maneuver type (0..53) + driving side + angle -> BMW cluster direction code (kombi.h table). */
int rgd_maneuver_to_cluster_dir(uint8_t type, int angle, uint8_t side);

#endif

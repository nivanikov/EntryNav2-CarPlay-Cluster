/* iap2_rgd.c -- implementation of the portable iAP2 Route Guidance TLV builder/parser. See iap2_rgd.h.
 * Dependency-free (string.h only). Compiled into BOTH the host unit tests and the ARM shim. */
#include "iap2_rgd.h"
#include <string.h>

/* ---------- TLV builders ---------- */
int iap2_tlv(uint8_t* out, size_t cap, uint16_t id, const void* val, size_t vlen) {
    size_t tlen = 4 + vlen;                 /* length field INCLUDES the 4-byte header */
    if (tlen > 0xffff || tlen > cap) return 0;
    wbe16(out, (uint16_t)tlen);
    wbe16(out + 2, id);
    if (vlen) memcpy(out + 4, val, vlen);
    return (int)tlen;
}
int iap2_tlv_u16(uint8_t* out, size_t cap, uint16_t id, uint16_t v) {
    uint8_t b[2]; wbe16(b, v);
    return iap2_tlv(out, cap, id, b, 2);
}
int iap2_tlv_str(uint8_t* out, size_t cap, uint16_t id, const char* s) {
    return iap2_tlv(out, cap, id, s, strlen(s) + 1);   /* include NUL */
}

/* ---------- RouteGuidanceDisplayComponent (Identification TLV 0x001E) ----------
 * Inner TLVs id 0x0000..0x0008 in EXACT order; a shifted id or an extra TLV makes iOS accept identify but silently
 * stop sending data (kamgurgul rgd_tlv.c note). Values: comp_id; name "RouteGuidanceDisplayComponent"+NUL;
 * 0x0100 for the four name-length caps (0x02-0x05); 0x0100 lane-desc (0x07); 6 maneuver capacity (0x06); 6 lane
 * capacity (0x08). */
int rgd_build_component(uint8_t* out, size_t cap, uint16_t comp_id) {
    uint8_t inner[256];
    int o = 0, r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_COMPONENT_ID, comp_id); if (!r) return 0; o += r;
    r = iap2_tlv_str(inner + o, sizeof(inner) - o, RGD_COMP_NAME, RGD_COMPONENT_NAME); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_CURRENT_ROAD_NAME,      0x0100); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_DEST_NAME,              0x0100); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_AFTER_MANEUVER_NAME,    0x0100); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_MANEUVER_DESC,          0x0100); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_MANEUVER_CAPACITY,      0x0006); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_LANE_GUIDANCE_DESC,     0x0100); if (!r) return 0; o += r;
    r = iap2_tlv_u16(inner + o, sizeof(inner) - o, RGD_COMP_MAX_LANE_GUIDANCE_CAPACITY, 0x0006); if (!r) return 0; o += r;
    return iap2_tlv(out, cap, IDENT_TLV_RG_COMPONENT, inner, (size_t)o);   /* outer len field = 4 + o */
}

/* ---------- Identification rebuild (declare RG message-id lists + component) ---------- */
static int append_ids_tlv(uint8_t* out, size_t cap, uint16_t id, const uint8_t* oldv, size_t oldvl,
                          const uint16_t* add, int naddl) {
    uint8_t val[256]; size_t vl = 0;
    if (oldvl) { if (oldvl > sizeof(val)) return 0; memcpy(val, oldv, oldvl); vl = oldvl; }
    for (int i = 0; i < naddl; i++) {
        int present = 0;
        for (size_t k = 0; k + 2 <= oldvl; k += 2) if (rbe16(oldv + k) == add[i]) { present = 1; break; }
        if (!present && vl + 2 <= sizeof(val)) { wbe16(val + vl, add[i]); vl += 2; }
    }
    return iap2_tlv(out, cap, id, val, vl);
}

int rebuild_identify_with_rgd(uint8_t* dst, size_t cap, const uint8_t* src, size_t n, uint16_t comp_id) {
    static const uint16_t SENT[] = { IAP2_MSG_RG_START, IAP2_MSG_RG_STOP };                 /* 0x5200, 0x5203 */
    static const uint16_t RECV[] = { IAP2_MSG_RG_UPDATE, IAP2_MSG_RG_MANEUVER, IAP2_MSG_RG_LANE }; /* 0x5201/2/4 */
    if (n < 6 || cap < 6) return 0;
    memcpy(dst, src, 6);                                     /* 40 40 | len16 | msgid16 */
    int o = 6, have_comp = 0;
    const uint8_t* p = src + 6; size_t rem = n - 6;
    while (rem >= 4) {
        size_t tlen = rbe16(p); if (tlen < 4 || tlen > rem) break;
        uint16_t id = rbe16(p + 2); const uint8_t* v = p + 4; size_t vl = tlen - 4;
        int w;
        if (id == IDENT_TLV_MSGS_SENT)          w = append_ids_tlv(dst + o, cap - (size_t)o, id, v, vl, SENT, 2);
        else if (id == IDENT_TLV_MSGS_RECEIVED) w = append_ids_tlv(dst + o, cap - (size_t)o, id, v, vl, RECV, 3);
        else { if (id == IDENT_TLV_RG_COMPONENT) have_comp = 1;
               if ((size_t)o + tlen > cap) return 0; memcpy(dst + o, p, tlen); w = (int)tlen; }
        if (w <= 0) return 0;
        o += w; p += tlen; rem -= tlen;
    }
    if (!have_comp) { int w = rgd_build_component(dst + o, cap - (size_t)o, comp_id); if (w <= 0) return 0; o += w; }
    wbe16(dst + 2, (uint16_t)o);                             /* fix total message length */
    return o;
}

/* ---------- frames ---------- */
int iap2_build_frame(uint8_t* out, size_t cap, uint16_t msgid, const uint8_t* payload, size_t plen) {
    size_t total = 6 + plen;
    if (total > 0xffff || total > cap) return 0;
    out[0] = 0x40; out[1] = 0x40;
    wbe16(out + 2, (uint16_t)total);
    wbe16(out + 4, msgid);
    if (plen) memcpy(out + 6, payload, plen);
    return (int)total;
}
int iap2_link_frame(uint8_t* out, size_t cap, uint8_t ctrl, uint8_t seq, uint8_t ack, uint8_t session,
                    const uint8_t* sess, size_t slen) {
    size_t total = 9 + slen + 1;                 /* 9-byte link header + session bytes + 1 payload checksum */
    if (total > 0xffff || total > cap) return 0;
    out[0] = 0xff; out[1] = 0x5a;
    wbe16(out + 2, (uint16_t)total);
    out[4] = ctrl; out[5] = seq; out[6] = ack; out[7] = session;
    unsigned s = 0; for (int i = 0; i < 8; i++) s += out[i];
    out[8] = (uint8_t)(0x100u - (s & 0xff));     /* header checksum over bytes [0..7] */
    if (slen) memcpy(out + 9, sess, slen);
    unsigned p = 0; for (size_t i = 0; i < slen; i++) p += sess[i];
    out[9 + slen] = (uint8_t)(0x100u - (p & 0xff)); /* payload checksum over the session bytes */
    return (int)total;
}

int rgd_build_start_frame(uint8_t* out, size_t cap, uint16_t comp_id) {
    uint8_t p[32]; int o = 0, r;
    r = iap2_tlv_u16(p + o, sizeof(p) - o, RGD_START_TLV_COMPONENT_ID, comp_id); if (!r) return 0; o += r;
    r = iap2_tlv(p + o, sizeof(p) - o, RGD_START_TLV_SOURCE_NAME, 0, 0);         if (!r) return 0; o += r;
    r = iap2_tlv(p + o, sizeof(p) - o, RGD_START_TLV_SOURCE_SUPPORTS_RG, 0, 0);  if (!r) return 0; o += r;
    r = iap2_tlv(p + o, sizeof(p) - o, RGD_START_TLV_SUPPORTS_EXIT_INFO, 0, 0);  if (!r) return 0; o += r;
    return iap2_build_frame(out, cap, IAP2_MSG_RG_START, p, (size_t)o);
}
int rgd_build_stop_frame(uint8_t* out, size_t cap, uint16_t comp_id) {
    uint8_t p[8]; int o = iap2_tlv_u16(p, sizeof(p), RGD_START_TLV_COMPONENT_ID, comp_id);
    if (!o) return 0;
    return iap2_build_frame(out, cap, IAP2_MSG_RG_STOP, p, (size_t)o);
}

/* ---------- frame + TLV parse ---------- */
int iap2_frame_parse(const uint8_t* buf, size_t n, uint16_t* msgid, const uint8_t** payload, size_t* plen) {
    if (n < 6 || buf[0] != 0x40 || buf[1] != 0x40) return 0;
    size_t total = rbe16(buf + 2);
    if (total < 6 || total > n) return 0;
    *msgid = rbe16(buf + 4);
    *payload = buf + 6;
    *plen = total - 6;
    return 1;
}
const uint8_t* iap2_tlv_find(const uint8_t* p, size_t n, uint16_t id, size_t* vlen) {
    while (n >= 4) {
        size_t tlen = rbe16(p);
        if (tlen < 4 || tlen > n) break;
        if (rbe16(p + 2) == id) { if (vlen) *vlen = tlen - 4; return p + 4; }
        p += tlen; n -= tlen;
    }
    return 0;
}
static void cpstr(char* dst, size_t cap, const uint8_t* v, size_t vlen) {
    size_t m = vlen < cap - 1 ? vlen : cap - 1;
    size_t i = 0;
    for (; i < m && v[i]; i++) dst[i] = (char)v[i];
    dst[i] = 0;
}

int rgd_parse_update(const uint8_t* p, size_t n, rg_update_t* out) {
    memset(out, 0, sizeof(*out));
    int walked = 0;
    while (n >= 4) {
        size_t tlen = rbe16(p); if (tlen < 4 || tlen > n) return 0;
        uint16_t id = rbe16(p + 2); const uint8_t* v = p + 4; size_t vl = tlen - 4;
        switch (id) {
            case 0x0001: if (vl >= 1) out->route_state = v[0]; break;
            case 0x0002: if (vl >= 1) out->maneuver_state = v[0]; break;
            case 0x0003: cpstr(out->road, sizeof(out->road), v, vl); break;
            case 0x0004: cpstr(out->dest, sizeof(out->dest), v, vl); break;
            case 0x000A: if (vl >= 4) out->dist_to_maneuver = rbe32(v); break;
            case 0x000C: if (vl >= 1) out->dist_units = v[0]; break;
            case 0x000D: if (vl >= 2) out->cur_maneuver_idx = rbe16(v); break;
            case 0x000E: if (vl >= 2) out->maneuver_count = rbe16(v); break;
            case 0x0013: cpstr(out->source, sizeof(out->source), v, vl); break;
            default: break;
        }
        p += tlen; n -= tlen; walked = 1;
    }
    out->have = walked;
    return walked;
}

int rgd_parse_maneuver(const uint8_t* p, size_t n, rg_maneuver_t* out) {
    memset(out, 0, sizeof(*out));
    int have_index = 0, have_exit = 0, walked = 0;
    while (n >= 4) {
        size_t tlen = rbe16(p); if (tlen < 4 || tlen > n) return 0;
        uint16_t id = rbe16(p + 2); const uint8_t* v = p + 4; size_t vl = tlen - 4;
        switch (id) {
            case 0x0001: if (vl >= 2) { out->index = rbe16(v); have_index = 1; } break;
            case 0x0002: cpstr(out->desc, sizeof(out->desc), v, vl); break;
            case 0x0003: if (vl >= 1) out->type = v[0]; break;
            case 0x0004: cpstr(out->after_road, sizeof(out->after_road), v, vl); break;
            case 0x0005: if (vl >= 4) out->dist_between = rbe32(v); break;
            case 0x0007: if (vl >= 1) out->dist_units = v[0]; break;
            case 0x0008: if (vl >= 1) out->driving_side = v[0]; break;
            case 0x0009: if (vl >= 1) out->junction_type = v[0]; break;
            case 0x000A: { size_t k = 0; while (k + 2 <= vl && out->angle_n < RG_MAX_ANGLES) {
                              out->angles[out->angle_n++] = (int16_t)rbe16(v + k); k += 2; } } break;
            case 0x000B: if (vl >= 2) { out->exit_angle = (int16_t)rbe16(v); have_exit = 1; } break;
            default: break;
        }
        p += tlen; n -= tlen; walked = 1;
    }
    if (have_index && !have_exit) out->exit_angle = 1000;   /* sentinel: unknown exit angle */
    out->have = walked;
    return walked;
}

int rgd_parse_lane(const uint8_t* p, size_t n, rg_laneinfo_t* out) {
    memset(out, 0, sizeof(*out));
    int walked = 0;
    while (n >= 4) {
        size_t tlen = rbe16(p); if (tlen < 4 || tlen > n) return 0;
        uint16_t id = rbe16(p + 2); const uint8_t* v = p + 4; size_t vl = tlen - 4;
        if (id == 0x0001) { if (vl >= 2) out->index = rbe16(v); else if (vl == 1) out->index = v[0]; }
        else if (id == 0x0002) {   /* LaneInformations: nested sub-TLVs */
            const uint8_t* q = v; size_t qn = vl; rg_lane_t* cur = 0;
            while (qn >= 4) {
                size_t sl = rbe16(q); if (sl < 4 || sl > qn) break;
                uint16_t sid = rbe16(q + 2); const uint8_t* sv = q + 4; size_t svl = sl - 4;
                if (sid == 0x0000) {   /* position -> start a new lane */
                    if (out->n < RG_MAX_LANES) { cur = &out->lanes[out->n++];
                        memset(cur, 0, sizeof(*cur)); cur->highlight = 1000;
                        if (svl >= 2) cur->position = rbe16(sv); }
                } else if (cur) {
                    if (sid == 0x0001 && svl >= 1) cur->status = sv[0];
                    else if (sid == 0x0002) { size_t k = 0; while (k + 2 <= svl && cur->ang_n < RG_MAX_ANGLES) {
                                  cur->ang[cur->ang_n++] = (int16_t)rbe16(sv + k); k += 2; } }
                    else if (sid == 0x0003 && svl >= 2) cur->highlight = (int16_t)rbe16(sv);
                }
                q += sl; qn -= sl;
            }
        }
        p += tlen; n -= tlen; walked = 1;
    }
    out->have = walked;
    return walked;
}

/* ---------- Apple maneuver type -> BMW cluster direction (NBT rgkombi kombi.h table) ---------- */
enum { kStraight=1,kHalfLeft=2,kHalfRight=3,kTightLeft=4,kTightRight=5,kLeft=6,kRight=7,
       kUturnLeft=8,kUturnRight=9,kDestination=10,kRoundaboutCw=14,kRoundaboutCcw=15,
       kHwyInLeft=25,kHwyInRight=26,kHwyOutLeft=28,kHwyOutRight=29,kLaneChangeLeft=32,kLaneChangeRight=33 };
int rgd_maneuver_to_cluster_dir(uint8_t type, int angle, uint8_t side) {
    int lht = (side == 1);
    int uturn = lht ? kUturnLeft : kUturnRight;
    int round = lht ? kRoundaboutCw : kRoundaboutCcw;
    int rampLeft = (angle != 0) ? (angle < 0) : lht;
    switch (type) {
        case 1: case 20: return kLeft;
        case 2: case 21: return kRight;
        case 4: case 18: case 26: return uturn;
        case 6: case 7: case 19: return round;
        case 8: return rampLeft ? kHwyOutLeft : kHwyOutRight;
        case 9: return rampLeft ? kHwyInLeft : kHwyInRight;
        case 10: case 12: case 24: case 25: case 27: return kDestination;
        case 13: case 49: return kHalfLeft;
        case 14: case 50: return kHalfRight;
        case 22: return kHwyOutLeft;
        case 23: return kHwyOutRight;
        case 47: return kTightLeft;
        case 48: return kTightRight;
        case 52: return kLaneChangeLeft;
        case 53: return kLaneChangeRight;
        default:
            if (type >= 28 && type <= 46) return round;   /* roundabout exit n = type-27 */
            return kStraight;                             /* 0,3,5,11,15-17, unknown */
    }
}

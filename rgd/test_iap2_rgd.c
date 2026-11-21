/* test_iap2_rgd.c -- host unit tests for iap2_rgd.c. Build on a PC (no car needed):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -o t iap2_rgd.c test_iap2_rgd.c && ./t
 * Exit 0 = all pass. Validates the exact byte layout of the 0x001E component and 0x5200 frame against the
 * kamgurgul/mib2q documented layout, round-trips the 0x5201/0x5202/0x5204 parsers, and fuzzes malformed input. */
#include "iap2_rgd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

/* ---- component 0x001E ---- */
static void test_component(void) {
    uint8_t buf[256];
    int len = rgd_build_component(buf, sizeof(buf), 0x0F0E);
    /* inner = 6 (id) + (4+30 name) + 7*6 (u16 caps) = 6+34+42 = 82; outer = 4+82 = 86 */
    CHECK(len == 86, "component total bytes == 86");
    CHECK(rbe16(buf + 0) == 86, "outer len field == 0x0056");
    CHECK(rbe16(buf + 2) == 0x001E, "outer id == 0x001E");
    const uint8_t* inner = buf + 4; size_t iln = (size_t)len - 4;
    size_t vl;
    const uint8_t* v;
    v = iap2_tlv_find(inner, iln, 0x0000, &vl); CHECK(v && vl == 2 && rbe16(v) == 0x0F0E, "inner 0x0000 comp_id");
    v = iap2_tlv_find(inner, iln, 0x0001, &vl); CHECK(v && vl == 30 && memcmp(v, "RouteGuidanceDisplayComponent", 29) == 0 && v[29] == 0, "inner 0x0001 name+NUL");
    v = iap2_tlv_find(inner, iln, 0x0002, &vl); CHECK(v && vl == 2 && rbe16(v) == 0x0100, "0x0002 max cur road = 0x100");
    v = iap2_tlv_find(inner, iln, 0x0005, &vl); CHECK(v && rbe16(v) == 0x0100, "0x0005 max maneuver desc = 0x100");
    v = iap2_tlv_find(inner, iln, 0x0006, &vl); CHECK(v && rbe16(v) == 0x0006, "0x0006 maneuver capacity = 6");
    v = iap2_tlv_find(inner, iln, 0x0007, &vl); CHECK(v && rbe16(v) == 0x0100, "0x0007 lane desc = 0x100");
    v = iap2_tlv_find(inner, iln, 0x0008, &vl); CHECK(v && rbe16(v) == 0x0006, "0x0008 lane capacity = 6");
    /* no spurious ids 0x0009+ */
    v = iap2_tlv_find(inner, iln, 0x0009, &vl); CHECK(v == 0, "no inner 0x0009 (would break lane guidance)");
    /* exactly 9 inner TLVs */
    int count = 0; const uint8_t* p = inner; size_t n = iln;
    while (n >= 4) { size_t t = rbe16(p); if (t < 4 || t > n) break; count++; p += t; n -= t; }
    CHECK(count == 9, "exactly 9 inner TLVs");
}

/* ---- 0x5200 start frame ---- */
static void test_start_frame(void) {
    uint8_t buf[64];
    int len = rgd_build_start_frame(buf, sizeof(buf), 0x0F0E);
    /* payload = 6 (comp u16) + 4 + 4 + 4 = 18; total = 6 + 18 = 24 */
    CHECK(len == 24, "start frame total == 24");
    CHECK(buf[0] == 0x40 && buf[1] == 0x40, "start frame sync 40 40");
    CHECK(rbe16(buf + 2) == 24, "start frame len == 24");
    CHECK(rbe16(buf + 4) == 0x5200, "start frame msgid == 0x5200");
    uint16_t mid; const uint8_t* pl; size_t pn;
    CHECK(iap2_frame_parse(buf, (size_t)len, &mid, &pl, &pn) == 1 && mid == 0x5200 && pn == 18, "start frame parses back");
    size_t vl; const uint8_t* v = iap2_tlv_find(pl, pn, 0x0000, &vl);
    CHECK(v && vl == 2 && rbe16(v) == 0x0F0E, "start comp id present");
    v = iap2_tlv_find(pl, pn, 0x0001, &vl); CHECK(v && vl == 0, "start 0x0001 empty (presence)");
    v = iap2_tlv_find(pl, pn, 0x0003, &vl); CHECK(v && vl == 0, "start 0x0003 empty (exit info)");
}

/* ---- 0x5202 maneuver round-trip ---- */
static void test_maneuver(void) {
    uint8_t pl[128]; int o = 0;
    o += iap2_tlv_u16(pl + o, sizeof(pl) - o, 0x0001, 3);              /* index */
    o += iap2_tlv_str(pl + o, sizeof(pl) - o, 0x0002, "Turn right");   /* description */
    { uint8_t t = 2; o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0003, &t, 1); }  /* type 2 = RIGHT_TURN */
    o += iap2_tlv_str(pl + o, sizeof(pl) - o, 0x0004, "Main St");       /* after road */
    { uint8_t d[4]; wbe32(d, 120); o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0005, d, 4); }  /* 120 m */
    { uint8_t u = 2; o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0007, &u, 1); }  /* units = meters */
    { uint8_t s = 0; o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0008, &s, 1); }  /* right-hand */
    /* no 0x000B exit angle -> parser should fill 1000 since index present */
    uint8_t frame[160]; int fl = iap2_build_frame(frame, sizeof(frame), 0x5202, pl, (size_t)o);
    CHECK(fl > 0, "maneuver frame built");
    uint16_t mid; const uint8_t* fp; size_t fn;
    CHECK(iap2_frame_parse(frame, (size_t)fl, &mid, &fp, &fn) && mid == 0x5202, "maneuver frame parses");
    rg_maneuver_t m;
    CHECK(rgd_parse_maneuver(fp, fn, &m) == 1, "maneuver parse ok");
    CHECK(m.index == 3, "maneuver index 3");
    CHECK(m.type == 2, "maneuver type 2");
    CHECK(strcmp(m.desc, "Turn right") == 0, "maneuver desc");
    CHECK(strcmp(m.after_road, "Main St") == 0, "maneuver after road");
    CHECK(m.dist_between == 120, "maneuver distance 120");
    CHECK(m.driving_side == 0, "maneuver side right");
    CHECK(m.exit_angle == 1000, "maneuver exit angle defaulted to 1000");
    { int e; CHECK(rgd_apple_to_aa_type(m.type, m.driving_side, &e) == 8, "apple RightTurn(2) -> AA turn-right(8)"); }
}

/* ---- 0x5201 update round-trip ---- */
static void test_update(void) {
    uint8_t pl[128]; int o = 0;
    { uint8_t st = 1; o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0001, &st, 1); }  /* route set */
    o += iap2_tlv_str(pl + o, sizeof(pl) - o, 0x0003, "Central Blvd");
    { uint8_t d[4]; wbe32(d, 40); o += iap2_tlv(pl + o, sizeof(pl) - o, 0x000A, d, 4); }  /* 40 m to maneuver */
    { uint8_t u = 2; o += iap2_tlv(pl + o, sizeof(pl) - o, 0x000C, &u, 1); }
    rg_update_t u;
    CHECK(rgd_parse_update(pl, (size_t)o, &u) == 1, "update parse ok");
    CHECK(u.route_state == 1, "update route_state 1");
    CHECK(strcmp(u.road, "Central Blvd") == 0, "update road");
    CHECK(u.dist_to_maneuver == 40, "update dist 40");
    CHECK(u.dist_units == 2, "update units meters");
}

/* ---- 0x5204 lane round-trip ---- */
static void test_lane(void) {
    uint8_t sub[64]; int so = 0;
    { uint8_t pos[2]; wbe16(pos, 0); so += iap2_tlv(sub + so, sizeof(sub) - so, 0x0000, pos, 2); }  /* lane 0 */
    { uint8_t st = 2; so += iap2_tlv(sub + so, sizeof(sub) - so, 0x0001, &st, 1); }  /* preferred */
    { uint8_t pos[2]; wbe16(pos, 1); so += iap2_tlv(sub + so, sizeof(sub) - so, 0x0000, pos, 2); }  /* lane 1 */
    { uint8_t st = 0; so += iap2_tlv(sub + so, sizeof(sub) - so, 0x0001, &st, 1); }  /* not good */
    uint8_t pl[128]; int o = 0;
    { uint8_t idx[2]; wbe16(idx, 5); o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0001, idx, 2); }
    o += iap2_tlv(pl + o, sizeof(pl) - o, 0x0002, sub, (size_t)so);
    rg_laneinfo_t li;
    CHECK(rgd_parse_lane(pl, (size_t)o, &li) == 1, "lane parse ok");
    CHECK(li.index == 5, "lane index 5");
    CHECK(li.n == 2, "two lanes");
    CHECK(li.lanes[0].status == 2 && li.lanes[1].status == 0, "lane statuses");
    CHECK(li.lanes[0].highlight == 1000, "lane highlight default 1000");
}

/* ---- malformed / fuzz: must never crash (ASan/UBSan) and must reject cleanly ---- */
static void test_malformed(void) {
    rg_maneuver_t m; rg_update_t u; rg_laneinfo_t li; uint16_t mid; const uint8_t* p; size_t n;
    /* bad sync */
    uint8_t bad[8] = { 0x00, 0x00, 0, 8, 0x52, 0x02, 0, 0 };
    CHECK(iap2_frame_parse(bad, 8, &mid, &p, &n) == 0, "reject bad sync");
    /* length larger than buffer */
    uint8_t big[8] = { 0x40, 0x40, 0xff, 0xff, 0x52, 0x02, 0, 0 };
    CHECK(iap2_frame_parse(big, 8, &mid, &p, &n) == 0, "reject oversized length");
    /* TLV length < 4 */
    uint8_t t0[6] = { 0x00, 0x02, 0x00, 0x03, 0x05, 0x00 };
    CHECK(rgd_parse_maneuver(t0, 6, &m) == 0, "reject tlv len<4");
    /* TLV length > remaining */
    uint8_t t1[6] = { 0x00, 0x20, 0x00, 0x03, 0x05, 0x00 };
    CHECK(rgd_parse_maneuver(t1, 6, &m) == 0, "reject tlv len>buf");
    /* truncated / every prefix of a valid maneuver payload must not crash */
    uint8_t full[64]; int o = 0;
    o += iap2_tlv_u16(full + o, sizeof(full) - o, 0x0001, 1);
    { uint8_t t = 28; o += iap2_tlv(full + o, sizeof(full) - o, 0x0003, &t, 1); }
    { uint8_t a[2]; wbe16(a, 90); o += iap2_tlv(full + o, sizeof(full) - o, 0x000A, a, 2); }
    for (int k = 0; k <= o; k++) { (void)rgd_parse_maneuver(full, (size_t)k, &m);
                                   (void)rgd_parse_update(full, (size_t)k, &u);
                                   (void)rgd_parse_lane(full, (size_t)k, &li); }
    /* random fuzz */
    srand(1234);
    for (int it = 0; it < 20000; it++) {
        uint8_t fz[48]; int fl = rand() % (int)sizeof(fz);
        for (int j = 0; j < fl; j++) fz[j] = (uint8_t)rand();
        (void)rgd_parse_maneuver(fz, (size_t)fl, &m);
        (void)rgd_parse_update(fz, (size_t)fl, &u);
        (void)rgd_parse_lane(fz, (size_t)fl, &li);
        if (fl >= 6) { fz[0] = 0x40; fz[1] = 0x40; (void)iap2_frame_parse(fz, (size_t)fl, &mid, &p, &n); }
    }
    CHECK(1, "fuzz 20000 iterations without crash");
}

/* ---- Apple CPManeuverType -> Android Auto type spot checks (what carplay-kombi's clusterDirection expects).
 * Each case is cross-checked against the symptoms seen on the car + the canonical CPManeuverType enum. ---- */
static void test_dirmap(void) {
    int e;
    CHECK(rgd_apple_to_aa_type(1, 0, &e) == 7,   "apple LeftTurn(1) -> AA turn-left(7)");
    CHECK(rgd_apple_to_aa_type(2, 0, &e) == 8,   "apple RightTurn(2) -> AA turn-right(8)");
    CHECK(rgd_apple_to_aa_type(4, 0, &e) == 11,  "apple UTurn(4) RHT -> AA uturn-left(11)");
    CHECK(rgd_apple_to_aa_type(4, 1, &e) == 12,  "apple UTurn(4) LHT -> AA uturn-right(12)");
    CHECK(rgd_apple_to_aa_type(12, 0, &e) == 39, "apple ArriveAtDestination(12) -> AA destination(39)");
    CHECK(rgd_apple_to_aa_type(10, 0, &e) == 39, "apple ArriveEndOfNavigation(10) -> AA destination(39)");
    CHECK(rgd_apple_to_aa_type(23, 0, &e) == 22, "apple HighwayOffRampRight(23) -> AA off-ramp-right(22)");
    CHECK(rgd_apple_to_aa_type(8, 0, &e) == 22,  "apple OffRamp(8) RHT -> AA off-ramp-right(22)");
    CHECK(rgd_apple_to_aa_type(9, 0, &e) == 13,  "apple OnRamp(9) RHT -> AA on-ramp-left(13)");
    CHECK(rgd_apple_to_aa_type(49, 0, &e) == 5,  "apple SlightLeft(49) -> AA slight-left(5)");
    CHECK(rgd_apple_to_aa_type(48, 0, &e) == 10, "apple SharpRight(48) -> AA sharp-right(10)");
    CHECK(rgd_apple_to_aa_type(6, 0, &e) == 30,  "apple EnterRoundabout(6) RHT -> AA roundabout CCW(30)");
    CHECK(rgd_apple_to_aa_type(30, 0, &e) == 45 && e == 3, "apple RoundaboutExit3(30) -> AA roundabout+exit 3");
    CHECK(rgd_apple_to_aa_type(3, 0, &e) == 36,  "apple StraightAhead(3) -> AA straight(36)");
    CHECK(rgd_apple_to_aa_type(99, 0, &e) == 36 && e == 0, "unknown -> AA straight(36), no exit");
}

/* ---- FF5A link frame: validate header + checksum against a REAL captured frame ---- */
static void test_linkframe(void) {
    /* Captured LocationInfo frame header: ff 5a 01 34 40 74 ec 01 d1 (seq 0x74). total 0x134 => session is 298 B. */
    uint8_t sess[298]; memset(sess, 0, sizeof(sess));
    sess[0] = 0x40; sess[1] = 0x40; wbe16(sess + 2, 298); wbe16(sess + 4, 0xFFFB);
    uint8_t fr[512];
    int fl = iap2_link_frame(fr, sizeof(fr), 0x40, 0x74, 0xec, 0x01, sess, sizeof(sess));
    CHECK(fl == 308, "link frame total == 0x0134 (308)");
    CHECK(fr[0] == 0xff && fr[1] == 0x5a, "link sync ff 5a");
    CHECK(rbe16(fr + 2) == 308, "link len field == 0x0134");
    CHECK(fr[4] == 0x40 && fr[5] == 0x74 && fr[6] == 0xec && fr[7] == 0x01, "link ctrl/seq/ack/session");
    CHECK(fr[8] == 0xd1, "link header checksum == 0xd1 (matches captured frame)");
    unsigned s = 0; for (size_t i = 0; i < sizeof(sess); i++) s += sess[i];
    CHECK(fr[308 - 1] == (uint8_t)(0x100u - (s & 0xff)), "link payload checksum");
    /* wrap our real 0x5200 and confirm it unwraps at +9 */
    uint8_t s5[64]; int sl = rgd_build_start_frame(s5, sizeof(s5), 0x0F0E);
    int fl2 = iap2_link_frame(fr, sizeof(fr), 0x40, 0x10, 0x20, 0x01, s5, (size_t)sl);
    CHECK(fl2 == 9 + sl + 1, "0x5200 link frame length");
    uint16_t mid; const uint8_t* pl; size_t pn;
    CHECK(iap2_frame_parse(fr + 9, (size_t)(fl2 - 9), &mid, &pl, &pn) && mid == 0x5200, "0x5200 link frame unwraps at +9");
}

/* ---- rebuild Identify: declare msg-id lists + component ---- */
static void test_rebuild_identify(void) {
    uint8_t body[256]; int o = 6;
    body[0] = 0x40; body[1] = 0x40; wbe16(body + 4, 0x1D01);
    o += iap2_tlv_str(body + o, sizeof(body) - o, 0x0000, "BMW");                      /* name */
    { uint8_t v[2]; wbe16(v, 0xFFFB); o += iap2_tlv(body + o, sizeof(body) - o, 0x0006, v, 2); }  /* sent={FFFB} */
    { uint8_t v[2]; wbe16(v, 0x5001); o += iap2_tlv(body + o, sizeof(body) - o, 0x0007, v, 2); }  /* recv={5001} */
    wbe16(body + 2, (uint16_t)o);
    uint8_t out[512];
    int ol = rebuild_identify_with_rgd(out, sizeof(out), body, (size_t)o, 0x0F0E);
    CHECK(ol > o, "rebuilt identify grew");
    CHECK(rbe16(out + 2) == ol, "rebuilt total length fixed");
    const uint8_t* pl = out + 6; size_t pn = (size_t)ol - 6, vl;
    const uint8_t* v6 = iap2_tlv_find(pl, pn, 0x0006, &vl);
    CHECK(v6 && vl == 6 && rbe16(v6) == 0xFFFB && rbe16(v6 + 2) == 0x5200 && rbe16(v6 + 4) == 0x5203, "sent list += 5200,5203");
    const uint8_t* v7 = iap2_tlv_find(pl, pn, 0x0007, &vl);
    CHECK(v7 && vl == 8 && rbe16(v7) == 0x5001 && rbe16(v7 + 2) == 0x5201 && rbe16(v7 + 4) == 0x5202 && rbe16(v7 + 6) == 0x5204, "recv list += 5201,5202,5204");
    CHECK(iap2_tlv_find(pl, pn, 0x001E, &vl) != 0, "component 0x001E added");
    /* idempotent: rebuilding the already-declared message adds nothing new */
    uint8_t out2[512]; int ol2 = rebuild_identify_with_rgd(out2, sizeof(out2), out, (size_t)ol, 0x0F0E);
    CHECK(ol2 == ol, "rebuild is idempotent (no dup ids)");
}

int main(void) {
    test_component();
    test_rebuild_identify();
    test_linkframe();
    test_start_frame();
    test_maneuver();
    test_update();
    test_lane();
    test_malformed();
    test_dirmap();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

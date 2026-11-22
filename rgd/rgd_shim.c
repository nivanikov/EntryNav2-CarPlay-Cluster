/* rgd_shim.c -- EntryNav2 CarPlay->cluster shim, v18 (declaration + NATIVE subscribe + repo retry state machine).
 *
 * Architecture (plaintext iAP2 message layer via stock Cinemo symbols):
 *   - hook NmeIAP2Message::Encode  -> patch the OUTGOING Identify (0x1D01): declare RG msg-id lists + 0x001E component.
 *   - hook NmeIAP2Message::Decode  -> catch INCOMING 0x5201/0x5202/0x5204 route guidance -> parse -> forward to cluster.
 *   - hook NmeTransport::Send      -> capture transport; arm the 0x5200 request on each outgoing LocationInfo (0xFFFB).
 *   - capture every ICinemoIAP at CinemoCreateIAP (proxy + real); pick the REAL one (vt[67]==libNmeSDK+0x1897c0).
 *   - WORKER THREAD: once declaration is accepted, call the native ICinemoIAP::StartRouteGuidanceUpdates (vtable slot
 *     67) on the real iap to emit 0x5200, retried with backoff until the first 0x52xx arrives.
 *
 * WHY NATIVE SUBSCRIBE (not a hand-built 0x5200 like kamgurgul/luka-dev/ftojke): those Audi/NBT impls push a hand-built
 * 0x5200 through a GENERIC ICinemoIAP_SendIAP2, which our EntryNav2 libNmeSDK does NOT export. Our only route-guidance
 * sender is the typed ICinemoIAP_StartRouteGuidanceUpdates. It crashed before ONLY because RGD was undeclared (the
 * guidance object serialiser hit a NULL field, libNmeVfs+0x149810). v17 proved the declaration is now ACCEPTED by iOS,
 * so the guidance object should build and the subscribe should survive. We lift the repos' ARCHITECTURE (declare +
 * actively request + retry/backoff + stop-on-520x + re-arm-on-Identify) and wrap it around our native call.
 *
 * SAFE BY DEFAULT: no flags -> read-only recon (never modifies a frame). Patch, inject and subscribe are each gated.
 * CRASH GUARD: the native subscribe is a generation-gated one-arm-per-marker call; if it segfaults, the restarted
 * CARPLAY reads the spent gen and refuses to retry (one crash, never a reboot loop). Bump CP_SUB_GEN to re-arm.
 *
 * Config (live via /var/local/cpshim.conf, hot-reloaded ~1x/s; or env):
 *   CP_ENC_RECON=1    log outgoing Identify (default ON)       CP_DEC_RECON handled inline
 *   CP_RGD_PATCH=1    declare RG lists + 0x001E in the Identify (v17; the accepted declaration)
 *   CP_FORWARD=1      forward parsed maneuvers as UDP to carplay-kombi (CP_UDP_PORT, default 42042)
 *   CP_COMP_ID=0xNNNN component id (default 0x0F0E)
 *   --- native subscribe (v18) ---
 *   CP_RGD_SUBSCRIBE=1  ACTIVE: call StartRouteGuidanceUpdates (needs PATCH active + ident-accept + auth)
 *   CP_SUB_GEN=N        crash-guard generation; must exceed the marker (/var/local/cpshim.gen) to arm
 *   CP_SUB_START_MS     worker warm-up before first attempt (default 6000)
 *   CP_SUB_FAST_MS 2000 / CP_SUB_FAST_N 5 / CP_SUB_SLOW_MS 30000   retry backoff (repo cadence)
 *   CP_SUB_POLL_MS 1000   worker tick
 *   CP_RGU_SLOT 67 / CP_RGU_IMPL_OFF 0x1897c0   vtable slot + expected concrete offset for iap_is_real
 *   CP_CTX 0            r2/ctx arg to StartRouteGuidanceUpdates (default 0)
 *   CP_REQUIRE_CHANNEL=1  only subscribe on an iap whose guidance channel (iap+0x80) is live (default ON)
 *   CP_NO_CALL=1       inspect the real iap + channel but do NOT call (gen not spent) -- dry run
 *   CP_FORCE_CALL=1    subscribe even if iap_is_real() rejects the iap (debug)
 *   --- retired ---
 *   CP_RGD_INJECT=1   raw 0xFFFB-replace inject (DEAD END: stalls the link; kept off, present for reference)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "iap2_rgd.h"

/* ============================ infra: logging, is_carplay, config ============================ */
static int g_logfd = -1;
static volatile int g_stop = 0;

static int is_carplay(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    int fd = open("/proc/self/comm", O_RDONLY);
    char c[32]; int n = fd >= 0 ? (int)read(fd, c, sizeof(c) - 1) : 0;
    if (fd >= 0) close(fd);
    if (n < 0) n = 0; c[n] = 0;
    cached = (strncmp(c, "CARPLAY", 7) == 0) ? 1 : 0;
    return cached;
}
static void shim_log(const char* fmt, ...) {
    if (g_logfd < 0) {
        const char* p = getenv("CP_SHIM_LOG"); if (!p || !*p) p = "/tmp/cpshim.log";
        g_logfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_logfd < 0) return;
    }
    char line[768]; struct timeval tv; gettimeofday(&tv, 0);
    int n = snprintf(line, sizeof(line), "[cpshim %ld.%03ld] ", (long)tv.tv_sec, (long)tv.tv_usec / 1000);
    va_list ap; va_start(ap, fmt); n += vsnprintf(line + n, sizeof(line) - n, fmt, ap); va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    if (write(g_logfd, line, n) < 0) { /* ignore */ }
}
static void hexdump(const char* tag, const unsigned char* b, int n) {
    char h[16 * 3 + 1]; int m = n < 16 ? n : 16, p = 0;
    for (int i = 0; i < m; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", b[i]);
    shim_log("%s n=%d [%s%s]", tag, n, h, n > 16 ? "..." : "");
}
/* full buffer dump, 32 bytes per line (for reading the whole Identify TLV structure). */
static void hexdump_full(const char* tag, const unsigned char* b, int n) {
    shim_log("%s FULL n=%d:", tag, n);
    for (int off = 0; off < n; off += 32) {
        char h[32 * 3 + 1]; int p = 0, e = off + 32 < n ? off + 32 : n;
        for (int i = off; i < e; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", b[i]);
        shim_log("  +%04x %s", off, h);
    }
}
static long now_ms(void){ struct timeval tv; gettimeofday(&tv, 0); return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }

/* tiny KEY=VALUE conf, mtime-gated hot reload */
static char g_cfg_path[256];
static char g_cfg_buf[2048];
static time_t g_cfg_mtime = 0;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static const char* cfg_path(void) {
    if (!g_cfg_path[0]) { const char* p = getenv("CP_CONF"); if (!p || !*p) p = "/var/local/cpshim.conf";
        strncpy(g_cfg_path, p, sizeof(g_cfg_path) - 1); }
    return g_cfg_path;
}
static void cfg_reload(void) {
    struct stat st; if (stat(cfg_path(), &st) != 0) return;
    if (st.st_mtime == g_cfg_mtime) return;
    int fd = open(cfg_path(), O_RDONLY); if (fd < 0) return;
    int n = (int)read(fd, g_cfg_buf, sizeof(g_cfg_buf) - 1); close(fd);
    if (n < 0) n = 0; g_cfg_buf[n] = 0; g_cfg_mtime = st.st_mtime;
}
/* returns value for KEY from conf, else env, else defv */
static int cfg_int(const char* key, int defv) {
    pthread_mutex_lock(&g_cfg_lock); cfg_reload();
    int ret = defv, found = 0; size_t kl = strlen(key); const char* p = g_cfg_buf;
    while (*p) {
        const char* nl = strchr(p, '\n'); size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') { ret = (int)strtol(p + kl + 1, 0, 0); found = 1; }
        if (!nl) break; p = nl + 1;
    }
    pthread_mutex_unlock(&g_cfg_lock);
    if (!found) { const char* e = getenv(key); if (e && *e) ret = (int)strtol(e, 0, 0); }
    return ret;
}
static int opt(const char* k, int d){ return cfg_int(k, d); }

/* ============================ real symbol resolution ============================ */
typedef int (*enc_fn)(void* self, void* arr);                               /* NmeIAP2Message::Encode(NmeArray&) const */
typedef int (*dec_fn)(void* self, const unsigned char* data, int len);      /* NmeIAP2Message::Decode(const uchar*, int) */
typedef int (*snd_fn)(void* self, const unsigned char* data, unsigned len, unsigned* out); /* NmeTransport::Send */
typedef int (*rcv_fn)(void* self, void* arr);                               /* NmeTransport::Recv(NmeArray&) */
static enc_fn real_encode;
static dec_fn real_decode;
static snd_fn real_send;
static rcv_fn real_recv;
static void* real_of(const char* nm) { return dlsym(RTLD_NEXT, nm); }

/* ============================ captured objects / state ============================ */
static void* g_iap = 0;          /* last ICinemoIAP* (any) -- debug only */
/* LIFETIME SAFETY (v22): we never iterate a cached array of iap pointers (Cinemo frees iaps per session; a later deref
 * of a freed one is a use-after-free -> the shim+0x1744 crash, cores 2732/4703). Instead we keep ONE pointer, g_real_iap,
 * and write it ONLY from CinemoCreateIAP where *out is freshly constructed (iap_is_real is safe there). The worker
 * derefs g_real_iap exactly once per session, right after auth, when it is the current live iap. On reconnect a new
 * capture overwrites g_real_iap with the fresh one, so a freed pointer is never touched again. */
static void* volatile g_real_iap = 0;
static void* g_transport = 0;    /* NmeTransport* self captured from Send */
static volatile int g_ident_accepted = 0;
static volatile int g_auth_done = 0;
static volatile int g_ident_patched = 0;
static int g_udp = -1;

/* subscribe / retry state (v18) */
static volatile int g_got_520x = 0;      /* first incoming 0x52xx seen -> stop requesting */
static volatile int g_loc_trigger = 0;   /* an outgoing 0xFFFB LocationInfo armed an immediate attempt */
static int g_sub_alive = 0;              /* the native call returned at least once (so retries are safe) */
static int g_sub_done = 0;               /* subscribed once this session -> stop polling (avoid stale-iap UAF) */
static int g_sub_attempts = 0;
static long g_sub_next_ms = 0;
/* anti-boot-loop crash guard (persistent install): a counter in /var/local/cpshim.boot is bumped before the first
 * subscribe each boot and reset to 0 once the shim has run stably CP_STABLE_S after subscribing. If it reaches
 * CP_MAX_CRASHES (consecutive boots that died subscribing), subscribe is skipped this boot -- CarPlay keeps working,
 * just no arrows -- so a regression can never brick the car in a boot loop. */
static int  g_counter_checked = 0;       /* bumped the boot counter once this boot */
static int  g_sub_blocked = 0;           /* crash guard tripped -> don't subscribe this boot */
static int  g_counter_reset = 0;         /* already reset the counter after a stable run */
static long g_sub_time = 0;              /* when the subscribe SURVIVED (for the stability timer) */

static uint16_t comp_id(void){ return (uint16_t)cfg_int("CP_COMP_ID", RGD_DEFAULT_COMP_ID); }

/* read the byte buffer out of an NmeArray<uchar>, with a sanity guard so a wrong layout never derefs garbage. */
static int narr_read(void* arr, const unsigned char** data, unsigned* len) {
    if (!arr) return 0;
    unsigned* w = (unsigned*)arr;
    int di = cfg_int("CP_NARR_DATA", 0), li = cfg_int("CP_NARR_LEN", 1);
    unsigned dptr = w[di], dlen = w[li];
    if (dptr < 0x1000u || dlen == 0 || dlen > 0x10000u) return 0;   /* implausible -> refuse to deref */
    *data = (const unsigned char*)(uintptr_t)dptr; *len = dlen;
    return 1;
}

/* ============================ UDP forward to carplay-kombi ============================
 * carplay-kombi (carplay_main.cpp) binds 127.0.0.1:42042 and parseDatagram() reads ';'-separated single-letter KEYs:
 *   G=<guiding 0/1>  T=<maneuver type>  U=<distance unit>  M=<distance value>  D=<text>  R=<road>  E=<exit>  L=<lanes>
 * carplay-kombi's clusterDirection() is the ANDROID AUTO maneuver table, so T must be an Android Auto Maneuver.TYPE_*
 * (rgd_apple_to_aa_type() translates the Apple CPManeuverType for us), NOT the raw Apple code.
 *
 * The phone sends a 0x5202 per maneuver in the list (a burst on app-switch); the CURRENT one + the live distance come
 * from 0x5201 (frequent). So we CACHE each 0x5202 by its index and DISPLAY the current maneuver -- the one 0x5201's
 * ManeuverList points at -- with the live countdown. Displaying every 0x5202 is what made the cluster flicker through
 * all upcoming turns and sit on a far "straight" maneuver. parseDatagram replaces feed.man wholesale, so each datagram
 * is complete (type + distance + road). */
#define MAN_SLOTS 32
static rg_maneuver_t g_man_slot[MAN_SLOTS];
static int  g_man_slot_idx[MAN_SLOTS];   /* actual maneuver index in each slot, -1 = empty */
static int  g_man_slots_init = 0;
static int  g_man_any = 0;               /* at least one maneuver cached */
static int  g_man_cur = -1;              /* current maneuver index from the latest 0x5201 */
static int  g_man_last = -1;             /* most recently cached index (fallback) */
static void man_cache_init(void) { for (int i = 0; i < MAN_SLOTS; i++) g_man_slot_idx[i] = -1; g_man_slots_init = 1; }
static const rg_maneuver_t* man_find(int idx) {
    if (idx < 0) return 0;
    int s = (int)(((unsigned)idx) % MAN_SLOTS);
    return g_man_slot_idx[s] == idx ? &g_man_slot[s] : 0;
}

/* Lane guidance (0x5204): cache the latest lane set by its maneuver index; it rides along with the maneuver datagram as
 * L=<lanes> (lanes left->right, ',' between lanes; each direction a shape digit 0..9, '!' before the lane to take).
 * The iAP2 angle -> LaneShape.shape mapping is best-effort until a real 0x5204 capture calibrates it (raw fields are
 * logged with CP_FWD_LOG so the next drive pins it down). */
static rg_laneinfo_t g_lanes;
static int g_lanes_have = 0;
static int g_lanes_idx = -1;

/* iAP2 JunctionAngle -> androidx LaneDirection.Shape (0 unk,1 straight,2/3 slight L/R,4/5 L/R,6/7 sharp L/R,8/9 uturn).
 * Assumes signed degrees, negative = left -- CONFIRM from the capture log and adjust the thresholds/sign if needed. */
static int lane_angle_to_shape(int a) {
    int m = a < 0 ? -a : a, left = a < 0;
    if (m <= 15)  return 1;              /* straight */
    if (m <= 45)  return left ? 2 : 3;   /* slight */
    if (m <= 135) return left ? 4 : 5;   /* normal */
    if (m <= 170) return left ? 6 : 7;   /* sharp */
    return left ? 8 : 9;                  /* u-turn */
}
/* Build the L= value from the cached lane set. Returns the length, 0 if nothing renderable. */
static int build_lane_str(char* out, size_t cap) {
    int o = 0;
    for (int i = 0; i < g_lanes.n && o < (int)cap - 8; i++) {
        const rg_lane_t* L = &g_lanes.lanes[i];
        if (i) out[o++] = ',';
        int take = (L->status >= 1);              /* status 2 preferred, 1 usable (best-effort) */
        int ndir = L->ang_n > 0 ? L->ang_n : 1;
        for (int k = 0; k < ndir && o < (int)cap - 4; k++) {
            int shape = L->ang_n > 0 ? lane_angle_to_shape(L->ang[k]) : 1;
            if (take && k == 0) out[o++] = '!';   /* the lane's primary direction = the one to take */
            out[o++] = (char)('0' + (shape % 10));
        }
    }
    out[o] = 0;
    return o;
}

/* Replicate carplay-kombi metric() (kombi_display.cpp): >= ~1000 m -> "X.X" km (unit kUnitKilometresTenths=3);
 * else metres rounded down (10 m step under 100, 50 m above), min 10 (unit kUnitMetres=1). carplay-kombi DISPLAYS the
 * D= text as-is (empty D -> "-" -> the "-km" we saw), and only auto-formats from M when the unit is unknown, so the
 * shim must produce D itself. */
static void fmt_metric(long m, char* out, size_t cap, int* unit) {
    if (m < 0) m = 0;
    if (m >= 1000) { snprintf(out, cap, "%ld.%ld", m / 1000, (m % 1000) / 100); *unit = 3; }   /* kUnitKilometresTenths */
    else { long step = m < 100 ? 10 : 50; long v = (m / step) * step; if (v < 10) v = 10;
           snprintf(out, cap, "%ld", v); *unit = 1; }                                           /* kUnitMetres */
}
/* Dump every TLV (id, len, hex preview) of a 0x52xx params buffer -- used with CP_RGD_DUMP to locate on-car which TLV
 * carries the road name / fields the parser misses (Cyrillic road names are UTF-8, so show hex not ascii). */
static void dump_tlvs(const char* tag, const uint8_t* p, size_t n) {
    while (n >= 4) {
        size_t tlen = rbe16(p); if (tlen < 4 || tlen > n) break;
        uint16_t id = rbe16(p + 2); const uint8_t* v = p + 4; size_t vl = tlen - 4;
        char h[24 * 3 + 4]; int hp = 0; size_t m = vl < 24 ? vl : 24;
        for (size_t k = 0; k < m; k++) hp += snprintf(h + hp, sizeof(h) - hp, "%02x ", v[k]);
        shim_log("  %s id=0x%04x len=%u [%s%s]", tag, id, (unsigned)vl, h, vl > 24 ? "..." : "");
        p += tlen; n -= tlen;
    }
}

static void send_feed(int guiding, int type, long metres, const char* road, int exit, const char* lanes, int next_type) {
    if (!opt("CP_FORWARD", 0)) return;
    if (g_udp < 0) { g_udp = socket(AF_INET, SOCK_DGRAM, 0); if (g_udp < 0) return; }
    char dtext[16]; int unit; fmt_metric(metres, dtext, sizeof(dtext), &unit);
    char msg[320];
    int n = snprintf(msg, sizeof(msg), "G=%d;T=%d;U=%d;M=%ld;D=%s;R=%s",
                     guiding ? 1 : 0, type, unit, metres, dtext, road ? road : "");
    if (exit > 0 && n > 0 && n < (int)sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, ";E=%d", exit);
    if (lanes && lanes[0] && n > 0 && n < (int)sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, ";L=%s", lanes);
    if (next_type >= 0 && n > 0 && n < (int)sizeof(msg))
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, ";N=%d", next_type);   /* the "then..." maneuver */
    struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)cfg_int("CP_UDP_PORT", 42042));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(g_udp, msg, (size_t)n, 0, (struct sockaddr*)&a, sizeof(a));
    if (opt("CP_FWD_LOG", 0)) shim_log("FWD %.*s", n, msg);
}
/* Translate a cached Apple maneuver to its Android Auto type (+ roundabout exit), attach the "then..." maneuver's AA
 * type (next_aa, or -1 for none), and emit a complete datagram. */
static void emit_maneuver(const rg_maneuver_t* m, long metres, const char* road, int next_aa) {
    int exit = 0;
    int aa = rgd_apple_to_aa_type(m->type, m->driving_side, &exit);
    char lanes[128]; lanes[0] = 0;
    if (g_lanes_have && (g_lanes_idx == (int)m->index || g_lanes_idx < 0)) build_lane_str(lanes, sizeof(lanes));
    send_feed(1, aa, metres, road, exit, lanes[0] ? lanes : 0, next_aa);
    if (opt("CP_FWD_LOG", 0))
        shim_log("MAP idx=%u apple=%u side=%u -> aa=%d exit=%d next_aa=%d lanes='%s'(laneIdx=%d)",
                 m->index, m->type, m->driving_side, aa, exit, next_aa, lanes, g_lanes_idx);
}
/* 0x5202: cache the maneuver by its index. Display it only if it is the CURRENT maneuver (the index 0x5201 points at)
 * or we have no current one yet -- so a burst of upcoming maneuvers does not flicker across the cluster. */
static void forward_maneuver(const rg_maneuver_t* m) {
    if (!g_man_slots_init) man_cache_init();
    int s = (int)(((unsigned)m->index) % MAN_SLOTS);
    g_man_slot[s] = *m; g_man_slot_idx[s] = (int)m->index;
    g_man_last = (int)m->index; g_man_any = 1;
    if ((int)m->index == g_man_cur || g_man_cur < 0) {
        const char* road = m->after_road[0] ? m->after_road : m->desc;
        emit_maneuver(m, (long)m->dist_between, road, -1);
    }
}
/* 0x5201: live distance update -> show the CURRENT maneuver (the one the ManeuverList points at) counting down, plus
 * the "then..." maneuver (the next one in the list) as the cluster's secondary arrow. */
static void forward_update(const rg_update_t* u) {
    if (u->route_state == 0) {   /* RouteGuidanceState 0 = guidance stopped: clear the arrow, don't show a stale/first
                                   * maneuver (otherwise the cluster drew maneuver 0 -- a U-turn -- after the route ended) */
        send_feed(0, 1, 0, "", 0, 0, -1);
        return;
    }
    if (!g_man_any) return;
    g_man_cur = (int)u->cur_maneuver_idx;
    const rg_maneuver_t* m = man_find(g_man_cur);
    if (!m) m = man_find(g_man_last);   /* fallback: newest cached maneuver */
    if (!m) return;
    int next_aa = -1;
    const rg_maneuver_t* nx = man_find(g_man_cur + 1);   /* the "then..." maneuver */
    if (nx) { int e2; next_aa = rgd_apple_to_aa_type(nx->type, nx->driving_side, &e2); }
    const char* road = u->road[0] ? u->road : (m->after_road[0] ? m->after_road : m->desc);
    emit_maneuver(m, (long)u->dist_to_maneuver, road, next_aa);
    if (nx && opt("CP_FWD_LOG", 0))
        shim_log("NEXT idx=%u apple=%u -> aa=%d road='%s'", nx->index, nx->type, next_aa,
                 nx->after_road[0] ? nx->after_road : nx->desc);
}

/* 0x5204: cache the lane set (keyed by its maneuver index) and log the raw per-lane fields so the angle->shape mapping
 * can be calibrated from a real capture. Lanes ride out with the next maneuver datagram (see emit_maneuver). */
static void forward_lane(const rg_laneinfo_t* li) {
    g_lanes = *li; g_lanes_have = 1; g_lanes_idx = (int)li->index;
    if (opt("CP_FWD_LOG", 0)) {
        char buf[128]; build_lane_str(buf, sizeof(buf));
        shim_log("LANE idx=%u n=%d -> L='%s'", li->index, li->n, buf);
        for (int i = 0; i < li->n; i++) {
            const rg_lane_t* L = &li->lanes[i];
            char ang[72]; int ao = 0;
            for (int k = 0; k < L->ang_n && ao < (int)sizeof(ang) - 8; k++)
                ao += snprintf(ang + ao, sizeof(ang) - (size_t)ao, "%d ", L->ang[k]);
            shim_log("  lane[%d] pos=%u status=%u highlight=%d angles=[%s]", i, L->position, L->status, L->highlight, ang);
        }
    }
}

/* ============================ native subscribe: responder + crash guard ============================
 * The responder is a fake C++ object: a vtable cloned from the stock NmeIAPRouteGuidance (keeping offset-to-top +
 * typeinfo for RTTI), every slot a safe no-op EXCEPT the two route-guidance callbacks, which we point at log-only
 * handlers. Maneuver DATA is delivered by our Decode 0x52xx parser (forward_maneuver), so these callbacks only need to
 * prove the subscribe took and flip g_got_520x. Buffers are static (never on the stack). */
static void* g_vtbuf[20];        /* [0]=offset-to-top, [1]=typeinfo, [2..]=virtual slots */
static void* g_resp_obj[512];    /* responder object (2 KB, zeroed); [0] = vptr -> &g_vtbuf[2] */
static int   g_resp_ready = 0;

static void gen_noop(void) {}
static void my_maneuver(void* self, void* sp) {
    (void)self; void* m = sp ? *(void**)sp : 0;
    shim_log("responder MAN cb fired sp=%p obj=%p", sp, m);
    g_got_520x = 1;
}
static void my_update(void* self, void* sp) { (void)self; (void)sp; shim_log("responder UPD cb fired"); }

static int build_responder(void) {
    if (g_resp_ready) return 0;
    void* sym = dlsym(RTLD_DEFAULT, "_ZTV19NmeIAPRouteGuidance");
    if (!sym) sym = dlsym(RTLD_NEXT, "_ZTV19NmeIAPRouteGuidance");
    if (!sym) { shim_log("no _ZTV19NmeIAPRouteGuidance (base vtable) -- cannot build responder"); return -1; }
    void** vt = (void**)sym;
    g_vtbuf[0] = vt[0];               /* offset-to-top */
    g_vtbuf[1] = vt[1];               /* typeinfo */
    int man_slot = -1, upd_slot = -1;
    int slots = (int)(sizeof(g_vtbuf) / sizeof(g_vtbuf[0])) - 2;
    for (int i = 0; i < slots; ++i) {
        void* fp = vt[2 + i];
        g_vtbuf[2 + i] = (void*)gen_noop;
        if (!fp) continue;
        Dl_info di;
        const char* nm = (dladdr(fp, &di) && di.dli_sname) ? di.dli_sname : "";
        if (strstr(nm, "OnRouteGuidanceManeuverUpdate")) man_slot = i;
        else if (strstr(nm, "OnRouteGuidanceUpdate")) upd_slot = i;
    }
    if (man_slot < 0) { shim_log("OnRouteGuidanceManeuverUpdate slot not found -- abort subscribe"); return -1; }
    g_vtbuf[2 + man_slot] = (void*)my_maneuver;
    if (upd_slot >= 0) g_vtbuf[2 + upd_slot] = (void*)my_update;
    g_resp_obj[0] = (void*)&g_vtbuf[2];
    g_resp_ready = 1;
    shim_log("responder built: man_slot=%d upd_slot=%d (others no-op) obj=%p", man_slot, upd_slot, (void*)g_resp_obj);
    return 0;
}

/* anti-boot-loop crash counter: an int in /var/local/cpshim.boot, bumped before the first subscribe each boot and
 * reset after a stable run. Persisted so consecutive boot-crashes accumulate; at CP_MAX_CRASHES the shim stops
 * subscribing (CarPlay still works). (CP_GEN_FILE kept as the path override for back-compat.) */
static const char* boot_path(void) {
    const char* p = getenv("CP_GEN_FILE"); if (!p || !*p) p = "/var/local/cpshim.boot"; return p;
}
static int read_crashes(void) {
    int fd = open(boot_path(), O_RDONLY); if (fd < 0) return 0;
    char b[32]; int n = (int)read(fd, b, sizeof(b) - 1); close(fd);
    if (n <= 0) return 0;
    b[n] = 0; return (int)strtol(b, 0, 0);
}
static void write_crashes(int v) {
    int fd = open(boot_path(), O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd < 0) return;
    char b[32]; int n = snprintf(b, sizeof(b), "%d\n", v);
    if (write(fd, b, n) < 0) { /* ignore */ }
    close(fd);
}

/* A subscribable iap is one whose vtable[67] resolves EXACTLY to the concrete StartRouteGuidanceUpdates implementation
 * (libNmeSDK.so + 0x1897c0). Rejects the proxy iap and abstract iaps. CP_RGU_IMPL_OFF overrides (0 = any libNme). */
static int iap_is_real(void* iap) {
    if (!iap) return 0;
    void** vt = *(void***)iap;
    void* fp = vt[cfg_int("CP_RGU_SLOT", 67)];
    Dl_info di;
    if (!dladdr(fp, &di) || !di.dli_fname) return 0;
    if (!strstr(di.dli_fname, "libNme")) return 0;
    if (di.dli_sname && strstr(di.dli_sname, "__cxa_pure_virtual")) return 0;
    long want = (long)cfg_int("CP_RGU_IMPL_OFF", 0x1897c0);
    if (want > 0 && di.dli_fbase) {
        long off = (long)((char*)fp - (char*)di.dli_fbase);
        if (off != want) return 0;
    }
    return 1;
}

/* Return the current live REAL iap (g_real_iap), or 0. Derefs ONLY g_real_iap (never a stale cached array); the single
 * iap+0x80 channel read is safe because g_real_iap is the freshly-captured current session's iap. No iap_is_real here
 * (that already ran, safely, at capture time in CinemoCreateIAP). */
static void* pick_real_iap(void) {
    void* iap = g_real_iap;
    if (!iap) return 0;
    if (cfg_int("CP_REQUIRE_CHANNEL", 1)) { void* ch = *(void**)((char*)iap + 0x80); if (!ch) return 0; }
    return iap;
}

/* Call the real iap's vtable[67] = StartRouteGuidanceUpdates(self, uint32 count, const uint16* componentIdList).
 * CORE-PROVEN SIGNATURE (core.CARPLAY.3251, crash @libNmeVfs+0x149810, serialiser): arg1 is the COUNT of component
 * ids and arg2 is a POINTER to a u16 host-order array of those ids; the serialiser does `for(i<count) AddParam(msg,
 * 0x0000, rev16(list[i]), 2)` to emit the 0x5200. The old (self, responder, ctx) guess crashed because we passed the
 * responder ptr (0x76b38a5c) as count and ctx=NULL as the list -> ldrh [NULL] with count=huge. We now pass count=1 and
 * a static [comp_id()] array. (The responder/callback is NOT an arg here; maneuvers arrive via the Decode 0x52xx hook.)
 * Returns 1 if it returned (survived), 0 if skipped. A segfault inside never returns (guarded by the gen marker). */
static uint16_t g_compid_list[8];
static int subscribe_call(void* iap) {
    void** vt = *(void***)iap;
    int slot = cfg_int("CP_RGU_SLOT", 67);
    void* fp = vt[slot];
    void* ch80 = *(void**)((char*)iap + 0x80);
    unsigned count = (unsigned)cfg_int("CP_RG_COUNT", 1);
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    for (unsigned i = 0; i < 8; ++i) g_compid_list[i] = comp_id();   /* host-order u16; serialiser rev16's to BE */
    Dl_info di; const char* nm = (dladdr(fp, &di) && di.dli_sname) ? di.dli_sname : "?";
    shim_log("subscribe: iap=%p vt[%d]=%p %s count=%u comp=0x%04x list=%p ch=%p attempt=%d -> calling",
             iap, slot, fp, nm, count, comp_id(), (void*)g_compid_list, ch80, g_sub_attempts);
    if (cfg_int("CP_NO_CALL", 0)) { shim_log("  CP_NO_CALL -> inspected only"); return 0; }
    typedef int (*fn)(void*, unsigned, const void*);
    int rc = ((fn)fp)(iap, count, (const void*)g_compid_list);
    shim_log("  StartRouteGuidanceUpdates rc=%d (SURVIVED)", rc);
    return 1;
}

/* Worker thread: idle until the declaration is accepted and CP_RGD_SUBSCRIBE is armed, then emit 0x5200 via the native
 * call, retried on the repo cadence (fast CP_SUB_FAST_N x CP_SUB_FAST_MS, then CP_SUB_SLOW_MS) until the first 0x52xx. */
static void* worker_main(void* a) {
    (void)a;
    int warm = cfg_int("CP_SUB_START_MS", 6000);
    if (warm > 0 && !g_stop) usleep((useconds_t)warm * 1000);
    build_responder();
    shim_log("worker ready: idle until CP_RGD_SUBSCRIBE=1 + ident-accept+auth+patch (crash-counter=%d/%d)",
             read_crashes(), cfg_int("CP_MAX_CRASHES", 3));
    while (!g_stop) {
        int poll = cfg_int("CP_SUB_POLL_MS", 1000);
        if (opt("CP_RGD_SUBSCRIBE", 0) && !g_got_520x && !g_sub_done && !g_sub_blocked
            && g_ident_accepted && g_auth_done && g_ident_patched) {   /* one subscribe per session; stop after SURVIVED */
            if (!g_counter_checked) {                                  /* once per boot: check + bump the crash counter */
                g_counter_checked = 1;
                int crashes = read_crashes(), maxc = cfg_int("CP_MAX_CRASHES", 3);
                if (crashes >= maxc) { g_sub_blocked = 1;
                    shim_log("crash-guard: %d boot(s) died subscribing (>=%d) -> SUBSCRIBE DISABLED this boot; CarPlay unaffected (reset: rm %s)", crashes, maxc, boot_path()); }
                else { write_crashes(crashes + 1);
                    shim_log("crash-guard: subscribe attempt, boot-counter %d->%d (resets after %ds stable)", crashes, crashes + 1, cfg_int("CP_STABLE_S", 45)); }
            }
            if (!g_sub_blocked) {
                long t = now_ms();
                int fastn = cfg_int("CP_SUB_FAST_N", 5);
                long fast = cfg_int("CP_SUB_FAST_MS", 2000), slow = cfg_int("CP_SUB_SLOW_MS", 30000);
                int due = g_loc_trigger || t >= g_sub_next_ms;
                if (due) {
                    g_loc_trigger = 0;
                    void* iap = pick_real_iap();
                    if (iap) {
                        if (subscribe_call(iap)) { g_sub_alive = 1; g_sub_attempts++; g_sub_done = 1; g_sub_time = now_ms(); }  /* one-shot: a surviving API subscribe persists */
                        g_sub_next_ms = now_ms() + (g_sub_attempts < fastn ? fast : slow);
                    } else {
                        shim_log("subscribe: no real iap with a live channel yet (g_real_iap=%p)", (void*)g_real_iap);
                        g_sub_next_ms = t + fast;
                    }
                }
            }
        }
        /* stability reset: once alive CP_STABLE_S past a surviving subscribe, clear the boot crash counter. */
        if (g_sub_done && g_sub_alive && !g_counter_reset && g_sub_time
            && now_ms() - g_sub_time > (long)cfg_int("CP_STABLE_S", 45) * 1000) {
            write_crashes(0); g_counter_reset = 1;
            shim_log("crash-guard: stable %ds after subscribe -> boot-counter reset", cfg_int("CP_STABLE_S", 45));
        }
        if (g_stop) break;
        usleep((useconds_t)(poll > 0 ? poll : 1000) * 1000);
    }
    shim_log("worker exiting (stop=%d got_520x=%d attempts=%d alive=%d)", g_stop, g_got_520x, g_sub_attempts, g_sub_alive);
    return 0;
}

/* ============================ RETIRED: 0x5200 raw 0xFFFB-replace inject (DEAD END) ============================
 * Replacing an outgoing LocationInfo frame with a 0x5200 desyncs the stock link-state/retransmit accounting -> incoming
 * Decode goes silent. Kept off (CP_RGD_INJECT default 0) for reference; the native subscribe above is the real path. */
static volatile int g_sent_start = 0;
static int g_inject_rc = 0;
static int try_inject_replace(void* self, const unsigned char* data, unsigned len, unsigned* out) {
    if (g_sent_start || !opt("CP_RGD_INJECT", 0)) return 0;
    if (!(g_ident_accepted && g_auth_done) || !real_send) return 0;
    if (!data || len < 15 || data[0] != 0xff || data[1] != 0x5a) return 0;
    if (data[9] != 0x40 || data[10] != 0x40 || rbe16(data + 13) != 0xFFFB) return 0;
    uint8_t sess[64]; int sn = rgd_build_start_frame(sess, sizeof(sess), comp_id());
    if (sn <= 0) return 0;
    uint8_t fr[128];
    int fl = iap2_link_frame(fr, sizeof(fr), data[4], data[5], data[6], data[7], sess, (size_t)sn);
    if (fl <= 0) return 0;
    g_inject_rc = real_send(self, fr, (unsigned)fl, out);
    g_sent_start = 1;
    shim_log("inject: replaced 0xFFFB@seq=0x%02x with 0x5200 (link %d B) rc=%d comp=0x%04x", data[5], fl, g_inject_rc, comp_id());
    return 1;
}

/* ============================ Encode hook (outgoing) ============================ */
int shim_encode(void* self, void* arr) asm("_ZNK14NmeIAP2Message6EncodeER8NmeArrayIhE");
int shim_encode(void* self, void* arr) {
    int rc = real_encode ? real_encode(self, arr) : -1;
    if (g_stop || !is_carplay() || !arr) return rc;
    const unsigned char* d; unsigned n;
    if (!narr_read(arr, &d, &n)) {
        if (opt("CP_ENC_RECON", 1)) { unsigned* w = (unsigned*)arr;
            shim_log("ENC arr words: [0]=%08x [1]=%08x [2]=%08x [3]=%08x (narr_read refused -- set CP_NARR_DATA/LEN)",
                     w[0], w[1], w[2], w[3]); }
        return rc;
    }
    uint16_t mid; const uint8_t* pl; size_t pn;
    if (!iap2_frame_parse(d, n, &mid, &pl, &pn)) return rc;
    if (mid == IAP2_MSG_IDENTIFY) {
        /* A fresh Identify means a (re)connect: re-arm the 0x5200 request state for the new session. */
        g_got_520x = 0; g_sub_attempts = 0; g_sub_next_ms = 0; g_loc_trigger = 1; g_sub_done = 0;
        size_t have; const uint8_t* comp = iap2_tlv_find(pl, pn, IDENT_TLV_RG_COMPONENT, &have);
        if (opt("CP_ENC_RECON", 1)) {
            unsigned* w = (unsigned*)arr;
            shim_log("ENC Identify 0x1D01: arr[0..2]=%08x/%08x/%08x total=%u 0x001E_present=%d(len=%u)",
                     w[0], w[1], w[2], n, comp ? 1 : 0, comp ? (unsigned)have : 0u);
            hexdump("ENC Identify head", d, n < 64 ? (int)n : 64);
            if (opt("CP_ENC_FULL", 0)) hexdump_full("ENC Identify", d, (int)n);
        }
        if (opt("CP_RGD_PATCH", 0)) {
            unsigned* w = (unsigned*)arr;
            int di = cfg_int("CP_NARR_DATA", 0), li = cfg_int("CP_NARR_LEN", 1), ci = cfg_int("CP_NARR_CAP", 2);
            size_t bufcap = (size_t)n + 256;
            unsigned char* nb = (unsigned char*)malloc(bufcap);
            if (nb) {
                int rl = rebuild_identify_with_rgd(nb, bufcap, d, n, comp_id());
                if (rl > (int)n) {
                    w[di] = (unsigned)(uintptr_t)nb;
                    w[li] = (unsigned)rl;
                    w[ci] = (unsigned)rl;
                    g_ident_patched = 1;
                    shim_log("ENC PATCH v18: rebuilt Identify %u -> %d (RG lists 0x0006/0x0007 + comp 0x%04x)", n, rl, comp_id());
                } else {
                    free(nb);
                    shim_log("ENC PATCH: rebuild rl=%d (n=%u) -- no change", rl, n);
                }
            } else {
                shim_log("ENC PATCH: malloc(%u) FAILED", (unsigned)bufcap);
            }
        }
    }
    return rc;
}

/* ============================ incoming handler (Decode) ============================ */
static int g_dec_rawcap = 0;
static void handle_incoming(const unsigned char* data, int len, const char* src) {
    if (!data || len < 6) return;
    if (opt("CP_DEC_RAW", 0) && g_dec_rawcap < 300) { char t[24]; snprintf(t, sizeof(t), "%s raw", src);
        hexdump(t, data, len); g_dec_rawcap++; }
    const unsigned char* s = data; int sl = len;
    if (len >= 11 && data[0] == 0xff && data[1] == 0x5a) { s = data + 9; sl = len - 9; }   /* unwrap link frame */
    uint16_t mid; const uint8_t* pl; size_t pn;
    if (!iap2_frame_parse(s, (size_t)sl, &mid, &pl, &pn)) return;
    if (mid == IAP2_MSG_IDENTIFY_ACCEPTED) { g_ident_accepted = 1; shim_log("%s 0x1D02 IdentifyAccepted", src); }
    else if (mid == IAP2_MSG_IDENTIFY_REJECTED) { shim_log("%s 0x1D03 IdentifyREJECTED", src); }
    else if (mid == IAP2_MSG_AUTH_SUCCEEDED) { g_auth_done = 1; shim_log("%s 0xAA05 AuthSucceeded", src); }
    else if (mid == IAP2_MSG_RG_MANEUVER) { g_got_520x = 1;
            if (opt("CP_RGD_DUMP", 0)) dump_tlvs("5202", pl, pn);
            rg_maneuver_t m; if (rgd_parse_maneuver(pl, pn, &m)) {
            shim_log("%s 0x5202 maneuver idx=%u type=%u dist=%u side=%u road='%s' desc='%s'", src, m.index, m.type, m.dist_between, m.driving_side, m.after_road, m.desc);
            forward_maneuver(&m); } }
    else if (mid == IAP2_MSG_RG_UPDATE) { g_got_520x = 1;
            if (opt("CP_RGD_DUMP", 0)) dump_tlvs("5201", pl, pn);
            rg_update_t u; if (rgd_parse_update(pl, pn, &u)) {
            shim_log("%s 0x5201 update state=%u road='%s' dest='%s' dist=%u", src, u.route_state, u.road, u.dest, u.dist_to_maneuver);
            forward_update(&u); } }
    else if (mid == IAP2_MSG_RG_LANE) { g_got_520x = 1;
            if (opt("CP_RGD_DUMP", 0)) dump_tlvs("5204", pl, pn);
            rg_laneinfo_t li; if (rgd_parse_lane(pl, pn, &li)) {
            shim_log("%s 0x5204 lane idx=%u n=%d", src, li.index, li.n);
            forward_lane(&li); } }
}

/* ============================ Decode hook (incoming) ============================ */
int shim_decode(void* self, const unsigned char* data, int len) asm("_ZN14NmeIAP2Message6DecodeEPKhi");
int shim_decode(void* self, const unsigned char* data, int len) {
    if (!g_stop && is_carplay()) handle_incoming(data, len, "DEC");
    return real_decode ? real_decode(self, data, len) : -1;
}

/* ============================ Send hook (capture transport + arm 0x5200 on LocationInfo) ============================ */
static int g_send_rawcap = 0;
int shim_send(void* self, const unsigned char* data, unsigned len, unsigned* out) asm("_ZN12NmeTransport4SendEPKhjPj");
int shim_send(void* self, const unsigned char* data, unsigned len, unsigned* out) {
    if (!g_stop && is_carplay()) {
        if (!g_transport) { g_transport = self; shim_log("captured NmeTransport=%p", self); }
        if (opt("CP_SEND_RAW", 0) && g_send_rawcap < 60 && data && len >= 2) {
            hexdump("SND raw", data, len < 20 ? (int)len : 20); g_send_rawcap++; }
        /* repo trigger: an outgoing LocationInfo (0xFFFB) is the earliest point the phone is ready for 0x5200 */
        if (data && len >= 15 && data[0] == 0xff && data[1] == 0x5a &&
            data[9] == 0x40 && data[10] == 0x40 && rbe16(data + 13) == 0xFFFB) {
            if (opt("CP_RGD_SUBSCRIBE", 0) && !g_got_520x) g_loc_trigger = 1;
        }
        /* retired raw-splice inject (off by default) */
        if (try_inject_replace(self, data, len, out)) return g_inject_rc;
    }
    return real_send ? real_send(self, data, len, out) : -1;
}

/* ============================ Recv hook -- DISABLED (v12) ============================
 * The Recv NmeArray layout differs from Encode's; a wrong deref crashed CARPLAY (v11, core 9249). Incoming is carried
 * by the Decode hook. Do not re-add without core-confirming the Recv NmeArray layout. */

/* ============================ CinemoCreateIAP capture ============================ */
typedef void* (*cci_fn)(void** out);
void* CinemoCreateIAP(void** out) {
    static cci_fn real = 0; if (!real) real = (cci_fn)real_of("CinemoCreateIAP");
    void* r = real ? real(out) : 0;
    if (is_carplay() && out && *out) {
        g_iap = *out;
        int real = iap_is_real(*out);          /* SAFE here: *out is freshly constructed (not a stale/freed pointer) */
        if (real) g_real_iap = *out;           /* the ONLY writer of g_real_iap; always points at a just-created iap */
        shim_log("captured iap=%p real=%d%s", *out, real, real ? " -> g_real_iap" : "");
    }
    return r;
}

/* ============================ ctor/dtor ============================ */
__attribute__((constructor)) static void shim_init(void) {
    real_encode = (enc_fn)real_of("_ZNK14NmeIAP2Message6EncodeER8NmeArrayIhE");
    real_decode = (dec_fn)real_of("_ZN14NmeIAP2Message6DecodeEPKhi");
    real_send   = (snd_fn)real_of("_ZN12NmeTransport4SendEPKhjPj");
    real_recv   = (rcv_fn)real_of("_ZN12NmeTransport4RecvER8NmeArrayIhE");
    if (is_carplay()) {
        shim_log("=== cpshim v24 (persistent-ready: lifetime-safe + distance/road + anti-boot-loop crash-counter) pid %d enc=%p dec=%p send=%p PATCH=%d SUBSCRIBE=%d crashes=%d FWD=%d ===",
                 (int)getpid(), (void*)real_encode, (void*)real_decode, (void*)real_send,
                 opt("CP_RGD_PATCH",0), opt("CP_RGD_SUBSCRIBE",0), read_crashes(), opt("CP_FORWARD",0));
        pthread_t t;
        if (pthread_create(&t, 0, worker_main, 0) == 0) pthread_detach(t);
        else shim_log("pthread_create failed -- no subscribe worker");
    }
    (void)real_recv;
}
__attribute__((destructor)) static void shim_fini(void) { g_stop = 1; usleep(1000); }

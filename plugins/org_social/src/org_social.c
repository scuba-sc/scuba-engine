/* SPDX-License-Identifier: GPL-3.0-only
 * org_social: org roster, party presence and shared waypoints over sco.net + sco.storage,
 * with game.spatial pose_to_zone for billboard positions when the game pack publishes it.
 * Every host service is optional: without one the matching feature is off, never the plugin.
 */
#include "sco_api.h"
#include "sco_net.h"
#include "sco_storage.h"
#include "sco_widgets.h"
#include "sc_game_spatial.h"

#include "os_json.h"
#include "os_model.h"
#include "os_wire.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_ROSTER "org_social.roster"
#define CH_POSE   "org_social.pose"
#define PANEL_ID  "org_social.roster"
#define TABLE_COLUMNS 4u
#define TABLE_MAX_ROWS 63u   /* plus the header row: 64 * 4 = 256 cells, SCO_WIDGETS_MAX_ITEMS */
#define TABLE_PAGE 10u
#define REFRESH_TICKS 50u
#define POSE_TICKS 5u

#define API_HAS(m) (api->size >= offsetof(sco_api, m) + sizeof(api->m) && api->m != NULL)

enum { N_ROOT = 1, N_SUMMARY, N_TABLE, N_WAYPOINTS, N_EDIT_ROW, N_HANDLE, N_RANK, N_ADD, N_REMOVE,
       N_WP_ROW, N_SHARE, N_WP_REMOVE, N_COUNT = N_WP_REMOVE };

static const sco_api*        api;
static sco_plugin*           me;
static const sco_storage_v1* st;
static const sco_net_v1*     net;
static const sco_widgets_v1* widgets;
static int                   widgets_11;
static int                   panel_dirty;
static os_model              model;
static int                   session_active;
static int                   hello_due;
static int                   pose_dirty;
static uint32_t              ticks;
static char                  pending_handle[OS_MAX_HANDLE + 1];
static uint32_t              pending_rank;
static int32_t               selected_wp;
static uint32_t              table_first_row;
static int                   pose_channel_context;

static const sco_plugin_info info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, 0, "org_social", "0.1.0", "scubamount"
};

static void say(sco_log_level level, const char* msg) { api->log(me, level, msg); }

static void copy_text(char* dst, size_t cap, const char* src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- storage: m/<handle> = rank name, w/<id> = "<zone> <x> <y> <z> <name>", cfg/share = 0|1 ---- */

static int store_text(const char* key, const char* value) {
    if (!st) return 0;
    return st->put(me, key, value, (uint32_t)strlen(value)) == SCO_OK;
}

static void store_del(const char* key) {
    if (st) (void)st->del(me, key);
}

static int store_get(const char* key, char* out, uint32_t cap) {
    uint32_t n = cap - 1;
    if (!st || st->get(me, key, out, &n) != SCO_OK || n >= cap) return 0;
    out[n] = 0;
    return 1;
}

static int save_waypoint(const os_waypoint* wp) {
    char key[24], val[192];
    snprintf(key, sizeof key, "w/%u", (unsigned)wp->id);
    snprintf(val, sizeof val, "%llu %.17g %.17g %.17g %s", (unsigned long long)wp->zone,
             wp->pos[0], wp->pos[1], wp->pos[2], wp->name);
    return store_text(key, val);
}

static void load_waypoint(const char* key, const char* val) {
    char* end = NULL;
    const char* p = val;
    double pos[3];
    uint64_t zone;
    unsigned long long id;
    int k;
    id = strtoull(key + 2, &end, 10);
    if (!end || *end || id == 0 || id > 0xffffffffULL) return;
    zone = (uint64_t)strtoull(p, &end, 10);
    if (end == p || *end != ' ') return;
    for (k = 0; k < 3; ++k) {
        p = end + 1;
        pos[k] = strtod(p, &end);
        if (end == p || *end != ' ') return;
    }
    (void)os_waypoint_put(&model, 0, (uint32_t)id, end + 1, zone, pos);
}

static void load_store(void) {
    char key[SCO_STORAGE_MAX_KEY + 1], last[SCO_STORAGE_MAX_KEY + 1], val[192];
    const char* prefixes[2] = { "m/", "w/" };
    int p;
    if (!st) return;
    for (p = 0; p < 2; ++p) {
        const char* after = NULL;
        for (;;) {
            uint32_t n = sizeof key;
            uint32_t rank;
            if (st->next_key(me, prefixes[p], after, key, &n) != SCO_OK) break;
            copy_text(last, sizeof last, key);
            after = last;
            if (!store_get(key, val, sizeof val)) continue;
            if (p == 0) {
                if (os_rank_parse(val, &rank)) (void)os_member_set(&model, key + 2, rank);
            } else {
                load_waypoint(key, val);
            }
        }
    }
    if (store_get("cfg/share", val, sizeof val)) model.share_waypoints = strcmp(val, "0") != 0;
}

/* ---- sco.net ------------------------------------------------------------------------------- */

static void send_msg(const char* channel, const os_msg* msg) {
    uint8_t buf[OS_WIRE_MAX_ROSTER];
    size_t n;
    if (!net || !session_active) return;
    n = os_wire_encode(msg, buf, sizeof buf);
    if (n && net->send_channel(me, channel, buf, (uint32_t)n) != SCO_OK)
        say(SCO_LOG_WARN, "sco.net refused an outgoing message");
}

static void send_waypoint(const os_waypoint* wp, int remove) {
    os_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.type = remove ? OS_MSG_WAYPOINT_DEL : OS_MSG_WAYPOINT_SET;
    msg.id = wp->id;
    msg.zone = wp->zone;
    memcpy(msg.pos, wp->pos, sizeof msg.pos);
    copy_text(msg.text, sizeof msg.text, wp->name);
    send_msg(CH_ROSTER, &msg);
}

static void send_own_waypoints(int remove) {
    uint32_t i;
    for (i = 0; i < model.nwaypoints; ++i) {
        if (model.waypoints[i].owner_peer == 0) send_waypoint(&model.waypoints[i], remove);
    }
}

static void send_hello(void) {
    os_msg msg;
    const os_member* mine;
    memset(&msg, 0, sizeof msg);
    if (!net || !session_active || net->get_peer_name(model.self_peer, msg.text, sizeof msg.text) != SCO_OK ||
        !os_handle_valid(msg.text)) return;
    mine = os_find_member(&model, msg.text);
    msg.type = OS_MSG_HELLO;
    msg.status = model.self_status;
    msg.rank = (mine && mine->in_org) ? mine->rank : OS_RANK_GUEST;
    send_msg(CH_ROSTER, &msg);
    if (model.share_waypoints) send_own_waypoints(0);
}

static void refresh_panel(void);

static void refresh_session(void) {
    sco_net_peer peers[OS_MAX_PEERS];
    os_session_peer named[OS_MAX_PEERS];
    uint32_t count = OS_MAX_PEERS, i, n = 0;
    uint64_t self = 0;
    int was_active = session_active, changed;
    if (!net) return;
    session_active = net->is_active() != 0;
    if (session_active) {
        self = net->self_peer();
        const sco_result peer_result = net->get_peers(peers, &count);
        if (peer_result == SCO_TOO_MANY) count = OS_MAX_PEERS;
        else if (peer_result != SCO_OK) { say(SCO_LOG_WARN, "sco.net peer enumeration failed"); return; }
        for (i = 0; i < count && i < OS_MAX_PEERS; ++i) {
            if (peers[i].peer_id == self) continue;
            named[n].peer_id = peers[i].peer_id;
            if (net->get_peer_name(peers[i].peer_id, named[n].handle, sizeof named[n].handle) != SCO_OK ||
                !os_handle_valid(named[n].handle))
                snprintf(named[n].handle, sizeof named[n].handle, "peer-%llu", (unsigned long long)peers[i].peer_id);
            ++n;
        }
    }
    changed = os_session_set(&model, self, named, n);
    if (session_active && changed) hello_due = 1;
    if (changed || was_active != session_active) refresh_panel();
}

static void on_message(uint64_t sender, const void* buf, uint32_t len, void* ctx) {
    os_msg msg;
    os_member* mem;
    if (sender == 0 || sender == model.self_peer || !os_wire_decode((const uint8_t*)buf, len, &msg)) return;
    if ((ctx == &pose_channel_context) != (msg.type == OS_MSG_POSE)) return;
    mem = os_find_peer(&model, sender);
    if (!mem) {
        refresh_session();
        mem = os_find_peer(&model, sender);
    }
    if (!mem) return;
    switch (msg.type) {
    case OS_MSG_HELLO:
        if (strcmp(msg.text, mem->handle) != 0) return;
        mem->status = msg.status;
        mem->has_declared_rank = 1;
        mem->declared_rank = msg.rank;
        break;
    case OS_MSG_WAYPOINT_SET:
        (void)os_waypoint_put(&model, sender, msg.id, msg.text, msg.zone, msg.pos);
        break;
    case OS_MSG_WAYPOINT_DEL:
        (void)os_waypoint_remove(&model, sender, msg.id);
        break;
    case OS_MSG_POSE:
        mem->pose.valid = 1;
        mem->pose.zone = msg.zone;
        memcpy(mem->pose.pos, msg.pos, sizeof mem->pose.pos);
        return;   /* poses change often and show only in billboards: no panel rebuild */
    default:
        return;
    }
    panel_dirty = 1;
}

static void on_net_event(const char* event, const void* data, void* ctx) {
    (void)event; (void)data; (void)ctx;
    refresh_session();
}

static void on_tick(const char* event, const void* data, void* ctx) {
    (void)event; (void)data; (void)ctx;
    ++ticks;
    if (ticks % REFRESH_TICKS == 0) refresh_session();
    if (hello_due) { hello_due = 0; send_hello(); }
    if (pose_dirty && model.self_pose.valid && ticks % POSE_TICKS == 0) {
        os_msg msg;
        memset(&msg, 0, sizeof msg);
        msg.type = OS_MSG_POSE;
        msg.zone = model.self_pose.zone;
        memcpy(msg.pos, model.self_pose.pos, sizeof msg.pos);
        send_msg(CH_POSE, &msg);
        pose_dirty = 0;
    }
    if (panel_dirty) refresh_panel();
}

/* ---- game.spatial (capability-gated) ------------------------------------------------------- */

static const sc_game_spatial_v1* find_spatial(void) {
    const void* found = NULL;
    const sc_game_spatial_v1* sp;
    if (!API_HAS(query_service) ||
        api->query_service(SC_GAME_SPATIAL_NAME, SC_GAME_SPATIAL_VERSION, &found) != SCO_OK || !found) return NULL;
    sp = (const sc_game_spatial_v1*)found;
    if (sp->size < offsetof(sc_game_spatial_v1, pose_to_zone) + sizeof(sp->pose_to_zone) || !sp->pose_to_zone)
        return NULL;
    return sp;
}

static int spatial_to_frame(void* ctx, uint64_t from, uint64_t to, const double in[3], double out[3]) {
    const sc_game_spatial_v1* sp = (const sc_game_spatial_v1*)ctx;
    const double rot[4] = { 0.0, 0.0, 0.0, 1.0 };
    double out_rot[4];
    return sp->pose_to_zone(from, to, in, rot, out, out_rot) == 1;
}

/* ---- JSON documents ------------------------------------------------------------------------ */

static void build_billboards(os_json* j, const char* text, size_t len, int* ok) {
    os_view view;
    const sc_game_spatial_v1* sp = find_spatial();
    memset(&view, 0, sizeof view);
    *ok = os_view_parse(text, len, &view);
    if (!*ok) return;
    if (sp) { view.to_frame = spatial_to_frame; view.ctx = (void*)(uintptr_t)sp; }
    os_json_billboards(j, &model, &view, sp != NULL);
}

static sco_result raw_reply(os_json* j, uint32_t* inout_size) {
    if (!inout_size) return SCO_OK;
    if (j->len > 0xffffffffu) return SCO_TOO_MANY;
    *inout_size = (uint32_t)j->len;
    return j->len > j->cap ? SCO_TOO_MANY : SCO_OK;
}

static sco_result raw_roster(const void* in, uint32_t in_size, void* out, uint32_t* inout_size, void* ctx) {
    os_json j;
    (void)in; (void)in_size; (void)ctx;
    os_json_init(&j, (char*)out, inout_size ? *inout_size : 0);
    os_json_roster(&j, &model, session_active);
    return raw_reply(&j, inout_size);
}

static sco_result raw_billboards(const void* in, uint32_t in_size, void* out, uint32_t* inout_size, void* ctx) {
    os_json j;
    int ok;
    (void)ctx;
    if (in_size && !in) return SCO_BAD_ARG;
    os_json_init(&j, (char*)out, inout_size ? *inout_size : 0);
    build_billboards(&j, in_size ? (const char*)in : "", in_size, &ok);
    if (!ok) { if (inout_size) *inout_size = 0; return SCO_BAD_ARG; }
    return raw_reply(&j, inout_size);
}

/* ---- commands ------------------------------------------------------------------------------ */

static sco_result cmd_member_add(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    uint32_t rank;
    int r;
    char key[OS_MAX_HANDLE + 3];
    (void)n; (void)ctx;
    if (!os_handle_valid(a[0].v.s)) { snprintf(reply, cap, "bad handle"); return SCO_BAD_ARG; }
    if (!os_rank_parse(a[1].v.s, &rank)) {
        snprintf(reply, cap, "rank: recruit, member, officer, director or founder");
        return SCO_BAD_ARG;
    }
    r = os_member_set(&model, a[0].v.s, rank);
    if (r == -2) { snprintf(reply, cap, "roster full (%u)", (unsigned)OS_MAX_MEMBERS); return SCO_TOO_MANY; }
    snprintf(key, sizeof key, "m/%s", a[0].v.s);
    snprintf(reply, cap, "%s %s %s%s", a[0].v.s, r == 1 ? "added as" : "is now", os_rank_name(rank),
             store_text(key, os_rank_name(rank)) ? "" : " (not saved: storage unavailable or write failed)");
    refresh_panel();
    return SCO_OK;
}

static sco_result cmd_member_remove(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    char key[OS_MAX_HANDLE + 3];
    (void)n; (void)ctx;
    if (!os_handle_valid(a[0].v.s)) { snprintf(reply, cap, "bad handle"); return SCO_BAD_ARG; }
    if (!os_member_remove(&model, a[0].v.s)) { snprintf(reply, cap, "%s is not on the roster", a[0].v.s); return SCO_FAILED; }
    snprintf(key, sizeof key, "m/%s", a[0].v.s);
    store_del(key);
    snprintf(reply, cap, "%s removed", a[0].v.s);
    refresh_panel();
    return SCO_OK;
}

static sco_result cmd_roster(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    (void)a; (void)n; (void)ctx;
    snprintf(reply, cap, "org %u, online %u, party %u, waypoints %u%s", (unsigned)os_count_org(&model),
             (unsigned)os_count_online(&model), (unsigned)os_count_party(&model), (unsigned)model.nwaypoints,
             session_active ? "" : " (no session)");
    return SCO_OK;
}

static int read_zone_pos(const sco_arg* a, uint64_t* zone, double pos[3]) {
    if (a[0].v.i < 0) return 0;
    *zone = (uint64_t)a[0].v.i;
    pos[0] = a[1].v.f; pos[1] = a[2].v.f; pos[2] = a[3].v.f;
    return os_finite3(pos);
}

static sco_result cmd_waypoint_add(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    uint64_t zone;
    double pos[3];
    uint32_t id = model.next_wp_id;
    int r;
    (void)n; (void)ctx;
    if (!read_zone_pos(a + 1, &zone, pos)) { snprintf(reply, cap, "zone must be >= 0 and x, y, z finite"); return SCO_BAD_ARG; }
    r = os_waypoint_put(&model, 0, id, a[0].v.s, zone, pos);
    if (r == -1) { snprintf(reply, cap, "name: 1-%u printable bytes", (unsigned)OS_MAX_WP_NAME); return SCO_BAD_ARG; }
    if (r == -2) { snprintf(reply, cap, "waypoints full (%u)", (unsigned)OS_MAX_WAYPOINTS); return SCO_TOO_MANY; }
    {
        const os_waypoint* wp = os_find_waypoint(&model, 0, id);
        const int saved = save_waypoint(wp);
        if (model.share_waypoints) send_waypoint(wp, 0);
        snprintf(reply, cap, "waypoint %u %s%s", (unsigned)id, wp->name,
                 saved ? "" : " (not saved: storage unavailable or write failed)");
    }
    refresh_panel();
    return SCO_OK;
}

static sco_result cmd_waypoint_remove(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    os_waypoint copy;
    os_waypoint* wp;
    char key[24];
    (void)n; (void)ctx;
    if (a[0].v.i <= 0 || a[0].v.i > 0xffffffffLL) { snprintf(reply, cap, "bad id"); return SCO_BAD_ARG; }
    wp = os_find_waypoint(&model, 0, (uint32_t)a[0].v.i);
    if (!wp) { snprintf(reply, cap, "no own waypoint %lld", (long long)a[0].v.i); return SCO_FAILED; }
    copy = *wp;
    (void)os_waypoint_remove(&model, 0, copy.id);
    snprintf(key, sizeof key, "w/%u", (unsigned)copy.id);
    store_del(key);
    if (model.share_waypoints) send_waypoint(&copy, 1);
    selected_wp = -1;
    snprintf(reply, cap, "waypoint %u removed", (unsigned)copy.id);
    refresh_panel();
    return SCO_OK;
}

static sco_result cmd_self_pose(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    uint64_t zone;
    double pos[3];
    (void)n; (void)ctx;
    if (!read_zone_pos(a, &zone, pos)) { snprintf(reply, cap, "zone must be >= 0 and x, y, z finite"); return SCO_BAD_ARG; }
    model.self_pose.valid = 1;
    model.self_pose.zone = zone;
    memcpy(model.self_pose.pos, pos, sizeof pos);
    pose_dirty = 1;
    snprintf(reply, cap, "pose set in zone %llu", (unsigned long long)zone);
    return SCO_OK;
}

static sco_result cmd_status(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    uint32_t s;
    (void)n; (void)ctx;
    if (!os_status_parse(a[0].v.s, &s)) { snprintf(reply, cap, "status: available, busy or away"); return SCO_BAD_ARG; }
    model.self_status = s;
    hello_due = 1;
    snprintf(reply, cap, "status %s", os_status_name(s));
    return SCO_OK;
}

static sco_result cmd_share(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    const int on = a[0].v.i != 0;
    (void)n; (void)ctx;
    if (on != model.share_waypoints) {
        if (!on) send_own_waypoints(1);
        model.share_waypoints = on;
        if (on) send_own_waypoints(0);
    }
    (void)store_text("cfg/share", on ? "1" : "0");
    snprintf(reply, cap, "waypoint sharing %s", on ? "on" : "off");
    refresh_panel();
    return SCO_OK;
}

static sco_result cmd_billboards(const sco_arg* a, uint32_t n, void* ctx, char* reply, uint32_t cap) {
    os_json j;
    int ok;
    (void)a; (void)n; (void)ctx;
    os_json_init(&j, reply, cap ? cap - 1 : 0);
    build_billboards(&j, "", 0, &ok);
    if (j.len <= j.cap) { reply[j.len] = 0; return SCO_OK; }
    snprintf(reply, cap, "{\"schema\":\"org_social.billboards/1\",\"truncated\":true,\"bytes\":%llu,"
             "\"raw\":\"org_social.billboards_json\"}", (unsigned long long)j.len);
    return SCO_OK;
}

/* ---- widgets panel ------------------------------------------------------------------------- */

static void widget_invoke(const char* name, const sco_arg* args, uint32_t nargs) {
    (void)api->invoke(me, name, args, nargs, NULL, NULL);
}

static void on_widget(const char* panel, const sco_widget_event* ev, void* ctx) {
    sco_arg args[2];
    (void)panel; (void)ctx;
    if (!ev || ev->size < sizeof(*ev)) return;
    memset(args, 0, sizeof args);
    if (ev->kind == SCO_WIDGET_EVENT_PAGE) {
        const sco_widget_event_1_1* e11 = (const sco_widget_event_1_1*)ev;
        if (ev->size >= sizeof(*e11) && ev->node_id == N_TABLE && e11->row >= 0) {
            table_first_row = (uint32_t)e11->row;
            panel_dirty = 1;
        }
        return;
    }
    switch (ev->node_id) {
    case N_HANDLE:
        copy_text(pending_handle, sizeof pending_handle, ev->text);
        break;
    case N_RANK:
        if (ev->index >= 0 && (uint32_t)ev->index < OS_ORG_RANKS) pending_rank = (uint32_t)ev->index;
        break;
    case N_WAYPOINTS:
        selected_wp = ev->index;
        break;
    case N_ADD:
    case N_REMOVE:
        if (!os_handle_valid(pending_handle)) { api->status(me, "enter a handle first"); break; }
        args[0].type = SCO_ARG_STRING; args[0].v.s = pending_handle;
        args[1].type = SCO_ARG_STRING; args[1].v.s = os_rank_name(pending_rank);
        if (ev->node_id == N_ADD) widget_invoke("org_social.member_add", args, 2);
        else widget_invoke("org_social.member_remove", args, 1);
        break;
    case N_SHARE:
        args[0].type = SCO_ARG_BOOL; args[0].v.i = ev->value != 0.0 ? 1 : 0;
        widget_invoke("org_social.share", args, 1);
        break;
    case N_WP_REMOVE:
        if (selected_wp < 0 || (uint32_t)selected_wp >= model.nwaypoints ||
            model.waypoints[selected_wp].owner_peer != 0) {
            api->status(me, "select one of your own waypoints first");
            break;
        }
        args[0].type = SCO_ARG_INT; args[0].v.i = model.waypoints[selected_wp].id;
        widget_invoke("org_social.waypoint_remove", args, 1);
        break;
    default:
        break;
    }
}

static void refresh_panel(void) {
    static sco_widget_node_1_1 nodes[N_COUNT];
    static const char* cells[(TABLE_MAX_ROWS + 1) * TABLE_COLUMNS];
    static const char* wp_items[OS_MAX_WAYPOINTS];
    static char wp_labels[OS_MAX_WAYPOINTS][OS_MAX_WP_NAME + 40];
    static char summary[128];
    static const char* ranks[OS_ORG_RANKS];
    uint32_t i, rows = 0, ncells, base_size;
    panel_dirty = 0;
    if (!widgets) return;
    base_size = widgets_11 ? (uint32_t)sizeof(sco_widget_node_1_1) : (uint32_t)sizeof(sco_widget_node);
    memset(nodes, 0, sizeof nodes);
    for (i = 0; i < N_COUNT; ++i) { nodes[i].base.size = base_size; nodes[i].base.id = i + 1; nodes[i].base.parent = N_ROOT; }
    for (i = 0; i < OS_ORG_RANKS; ++i) ranks[i] = os_rank_name(i);

    nodes[N_ROOT - 1].base.kind = SCO_WIDGET_COLUMN;
    nodes[N_ROOT - 1].base.parent = 0;

    snprintf(summary, sizeof summary, "Org %u | online %u | party %u%s", (unsigned)os_count_org(&model),
             (unsigned)os_count_online(&model), (unsigned)os_count_party(&model),
             session_active ? "" : " | no session");
    nodes[N_SUMMARY - 1].base.kind = SCO_WIDGET_LABEL;
    nodes[N_SUMMARY - 1].base.label = summary;

    cells[0] = "Handle"; cells[1] = "Rank"; cells[2] = "Online"; cells[3] = "Status";
    for (i = 0; i < model.nmembers && rows < TABLE_MAX_ROWS; ++i, ++rows) {
        const os_member* mem = &model.members[i];
        const char** row = &cells[(rows + 1) * TABLE_COLUMNS];
        row[0] = mem->handle;
        row[1] = os_rank_name(mem->rank);
        row[2] = mem->peer_id ? "online" : "offline";
        row[3] = mem->peer_id ? os_status_name(mem->status) : "-";
    }
    ncells = (rows + 1) * TABLE_COLUMNS;
    nodes[N_TABLE - 1].base.kind = SCO_WIDGET_TABLE;
    nodes[N_TABLE - 1].base.label = "Roster";
    nodes[N_TABLE - 1].base.items = cells;
    nodes[N_TABLE - 1].base.item_count = ncells;
    nodes[N_TABLE - 1].base.columns = TABLE_COLUMNS;
    if (widgets_11 && rows + 1 > TABLE_PAGE) {
        if (table_first_row >= rows + 1) table_first_row = 0;
        nodes[N_TABLE - 1].visible_rows = TABLE_PAGE;
        nodes[N_TABLE - 1].first_row = table_first_row;
    }

    if (model.nwaypoints) {
        for (i = 0; i < model.nwaypoints; ++i) {
            const os_waypoint* wp = &model.waypoints[i];
            snprintf(wp_labels[i], sizeof wp_labels[i], "%s (%s %u, zone %llu)", wp->name,
                     wp->owner_peer ? "shared" : "own", (unsigned)wp->id, (unsigned long long)wp->zone);
            wp_items[i] = wp_labels[i];
        }
        if (selected_wp >= (int32_t)model.nwaypoints) selected_wp = -1;
        nodes[N_WAYPOINTS - 1].base.kind = SCO_WIDGET_LIST;
        nodes[N_WAYPOINTS - 1].base.label = "Waypoints";
        nodes[N_WAYPOINTS - 1].base.items = wp_items;
        nodes[N_WAYPOINTS - 1].base.item_count = model.nwaypoints;
        nodes[N_WAYPOINTS - 1].base.index = selected_wp;
    } else {
        selected_wp = -1;
        nodes[N_WAYPOINTS - 1].base.kind = SCO_WIDGET_LABEL;
        nodes[N_WAYPOINTS - 1].base.label = "Waypoints: none";
    }

    nodes[N_EDIT_ROW - 1].base.kind = SCO_WIDGET_ROW;
    nodes[N_HANDLE - 1].base.kind = SCO_WIDGET_TEXT_INPUT;
    nodes[N_HANDLE - 1].base.parent = N_EDIT_ROW;
    nodes[N_HANDLE - 1].base.label = "Handle";
    nodes[N_HANDLE - 1].base.text = pending_handle;
    nodes[N_RANK - 1].base.kind = SCO_WIDGET_COMBO;
    nodes[N_RANK - 1].base.parent = N_EDIT_ROW;
    nodes[N_RANK - 1].base.label = "Rank";
    nodes[N_RANK - 1].base.items = ranks;
    nodes[N_RANK - 1].base.item_count = OS_ORG_RANKS;
    nodes[N_RANK - 1].base.index = (int32_t)pending_rank;
    nodes[N_ADD - 1].base.kind = SCO_WIDGET_BUTTON;
    nodes[N_ADD - 1].base.parent = N_EDIT_ROW;
    nodes[N_ADD - 1].base.label = "Add or set rank";
    nodes[N_REMOVE - 1].base.kind = SCO_WIDGET_BUTTON;
    nodes[N_REMOVE - 1].base.parent = N_EDIT_ROW;
    nodes[N_REMOVE - 1].base.label = "Remove member";

    nodes[N_WP_ROW - 1].base.kind = SCO_WIDGET_ROW;
    nodes[N_SHARE - 1].base.kind = SCO_WIDGET_TOGGLE;
    nodes[N_SHARE - 1].base.parent = N_WP_ROW;
    nodes[N_SHARE - 1].base.label = "Share waypoints";
    nodes[N_SHARE - 1].base.value = model.share_waypoints ? 1.0 : 0.0;
    nodes[N_WP_REMOVE - 1].base.kind = SCO_WIDGET_BUTTON;
    nodes[N_WP_REMOVE - 1].base.parent = N_WP_ROW;
    nodes[N_WP_REMOVE - 1].base.label = "Remove waypoint";

    if (widgets->set_panel(me, PANEL_ID, "Org & party", &nodes[0].base, N_COUNT, (uint32_t)sizeof nodes[0],
                           on_widget, NULL) != SCO_OK)
        say(SCO_LOG_WARN, "roster panel rejected by sco.widgets");
}

/* ---- load / unload ------------------------------------------------------------------------- */

static sco_result add_command(const char* name, const char* title, const char* help, const sco_arg_def* defs,
                              uint32_t nargs, sco_command_fn fn) {
    sco_command c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.name = name;
    c.title = title;
    c.help = help;
    c.args = defs;
    c.nargs = nargs;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = fn;
    return api->register_command(me, &c);
}

static sco_result register_commands(void) {
    static const sco_arg_def member_add[2] = {
        { "handle", SCO_ARG_STRING, 0, "Player handle, 1-63 printable bytes" },
        { "rank", SCO_ARG_STRING, 0, "recruit, member, officer, director or founder" } };
    static const sco_arg_def handle_only[1] = { { "handle", SCO_ARG_STRING, 0, "Player handle" } };
    static const sco_arg_def waypoint_add[5] = {
        { "name", SCO_ARG_STRING, 0, "Waypoint name, 1-47 printable bytes" },
        { "zone", SCO_ARG_INT, 0, "game.spatial zone id (0 = world)" },
        { "x", SCO_ARG_FLOAT, 0, "metres" }, { "y", SCO_ARG_FLOAT, 0, "metres" }, { "z", SCO_ARG_FLOAT, 0, "metres" } };
    static const sco_arg_def id_only[1] = { { "id", SCO_ARG_INT, 0, "Own waypoint id" } };
    static const sco_arg_def pose[4] = {
        { "zone", SCO_ARG_INT, 0, "game.spatial zone id (0 = world)" },
        { "x", SCO_ARG_FLOAT, 0, "metres" }, { "y", SCO_ARG_FLOAT, 0, "metres" }, { "z", SCO_ARG_FLOAT, 0, "metres" } };
    static const sco_arg_def status[1] = { { "status", SCO_ARG_STRING, 0, "available, busy or away" } };
    static const sco_arg_def share[1] = { { "enabled", SCO_ARG_BOOL, 0, "Share own waypoints with the party" } };
    sco_result r = SCO_OK;
    if (r == SCO_OK) r = add_command("org_social.member_add", "Add org member", "Adds a handle or sets its rank", member_add, 2, cmd_member_add);
    if (r == SCO_OK) r = add_command("org_social.member_remove", "Remove org member", NULL, handle_only, 1, cmd_member_remove);
    if (r == SCO_OK) r = add_command("org_social.roster", "Roster summary", NULL, NULL, 0, cmd_roster);
    if (r == SCO_OK) r = add_command("org_social.waypoint_add", "Add waypoint", NULL, waypoint_add, 5, cmd_waypoint_add);
    if (r == SCO_OK) r = add_command("org_social.waypoint_remove", "Remove waypoint", NULL, id_only, 1, cmd_waypoint_remove);
    if (r == SCO_OK) r = add_command("org_social.self_pose", "Set own pose", "Pose shared with the party", pose, 4, cmd_self_pose);
    if (r == SCO_OK) r = add_command("org_social.status", "Set status", NULL, status, 1, cmd_status);
    if (r == SCO_OK) r = add_command("org_social.share", "Share waypoints", NULL, share, 1, cmd_share);
    if (r == SCO_OK) r = add_command("org_social.billboards", "Billboards (JSON)", "World-frame billboard JSON", NULL, 0, cmd_billboards);
    return r;
}

static const void* find_service(const char* name, uint32_t version) {
    const void* found = NULL;
    if (!API_HAS(query_service) || api->query_service(name, version, &found) != SCO_OK) return NULL;
    return found;
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &info; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* host, sco_plugin* self) {
    sco_result r;
    api = host;
    me = self;
    st = NULL; net = NULL; widgets = NULL;
    widgets_11 = 0; panel_dirty = 0; session_active = 0; hello_due = 0; pose_dirty = 0; ticks = 0;
    pending_handle[0] = 0; pending_rank = OS_RANK_MEMBER; selected_wp = -1; table_first_row = 0;
    os_model_init(&model);

    r = register_commands();
    if (r != SCO_OK) return r;

    st = (const sco_storage_v1*)find_service(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0);
    if (st && st->size < offsetof(sco_storage_v1, last_error) + sizeof(st->last_error)) st = NULL;
    if (st) load_store(); else say(SCO_LOG_INFO, "no sco.storage: the roster lives in memory only");

    net = (const sco_net_v1*)find_service(SCO_NET_NAME, SCO_NET_VERSION_1_0);
    if (net && net->size < offsetof(sco_net_v1, self_peer) + sizeof(net->self_peer)) net = NULL;
    if (net) {
        if (net->register_channel(me, CH_ROSTER, SCO_NET_RELIABLE, OS_WIRE_MAX_ROSTER, on_message, NULL) != SCO_OK ||
            net->register_channel(me, CH_POSE, 0, OS_WIRE_MAX_POSE, on_message, &pose_channel_context) != SCO_OK) {
            say(SCO_LOG_WARN, "sco.net channels not registered: party features off");
            net = NULL;
        } else {
            (void)api->subscribe(me, SCO_NET_EVENT_STATE, on_net_event, NULL);
            (void)api->subscribe(me, SCO_NET_EVENT_PEER, on_net_event, NULL);
            refresh_session();
        }
    } else {
        say(SCO_LOG_INFO, "no sco.net: party presence and sharing off");
    }
    (void)api->subscribe(me, "tick", on_tick, NULL);

    if (API_HAS(register_raw)) {
        (void)api->register_raw(me, "org_social.roster_json", NULL, raw_roster, NULL);
        (void)api->register_raw(me, "org_social.billboards_json", NULL, raw_billboards, NULL);
    }

    widgets = (const sco_widgets_v1*)find_service(SCO_WIDGETS_NAME, SCO_WIDGETS_VERSION_1_0);
    if (widgets && (widgets->size < offsetof(sco_widgets_v1, remove_panel) + sizeof(widgets->remove_panel) ||
                    !widgets->set_panel || !widgets->remove_panel)) widgets = NULL;
    widgets_11 = widgets && widgets->size >= offsetof(sco_widgets_v1, capabilities) + sizeof(widgets->capabilities);
    refresh_panel();
    say(SCO_LOG_INFO, "loaded");
    return SCO_OK;
}

SCO_EXPORT void sco_plugin_unload(void) {
    if (widgets) (void)widgets->remove_panel(me, PANEL_ID);
    widgets = NULL; net = NULL; st = NULL;
}

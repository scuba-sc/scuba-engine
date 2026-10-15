/* SPDX-License-Identifier: GPL-3.0-only
 * mp_chat: room-aware multiplayer chat over sco.net 1.0, with local history in sco.storage 1.0
 * and a sco.widgets panel (1.1 paging and severity when the host has them, 1.0 otherwise).
 *
 * Every service is optional: without sco.net the panel and history still work and sending
 * answers SCO_UNAVAILABLE. sco_api 1.1 members and every service member are called only after
 * the table's size is checked to cover them, so the plugin also loads on a 1.0 host.
 * All callbacks (commands, events, net messages, widget events) run on the game thread.
 */
#include "chat_model.h"
#include "sco_api.h"
#include "sco_net.h"
#include "sco_storage.h"
#include "sco_widgets.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MP_ID "mp_chat"
#define MP_CHANNEL "mp_chat.msg"
#define MP_PANEL "mp_chat.main"
#define MP_VISIBLE_ROWS 8u
#define MP_STORED_ROWS 2048

#define MP_COVERS(tbl, type, member) \
    ((tbl)->size >= offsetof(type, member) + sizeof(((const type*)0)->member) && (tbl)->member != NULL)

enum {
    NODE_ROOT = 1,
    NODE_STATUS,
    NODE_ROOM_ROW,
    NODE_ROOM,
    NODE_CLEAR,
    NODE_HISTORY,
    NODE_INPUT_ROW,
    NODE_INPUT,
    NODE_SEND,
    NODE_COUNT = NODE_SEND
};

#define DIRTY_STATUS 0x1u
#define DIRTY_HISTORY 0x2u
#define DIRTY_TREE 0x4u

typedef struct mp_peer_name {
    uint64_t id;
    char name[MPC_MAX_NAME + 1u];
} mp_peer_name;

static const sco_api* g_api;
static sco_plugin* g_self;
static const sco_net_v1* g_net;
static const sco_storage_v1* g_store;
static const sco_widgets_v1* g_widgets;
static int g_widgets_11;
static int g_update_nodes;
static int g_paging;
static int g_severity;
static int g_channel;
static int g_panel;
static unsigned g_dirty;
static mpc_state g_chat;
static char g_draft[MPC_MAX_TEXT + 1u];
static char g_status[MPC_LINE];
static uint32_t g_status_severity;
static mp_peer_name g_peers[SCO_NET_MAX_PEERS];
static uint32_t g_dropped;

static sco_widget_node_1_1 g_nodes[NODE_COUNT];
static const char* g_history_items[MPC_HISTORY];
static char g_room_labels[MPC_MAX_ROOMS][MPC_MAX_ROOM + 16u];
static const char* g_room_items[MPC_MAX_ROOMS];

static const sco_plugin_info g_info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, MP_ID, "0.1.0", "scuba-engine"
};

static void log_line(sco_log_level level, const char* text) {
    if (g_api) g_api->log(g_self, level, text);
}

static void set_status(uint32_t severity, const char* text) {
    size_t len = 0;
    g_status[0] = 0;
    mpc_append_clean(g_status, sizeof g_status, &len, text);
    g_status_severity = severity;
    g_dirty |= DIRTY_STATUS;
}

static int session_active(void) { return g_net && g_net->is_active(); }

static uint32_t peer_count(void) {
    uint32_t n = 0;
    if (!session_active()) return 0;
    if (g_net->get_peers(NULL, &n) != SCO_OK && n == 0) return 0;
    return n;
}

static void peer_name(uint64_t peer, char out[MPC_MAX_NAME + 1u]) {
    uint32_t i;
    out[0] = 0;
    if (g_net && g_net->get_peer_name(peer, out, MPC_MAX_NAME + 1u) == SCO_OK && out[0]) return;
    for (i = 0; i < SCO_NET_MAX_PEERS; ++i)
        if (g_peers[i].id == peer && g_peers[i].name[0]) {
            memcpy(out, g_peers[i].name, sizeof g_peers[i].name);
            return;
        }
    snprintf(out, MPC_MAX_NAME + 1u, "peer %llu", (unsigned long long)peer);
}

static void self_name(char out[MPC_MAX_NAME + 1u]) {
    const uint64_t me = session_active() ? g_net->self_peer() : 0u;
    if (me) peer_name(me, out);
    else snprintf(out, MPC_MAX_NAME + 1u, "me");
}

static void mark_room_changed(const mpc_room* room) {
    if (room == mpc_current(&g_chat)) g_dirty |= DIRTY_HISTORY;
    else g_dirty |= DIRTY_TREE;
}

static void add_line(mpc_room* room, uint32_t severity, const char* sender, const char* text) {
    mpc_add(&g_chat, room, severity, sender, text);
    if (room != mpc_current(&g_chat)) ++room->unread;
    mark_room_changed(room);
}

static void notice(uint32_t severity, const char* text) {
    add_line(mpc_current(&g_chat), severity, NULL, text);
    set_status(severity, text);
}

static sco_sql_value text_param(const char* s) {
    sco_sql_value v;
    memset(&v, 0, sizeof v);
    v.type = SCO_SQL_TEXT;
    v.size = (uint32_t)strlen(s);
    v.v.p = s;
    return v;
}

static void store_line(const char* room, const char* sender, const char* text) {
    sco_sql_value p[3];
    if (!g_store) return;
    p[0] = text_param(room);
    p[1] = text_param(sender);
    p[2] = text_param(text);
    if (g_store->exec(g_self, "INSERT INTO mp_chat_history(room, sender, body) VALUES (?, ?, ?)", p, 3, NULL) != SCO_OK) {
        log_line(SCO_LOG_WARN, "mp_chat: could not store a chat line");
        return;
    }
    (void)g_store->exec(g_self,
                        "DELETE FROM mp_chat_history WHERE room = ?1 AND id NOT IN "
                        "(SELECT id FROM mp_chat_history WHERE room = ?1 ORDER BY id DESC LIMIT 256)",
                        p, 1, NULL);
}

static int column_text(uint64_t cursor, uint32_t index, char* buf, uint32_t cap) {
    sco_sql_value v;
    uint32_t size = cap;
    if (g_store->column(g_self, cursor, index, &v, buf, &size) != SCO_OK) return 0;
    return v.type == SCO_SQL_TEXT;
}

static void load_history(void) {
    uint64_t cursor = 0;
    char room[MPC_MAX_ROOM + 1u], sender[MPC_MAX_NAME + 1u], text[MPC_MAX_TEXT + 1u];
    char current[MPC_MAX_ROOM + 1u];
    uint32_t size = sizeof current - 1u;
    if (g_store->exec(g_self,
                      "CREATE TABLE IF NOT EXISTS mp_chat_history("
                      "id INTEGER PRIMARY KEY, room TEXT NOT NULL, sender TEXT NOT NULL, body TEXT NOT NULL)",
                      NULL, 0, NULL) != SCO_OK) {
        log_line(SCO_LOG_WARN, "mp_chat: history table unavailable; history is not kept");
        g_store = NULL;
        return;
    }
    if (g_store->query(g_self,
                       "SELECT room, sender, body FROM (SELECT id, room, sender, body FROM mp_chat_history "
                       "ORDER BY id DESC LIMIT 2048) ORDER BY id",
                       NULL, 0, &cursor) == SCO_OK) {
        while (g_store->step(g_self, cursor) == SCO_OK) {
            int idx;
            if (!column_text(cursor, 0, room, sizeof room) || !column_text(cursor, 1, sender, sizeof sender) ||
                !column_text(cursor, 2, text, sizeof text))
                continue;
            if (!mpc_valid_text(text, strlen(text))) continue;
            idx = mpc_room_index(&g_chat, room, 1);
            if (idx >= 0) mpc_add(&g_chat, &g_chat.rooms[idx], MPC_SEV_NONE, sender, text);
        }
        (void)g_store->close(g_self, cursor);
    }
    memset(current, 0, sizeof current);
    if (g_store->get(g_self, "current_room", current, &size) == SCO_OK) {
        const int idx = mpc_room_index(&g_chat, current, 1);
        if (idx >= 0) g_chat.current = (uint32_t)idx;
    }
}

static void store_current_room(void) {
    const char* name = mpc_current(&g_chat)->name;
    if (g_store) (void)g_store->put(g_self, "current_room", name, (uint32_t)strlen(name));
}

static sco_widget_node_1_1* node_init(uint32_t id, uint32_t kind, uint32_t parent, const char* label) {
    sco_widget_node_1_1* n = &g_nodes[id - 1u];
    memset(n, 0, sizeof *n);
    n->base.size = g_widgets_11 ? (uint32_t)sizeof(sco_widget_node_1_1) : (uint32_t)sizeof(sco_widget_node);
    n->base.kind = kind;
    n->base.id = id;
    n->base.parent = parent;
    n->base.label = label;
    return n;
}

static void build_nodes(void) {
    mpc_room* room = mpc_current(&g_chat);
    sco_widget_node_1_1* n;
    uint32_t i;

    node_init(NODE_ROOT, SCO_WIDGET_COLUMN, 0, NULL);

    n = node_init(NODE_STATUS, SCO_WIDGET_LABEL, NODE_ROOT, g_status);
    if (g_severity) n->severity = g_status_severity;

    node_init(NODE_ROOM_ROW, SCO_WIDGET_ROW, NODE_ROOT, NULL);
    n = node_init(NODE_ROOM, SCO_WIDGET_COMBO, NODE_ROOM_ROW, "Room");
    for (i = 0; i < g_chat.nrooms; ++i) {
        const mpc_room* r = &g_chat.rooms[i];
        if (r->unread) snprintf(g_room_labels[i], sizeof g_room_labels[i], "#%s (%u)", r->name, (unsigned)r->unread);
        else snprintf(g_room_labels[i], sizeof g_room_labels[i], "#%s", r->name);
        g_room_items[i] = g_room_labels[i];
    }
    n->base.items = g_room_items;
    n->base.item_count = g_chat.nrooms;
    n->base.index = (int32_t)g_chat.current;
    node_init(NODE_CLEAR, SCO_WIDGET_BUTTON, NODE_ROOM_ROW, "Clear history");

    n = node_init(NODE_HISTORY, SCO_WIDGET_LIST, NODE_ROOT, "Messages");
    for (i = 0; i < room->count; ++i) g_history_items[i] = mpc_at(room, i)->line;
    n->base.items = room->count ? g_history_items : NULL;
    n->base.item_count = room->count;
    n->base.index = -1;
    if (g_widgets_11) {
        n->tooltip = "Newest messages last; page up for older ones";
        if (g_paging) {
            n->visible_rows = g_chat.visible;
            n->first_row = room->count ? room->first_row : 0u;
        }
        if (g_severity)
            n->severity = mpc_max_severity(room, g_paging ? n->first_row : 0u, g_paging ? g_chat.visible : MPC_HISTORY);
    }

    node_init(NODE_INPUT_ROW, SCO_WIDGET_ROW, NODE_ROOT, NULL);
    n = node_init(NODE_INPUT, SCO_WIDGET_TEXT_INPUT, NODE_INPUT_ROW, "Message");
    n->base.text = g_draft;
    n = node_init(NODE_SEND, SCO_WIDGET_BUTTON, NODE_INPUT_ROW, "Send");
    if (!session_active()) n->base.flags = SCO_WIDGET_DISABLED;
}

static void on_widget(const char* panel, const sco_widget_event* ev, void* ctx);

static void refresh_panel(void) {
    sco_result r = SCO_FAILED;
    if (!g_widgets || !g_dirty) return;
    build_nodes();
    if (g_panel && g_update_nodes && !(g_dirty & DIRTY_TREE)) {
        r = SCO_OK;
        if (g_dirty & DIRTY_STATUS)
            r = g_widgets->update_nodes(g_self, MP_PANEL, &g_nodes[NODE_STATUS - 1u].base, 1, sizeof(sco_widget_node_1_1));
        if (r == SCO_OK && (g_dirty & DIRTY_HISTORY))
            r = g_widgets->update_nodes(g_self, MP_PANEL, &g_nodes[NODE_HISTORY - 1u].base, 1, sizeof(sco_widget_node_1_1));
    }
    if (r != SCO_OK) {
        r = g_widgets->set_panel(g_self, MP_PANEL, "Chat", &g_nodes[0].base, NODE_COUNT, sizeof(sco_widget_node_1_1),
                                 on_widget, NULL);
        g_panel = r == SCO_OK;
        if (r != SCO_OK) {
            char msg[96];
            snprintf(msg, sizeof msg, "mp_chat: set_panel failed (%d)", (int)r);
            log_line(SCO_LOG_WARN, msg);
        }
    }
    g_dirty = 0;
}

static void detect_widgets(void) {
    const void* table = NULL;
    uint32_t caps[SCO_WIDGETS_CAPS_1_1];
    g_widgets = NULL;
    g_widgets_11 = g_update_nodes = g_paging = g_severity = 0;
    if (g_api->query_service(SCO_WIDGETS_NAME, SCO_WIDGETS_VERSION_1_0, &table) != SCO_OK || !table) return;
    g_widgets = (const sco_widgets_v1*)table;
    if (!MP_COVERS(g_widgets, sco_widgets_v1, set_panel) || !MP_COVERS(g_widgets, sco_widgets_v1, remove_panel)) {
        g_widgets = NULL;
        return;
    }
    if (!MP_COVERS(g_widgets, sco_widgets_v1, capabilities)) return;
    memset(caps, 0, sizeof caps);
    caps[SCO_WIDGETS_CAP_COUNT] = SCO_WIDGETS_CAPS_1_1;
    if (g_widgets->capabilities(g_self, caps) != SCO_OK || caps[SCO_WIDGETS_CAP_COUNT] <= SCO_WIDGETS_CAP_FEATURES) return;
    g_widgets_11 = 1;
    g_update_nodes = (caps[SCO_WIDGETS_CAP_FEATURES] & SCO_WIDGETS_FEATURE_UPDATE_NODES) &&
                     MP_COVERS(g_widgets, sco_widgets_v1, update_nodes);
    g_paging = (caps[SCO_WIDGETS_CAP_FEATURES] & SCO_WIDGETS_FEATURE_PAGING) != 0;
    g_severity = (caps[SCO_WIDGETS_CAP_FEATURES] & SCO_WIDGETS_FEATURE_SEVERITY) != 0;
}

static sco_result send_text(const char* text, char* reply, uint32_t reply_size) {
    uint8_t wire[MPC_WIRE_MAX];
    const mpc_room* room = mpc_current(&g_chat);
    char me[MPC_MAX_NAME + 1u];
    size_t len;
    sco_result r;
    if (!text || !mpc_valid_text(text, strlen(text))) {
        snprintf(reply, reply_size, "message must be 1-%u bytes of text", (unsigned)MPC_MAX_TEXT);
        return SCO_BAD_ARG;
    }
    if (!g_net) {
        notice(MPC_SEV_WARNING, "Not sent: multiplayer (sco.net) is not available on this host");
        snprintf(reply, reply_size, "sco.net is not available");
        return SCO_UNAVAILABLE;
    }
    if (!g_net->is_active()) {
        notice(MPC_SEV_WARNING, "Not sent: not in a multiplayer session");
        snprintf(reply, reply_size, "not in a session");
        return SCO_UNAVAILABLE;
    }
    len = mpc_encode(room->name, text, wire, sizeof wire);
    if (!len) return SCO_BAD_ARG;
    r = g_net->send_channel(g_self, MP_CHANNEL, wire, (uint32_t)len);
    if (r != SCO_OK) {
        notice(r == SCO_TOO_MANY ? MPC_SEV_WARNING : MPC_SEV_ERROR,
               r == SCO_TOO_MANY ? "Not sent: sending too fast" : "Not sent: the session refused the message");
        snprintf(reply, reply_size, "send failed (%d)", (int)r);
        return r;
    }
    self_name(me);
    add_line(mpc_current(&g_chat), MPC_SEV_NONE, me, text);
    store_line(room->name, me, text);
    snprintf(reply, reply_size, "sent to #%s", room->name);
    return SCO_OK;
}

static int switch_room(const char* name, int create) {
    const int idx = mpc_room_index(&g_chat, name, create);
    if (idx < 0) return 0;
    g_chat.current = (uint32_t)idx;
    g_chat.rooms[idx].unread = 0;
    store_current_room();
    g_dirty |= DIRTY_TREE;
    return 1;
}

static void clear_room(void) {
    mpc_room* room = mpc_current(&g_chat);
    if (g_store) {
        sco_sql_value p = text_param(room->name);
        (void)g_store->exec(g_self, "DELETE FROM mp_chat_history WHERE room = ?", &p, 1, NULL);
    }
    mpc_clear(&g_chat, room);
    g_dirty |= DIRTY_HISTORY;
}

static void on_widget(const char* panel, const sco_widget_event* ev, void* ctx) {
    char reply[128];
    (void)panel;
    (void)ctx;
    if (!ev || ev->size < sizeof(*ev)) return;
    if (ev->kind == SCO_WIDGET_EVENT_PAGE) {
        const sco_widget_event_1_1* x = (const sco_widget_event_1_1*)ev;
        if (ev->node_id == NODE_HISTORY && ev->size >= sizeof(*x) &&
            mpc_page(&g_chat, mpc_current(&g_chat), x->row))
            g_dirty |= DIRTY_HISTORY;
        return;
    }
    switch (ev->node_id) {
        case NODE_ROOM:
            if (ev->kind == SCO_WIDGET_COMBO && ev->index >= 0 && (uint32_t)ev->index < g_chat.nrooms)
                (void)switch_room(g_chat.rooms[ev->index].name, 0);
            break;
        case NODE_CLEAR:
            if (ev->kind == SCO_WIDGET_BUTTON) clear_room();
            break;
        case NODE_INPUT:
            if (ev->kind == SCO_WIDGET_TEXT_INPUT) {
                size_t len = 0;
                g_draft[0] = 0;
                if (ev->text) mpc_append_clean(g_draft, sizeof g_draft, &len, ev->text);
            }
            break;
        case NODE_SEND:
            if (ev->kind == SCO_WIDGET_BUTTON && g_draft[0] && send_text(g_draft, reply, sizeof reply) == SCO_OK) {
                g_draft[0] = 0;
                g_dirty |= DIRTY_TREE;
            }
            break;
        default:
            break;
    }
}

static void on_message(uint64_t sender, const void* buf, uint32_t len, void* ctx) {
    char room[MPC_MAX_ROOM + 1u], text[MPC_MAX_TEXT + 1u], name[MPC_MAX_NAME + 1u];
    int idx;
    (void)ctx;
    if (!mpc_decode((const uint8_t*)buf, len, room, text)) {
        if (++g_dropped == 1u || g_dropped % 100u == 0u) {
            char msg[96];
            snprintf(msg, sizeof msg, "mp_chat: dropped %u malformed message(s)", (unsigned)g_dropped);
            log_line(SCO_LOG_WARN, msg);
        }
        return;
    }
    idx = mpc_room_index(&g_chat, room, 1);
    if (idx < 0) return;
    peer_name(sender, name);
    add_line(&g_chat.rooms[idx], MPC_SEV_NONE, name, text);
    store_line(room, name, text);
}

static void remember_peer(uint64_t id) {
    uint32_t i, slot = SCO_NET_MAX_PEERS;
    for (i = 0; i < SCO_NET_MAX_PEERS; ++i) {
        if (g_peers[i].id == id) { slot = i; break; }
        if (!g_peers[i].id && slot == SCO_NET_MAX_PEERS) slot = i;
    }
    if (slot == SCO_NET_MAX_PEERS) return;
    g_peers[slot].id = id;
    if (!g_net || g_net->get_peer_name(id, g_peers[slot].name, sizeof g_peers[slot].name) != SCO_OK)
        g_peers[slot].name[0] = 0;
}

static void forget_peer(uint64_t id) {
    uint32_t i;
    for (i = 0; i < SCO_NET_MAX_PEERS; ++i)
        if (g_peers[i].id == id) memset(&g_peers[i], 0, sizeof g_peers[i]);
}

static void on_peer(const char* event, const void* data, void* ctx) {
    const sco_net_peer_event* pe = (const sco_net_peer_event*)data;
    char name[MPC_MAX_NAME + 1u], text[MPC_MAX_NAME + 16u];
    (void)event;
    (void)ctx;
    if (!pe || pe->size < sizeof(*pe)) return;
    if (pe->what == SCO_NET_PEER_JOINED) {
        remember_peer(pe->peer.peer_id);
        peer_name(pe->peer.peer_id, name);
        snprintf(text, sizeof text, "%s joined", name);
        notice(MPC_SEV_INFO, text);
    } else if (pe->what == SCO_NET_PEER_LEFT) {
        peer_name(pe->peer.peer_id, name);
        forget_peer(pe->peer.peer_id);
        snprintf(text, sizeof text, "%s left", name);
        notice(MPC_SEV_INFO, text);
    }
    g_dirty |= DIRTY_TREE;
}

static void on_state(const char* event, const void* data, void* ctx) {
    const sco_net_state_event* se = (const sco_net_state_event*)data;
    char text[192];
    char reason[sizeof se->reason];
    size_t len = 0;
    (void)event;
    (void)ctx;
    if (!se || se->size < sizeof(*se)) return;
    memset(g_peers, 0, sizeof g_peers);
    if (se->active) {
        notice(MPC_SEV_INFO, "Session started");
    } else {
        memcpy(reason, se->reason, sizeof reason);
        reason[sizeof reason - 1u] = 0;
        text[0] = 0;
        mpc_append_clean(text, sizeof text, &len, "Session ended");
        if (reason[0]) {
            mpc_append_clean(text, sizeof text, &len, ": ");
            mpc_append_clean(text, sizeof text, &len, reason);
        }
        notice(MPC_SEV_WARNING, text);
    }
    g_dirty |= DIRTY_TREE;
}

static void on_tick(const char* event, const void* data, void* ctx) {
    (void)event;
    (void)data;
    (void)ctx;
    refresh_panel();
}

static sco_result cmd_send(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    sco_result r;
    (void)ctx;
    if (nargs != 1 || args[0].type != SCO_ARG_STRING) return SCO_BAD_ARG;
    r = send_text(args[0].v.s, reply, reply_size);
    refresh_panel();
    return r;
}

static sco_result cmd_join(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    (void)ctx;
    if (nargs != 1 || args[0].type != SCO_ARG_STRING) return SCO_BAD_ARG;
    if (!mpc_valid_room(args[0].v.s)) {
        snprintf(reply, reply_size, "room names are 1-%u characters of a-z, 0-9, _ and -", (unsigned)MPC_MAX_ROOM);
        return SCO_BAD_ARG;
    }
    if (!switch_room(args[0].v.s, 1)) {
        snprintf(reply, reply_size, "at most %u rooms", (unsigned)MPC_MAX_ROOMS);
        return SCO_TOO_MANY;
    }
    refresh_panel();
    snprintf(reply, reply_size, "now in #%s", mpc_current(&g_chat)->name);
    return SCO_OK;
}

static sco_result cmd_status(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    const mpc_room* room = mpc_current(&g_chat);
    (void)args;
    (void)ctx;
    if (nargs != 0) return SCO_BAD_ARG;
    snprintf(reply, reply_size, "room=%s rooms=%u lines=%u session=%s peers=%u net=%s storage=%s widgets=%s",
             room->name, (unsigned)g_chat.nrooms, (unsigned)room->count, session_active() ? "on" : "off",
             (unsigned)peer_count(), g_net ? "yes" : "no", g_store ? "yes" : "no",
             !g_widgets ? "none" : (g_widgets_11 ? "1.1" : "1.0"));
    return SCO_OK;
}

static sco_result cmd_last(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    const mpc_entry* e = mpc_last_chat(mpc_current(&g_chat));
    (void)args;
    (void)ctx;
    if (nargs != 0) return SCO_BAD_ARG;
    snprintf(reply, reply_size, "%s", e ? e->line : "");
    return SCO_OK;
}

static sco_result cmd_clear(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    (void)args;
    (void)ctx;
    if (nargs != 0) return SCO_BAD_ARG;
    clear_room();
    refresh_panel();
    snprintf(reply, reply_size, "cleared #%s", mpc_current(&g_chat)->name);
    return SCO_OK;
}

static const sco_arg_def g_text_arg[] = { { "text", SCO_ARG_STRING, 0, "Message, 1-200 bytes of UTF-8" } };
static const sco_arg_def g_room_arg[] = { { "room", SCO_ARG_STRING, 0, "Room name: a-z, 0-9, _ and -" } };

static sco_result add_command(const char* name, const char* title, const char* help, const sco_arg_def* args,
                              uint32_t nargs, sco_command_fn fn) {
    sco_command c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.name = name;
    c.title = title;
    c.help = help;
    c.args = args;
    c.nargs = nargs;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = fn;
    return g_api->register_command(g_self, &c);
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &g_info; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* host, sco_plugin* self) {
    sco_result r;
    g_api = host;
    g_self = self;
    g_net = NULL;
    g_store = NULL;
    g_widgets = NULL;
    g_channel = g_panel = 0;
    g_draft[0] = 0;
    g_dropped = 0;
    memset(g_peers, 0, sizeof g_peers);

    if ((r = add_command("mp_chat.send", "Send chat message", "Sends a message to the current room", g_text_arg, 1, cmd_send)) != SCO_OK ||
        (r = add_command("mp_chat.join", "Join chat room", "Switches to a room, creating it locally", g_room_arg, 1, cmd_join)) != SCO_OK ||
        (r = add_command("mp_chat.status", "Chat status", "Room, session and service state", NULL, 0, cmd_status)) != SCO_OK ||
        (r = add_command("mp_chat.last", "Last chat message", "The newest chat line of the current room", NULL, 0, cmd_last)) != SCO_OK ||
        (r = add_command("mp_chat.clear", "Clear chat room", "Clears the current room's local history", NULL, 0, cmd_clear)) != SCO_OK)
        return r;

    if (!MP_COVERS(host, sco_api, query_service)) {
        mpc_init(&g_chat, 0);
        set_status(MPC_SEV_WARNING, "Chat needs a sco_api 1.1 host for its services");
        log_line(SCO_LOG_WARN, "mp_chat: sco_api 1.0 host; commands only");
        return SCO_OK;
    }

    detect_widgets();
    mpc_init(&g_chat, g_paging ? MP_VISIBLE_ROWS : 0u);

    {
        const void* t = NULL;
        if (host->query_service(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0, &t) == SCO_OK && t) {
            g_store = (const sco_storage_v1*)t;
            if (!MP_COVERS(g_store, sco_storage_v1, last_error)) g_store = NULL;
        }
        if (g_store) load_history();

        t = NULL;
        if (host->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, &t) == SCO_OK && t) {
            g_net = (const sco_net_v1*)t;
            if (!MP_COVERS(g_net, sco_net_v1, self_peer)) g_net = NULL;
        }
    }

    if (g_net) {
        r = g_net->register_channel(self, MP_CHANNEL, SCO_NET_RELIABLE, MPC_WIRE_MAX, on_message, NULL);
        if (r != SCO_OK) {
            log_line(SCO_LOG_WARN, "mp_chat: could not register the chat channel; chat is local only");
            g_net = NULL;
        } else {
            g_channel = 1;
            (void)host->subscribe(self, SCO_NET_EVENT_PEER, on_peer, NULL);
            (void)host->subscribe(self, SCO_NET_EVENT_STATE, on_state, NULL);
        }
    }

    set_status(session_active() ? MPC_SEV_INFO : MPC_SEV_NONE,
               g_net ? (session_active() ? "In a multiplayer session" : "No multiplayer session")
                     : "Multiplayer (sco.net) is not available on this host");
    if (g_widgets) {
        (void)host->subscribe(self, "tick", on_tick, NULL);
        g_dirty |= DIRTY_TREE;
        refresh_panel();
    }
    return SCO_OK;
}

SCO_EXPORT void sco_plugin_unload(void) {
    if (g_widgets && g_panel) (void)g_widgets->remove_panel(g_self, MP_PANEL);
    if (g_net && g_channel) (void)g_net->unregister_channel(g_self, MP_CHANNEL);
    g_widgets = NULL;
    g_net = NULL;
    g_store = NULL;
    g_panel = g_channel = 0;
    g_api = NULL;
    g_self = NULL;
}

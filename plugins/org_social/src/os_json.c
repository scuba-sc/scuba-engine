/* SPDX-License-Identifier: GPL-3.0-only */
#include "os_json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void os_json_init(os_json* j, char* buf, size_t cap) {
    j->p = buf;
    j->cap = buf ? cap : 0;
    j->len = 0;
}

static void put(os_json* j, const char* s, size_t n) {
    if (j->len + n <= j->cap) memcpy(j->p + j->len, s, n);
    j->len += n;
}

static void raw(os_json* j, const char* s) { put(j, s, strlen(s)); }

static void str(os_json* j, const char* s) {
    static const char hex[] = "0123456789abcdef";
    raw(j, "\"");
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char esc[2];
            esc[0] = '\\';
            esc[1] = (char)c;
            put(j, esc, 2);
        } else if (c < 0x20u) {
            char esc[6];
            esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 0xfu];
            put(j, esc, 6);
        } else {
            put(j, s, 1);
        }
    }
    raw(j, "\"");
}

static void key(os_json* j, const char* k) {
    str(j, k);
    raw(j, ":");
}

static void u64(os_json* j, uint64_t v) {
    char b[32];
    const int n = snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    if (n > 0) put(j, b, (size_t)n);
}

static void num(os_json* j, double v) {
    char b[40];
    int n;
    if (!isfinite(v)) { raw(j, "null"); return; }
    n = snprintf(b, sizeof b, "%.17g", v);
    if (n > 0) put(j, b, (size_t)n);
}

static void boolean(os_json* j, int v) { raw(j, v ? "true" : "false"); }

static void vec3(os_json* j, const double v[3]) {
    raw(j, "[");
    num(j, v[0]); raw(j, ",");
    num(j, v[1]); raw(j, ",");
    num(j, v[2]);
    raw(j, "]");
}

static void pose(os_json* j, const os_pose* p) {
    if (!p->valid) { raw(j, "null"); return; }
    raw(j, "{");
    key(j, "zone"); u64(j, p->zone); raw(j, ",");
    key(j, "pos"); vec3(j, p->pos);
    raw(j, "}");
}

static const char* member_state(const os_member* mem) {
    if (!mem->peer_id) return "offline";
    return os_status_name(mem->status);
}

void os_json_roster(os_json* j, const os_model* m, int session_active) {
    uint32_t i;
    raw(j, "{");
    key(j, "schema"); str(j, "org_social.roster/1"); raw(j, ",");
    key(j, "session"); raw(j, "{");
    key(j, "active"); boolean(j, session_active); raw(j, ",");
    key(j, "self_peer"); u64(j, m->self_peer); raw(j, ",");
    key(j, "status"); str(j, os_status_name(m->self_status)); raw(j, ",");
    key(j, "pose"); pose(j, &m->self_pose);
    raw(j, "},");
    key(j, "share_waypoints"); boolean(j, m->share_waypoints); raw(j, ",");
    key(j, "counts"); raw(j, "{");
    key(j, "org"); u64(j, os_count_org(m)); raw(j, ",");
    key(j, "online"); u64(j, os_count_online(m)); raw(j, ",");
    key(j, "party"); u64(j, os_count_party(m));
    raw(j, "},");
    key(j, "members"); raw(j, "[");
    for (i = 0; i < m->nmembers; ++i) {
        const os_member* mem = &m->members[i];
        if (i) raw(j, ",");
        raw(j, "{");
        key(j, "handle"); str(j, mem->handle); raw(j, ",");
        key(j, "rank"); str(j, os_rank_name(mem->rank)); raw(j, ",");
        key(j, "in_org"); boolean(j, mem->in_org); raw(j, ",");
        key(j, "online"); boolean(j, mem->peer_id != 0); raw(j, ",");
        key(j, "peer_id"); u64(j, mem->peer_id); raw(j, ",");
        key(j, "state"); str(j, member_state(mem)); raw(j, ",");
        key(j, "declared_rank");
        if (mem->has_declared_rank) str(j, os_rank_name(mem->declared_rank)); else raw(j, "null");
        raw(j, ",");
        key(j, "pose"); pose(j, &mem->pose);
        raw(j, "}");
    }
    raw(j, "],");
    key(j, "waypoints"); raw(j, "[");
    for (i = 0; i < m->nwaypoints; ++i) {
        const os_waypoint* wp = &m->waypoints[i];
        if (i) raw(j, ",");
        raw(j, "{");
        key(j, "owner_peer"); u64(j, wp->owner_peer); raw(j, ",");
        key(j, "id"); u64(j, wp->id); raw(j, ",");
        key(j, "name"); str(j, wp->name); raw(j, ",");
        key(j, "zone"); u64(j, wp->zone); raw(j, ",");
        key(j, "pos"); vec3(j, wp->pos);
        raw(j, "}");
    }
    raw(j, "]}");
}

static int skip_spaces(const char* s, size_t len, size_t* i) {
    while (*i < len && s[*i] == ' ') ++*i;
    return *i < len;
}

static int parse_token(const char* s, size_t len, size_t* i, char* tok, size_t cap) {
    size_t n = 0;
    if (!skip_spaces(s, len, i)) return 0;
    while (*i < len && s[*i] != ' ') {
        if ((unsigned char)s[*i] < 0x21u || (unsigned char)s[*i] > 0x7eu) return -1;
        if (n + 1 >= cap) return -1;
        tok[n++] = s[(*i)++];
    }
    tok[n] = 0;
    return 1;
}

int os_view_parse(const char* text, size_t len, os_view* view) {
    char tok[4][40];
    int got = 0, r;
    size_t i = 0;
    char* end = NULL;
    os_view v;
    memset(&v, 0, sizeof v);
    if (len > 256) return 0;
    while (got < 4 && (r = parse_token(text, len, &i, tok[got], sizeof tok[got])) != 0) {
        if (r < 0) return 0;
        ++got;
    }
    if (skip_spaces(text, len, &i)) return 0;
    if (got != 0 && got != 1 && got != 4) return 0;
    if (got >= 1) {
        if (tok[0][0] < '0' || tok[0][0] > '9') return 0;
        errno = 0;
        v.frame = (uint64_t)strtoull(tok[0], &end, 10);
        if (!end || *end || errno == ERANGE) return 0;
    }
    if (got == 4) {
        int k;
        for (k = 0; k < 3; ++k) {
            v.camera[k] = strtod(tok[k + 1], &end);
            if (!end || *end || end == tok[k + 1]) return 0;
        }
        if (!os_finite3(v.camera)) return 0;
        v.has_camera = 1;
    }
    view->frame = v.frame;
    view->has_camera = v.has_camera;
    memcpy(view->camera, v.camera, sizeof v.camera);
    return 1;
}

static void billboard(os_json* j, int* first, const char* kind, const char* id, const char* label,
                      const char* color, uint64_t zone, const double pos[3], const os_view* view) {
    double at[3];
    const char* method = "unresolved";
    int resolved = 0;
    if (zone == view->frame) {
        memcpy(at, pos, sizeof at);
        resolved = 1;
        method = "same_frame";
    } else if (view->to_frame && view->to_frame(view->ctx, zone, view->frame, pos, at) && os_finite3(at)) {
        resolved = 1;
        method = "pose_to_zone";
    }
    if (!*first) raw(j, ",");
    *first = 0;
    raw(j, "{");
    key(j, "kind"); str(j, kind); raw(j, ",");
    key(j, "id"); str(j, id); raw(j, ",");
    key(j, "label"); str(j, label); raw(j, ",");
    key(j, "color"); str(j, color); raw(j, ",");
    key(j, "zone"); u64(j, zone); raw(j, ",");
    key(j, "resolved"); boolean(j, resolved); raw(j, ",");
    key(j, "method"); str(j, method); raw(j, ",");
    key(j, "pos");
    if (resolved) vec3(j, at); else raw(j, "null");
    raw(j, ",");
    key(j, "distance_m");
    if (resolved && view->has_camera) {
        double d[3], u[3], dist;
        int k;
        for (k = 0; k < 3; ++k) d[k] = at[k] - view->camera[k];
        dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        num(j, dist);
        raw(j, ",");
        key(j, "to_camera");
        if (dist > 1e-9) {
            for (k = 0; k < 3; ++k) u[k] = -d[k] / dist;
            vec3(j, u);
        } else {
            raw(j, "null");
        }
    } else {
        raw(j, "null,");
        key(j, "to_camera"); raw(j, "null");
    }
    raw(j, "}");
}

void os_json_billboards(os_json* j, const os_model* m, const os_view* view, int spatial_available) {
    uint32_t i;
    int first = 1;
    char id[48], label[96];
    raw(j, "{");
    key(j, "schema"); str(j, "org_social.billboards/1"); raw(j, ",");
    key(j, "frame"); u64(j, view->frame); raw(j, ",");
    key(j, "spatial"); str(j, spatial_available ? "game.spatial" : "unavailable"); raw(j, ",");
    key(j, "camera");
    if (view->has_camera) vec3(j, view->camera); else raw(j, "null");
    raw(j, ",");
    key(j, "items"); raw(j, "[");
    for (i = 0; i < m->nwaypoints; ++i) {
        const os_waypoint* wp = &m->waypoints[i];
        snprintf(id, sizeof id, "wp:%llu:%u", (unsigned long long)wp->owner_peer, (unsigned)wp->id);
        billboard(j, &first, "waypoint", id, wp->name, wp->owner_peer ? "#ffb74d" : "#4fc3f7",
                  wp->zone, wp->pos, view);
    }
    if (m->self_pose.valid) {
        billboard(j, &first, "self", "self", "You", "#81c784", m->self_pose.zone, m->self_pose.pos, view);
    }
    for (i = 0; i < m->nmembers; ++i) {
        const os_member* mem = &m->members[i];
        if (!mem->pose.valid || !mem->peer_id) continue;
        snprintf(id, sizeof id, "peer:%llu", (unsigned long long)mem->peer_id);
        snprintf(label, sizeof label, "%s [%s]", mem->handle, os_rank_name(mem->rank));
        billboard(j, &first, "member", id, label, mem->in_org ? "#ba68c8" : "#90a4ae",
                  mem->pose.zone, mem->pose.pos, view);
    }
    raw(j, "]}");
}

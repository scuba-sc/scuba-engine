/* SPDX-License-Identifier: GPL-3.0-only */
#include "os_model.h"

#include <math.h>
#include <string.h>

static const char* const kRankNames[] = { "recruit", "member", "officer", "director", "founder", "guest" };
static const char* const kStatusNames[] = { "available", "busy", "away" };

void os_model_init(os_model* m) {
    memset(m, 0, sizeof *m);
    m->next_wp_id = 1;
    m->share_waypoints = 1;
}

const char* os_rank_name(uint32_t rank) {
    return rank <= OS_RANK_GUEST ? kRankNames[rank] : "unknown";
}

int os_rank_parse(const char* name, uint32_t* out) {
    uint32_t i;
    if (!name) return 0;
    for (i = 0; i < OS_ORG_RANKS; ++i) {
        if (strcmp(name, kRankNames[i]) == 0) { *out = i; return 1; }
    }
    return 0;
}

const char* os_status_name(uint32_t status) {
    return status < OS_STATUS_COUNT ? kStatusNames[status] : "unknown";
}

int os_status_parse(const char* name, uint32_t* out) {
    uint32_t i;
    if (!name) return 0;
    for (i = 0; i < OS_STATUS_COUNT; ++i) {
        if (strcmp(name, kStatusNames[i]) == 0) { *out = i; return 1; }
    }
    return 0;
}

int os_text_valid(const char* s, size_t len, size_t max) {
    size_t i;
    if (!s || len == 0 || len > max) return 0;
    if (s[0] == ' ' || s[len - 1] == ' ') return 0;
    for (i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20u || c == 0x7fu) return 0;
    }
    return 1;
}

int os_handle_valid(const char* s) {
    return s && os_text_valid(s, strlen(s), OS_MAX_HANDLE);
}

int os_finite3(const double v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static void copy_text(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

os_member* os_find_member(os_model* m, const char* handle) {
    uint32_t i;
    for (i = 0; i < m->nmembers; ++i) {
        if (strcmp(m->members[i].handle, handle) == 0) return &m->members[i];
    }
    return NULL;
}

os_member* os_find_peer(os_model* m, uint64_t peer_id) {
    uint32_t i;
    if (peer_id == 0) return NULL;
    for (i = 0; i < m->nmembers; ++i) {
        if (m->members[i].peer_id == peer_id) return &m->members[i];
    }
    return NULL;
}

static void drop_member_at(os_model* m, uint32_t i) {
    memmove(&m->members[i], &m->members[i + 1], (size_t)(m->nmembers - i - 1) * sizeof m->members[0]);
    --m->nmembers;
}

int os_member_set(os_model* m, const char* handle, uint32_t rank) {
    os_member* mem;
    if (!os_handle_valid(handle) || rank >= OS_ORG_RANKS) return -1;
    mem = os_find_member(m, handle);
    if (mem) {
        mem->rank = rank;
        mem->in_org = 1;
        return 0;
    }
    if (m->nmembers >= OS_MAX_MEMBERS) return -2;
    mem = &m->members[m->nmembers++];
    memset(mem, 0, sizeof *mem);
    copy_text(mem->handle, sizeof mem->handle, handle);
    mem->rank = rank;
    mem->in_org = 1;
    return 1;
}

int os_member_remove(os_model* m, const char* handle) {
    uint32_t i;
    for (i = 0; i < m->nmembers; ++i) {
        os_member* mem = &m->members[i];
        if (!mem->in_org || strcmp(mem->handle, handle) != 0) continue;
        if (mem->peer_id != 0) {
            mem->in_org = 0;
            mem->rank = OS_RANK_GUEST;
        } else {
            drop_member_at(m, i);
        }
        return 1;
    }
    return 0;
}

static int peer_listed(const os_session_peer* peers, uint32_t count, uint64_t peer_id) {
    uint32_t i;
    for (i = 0; i < count; ++i) {
        if (peers[i].peer_id == peer_id) return 1;
    }
    return 0;
}

static int drop_peer_waypoints(os_model* m, uint64_t peer_id) {
    uint32_t i = 0;
    int changed = 0;
    while (i < m->nwaypoints) {
        if (m->waypoints[i].owner_peer == peer_id) {
            memmove(&m->waypoints[i], &m->waypoints[i + 1],
                    (size_t)(m->nwaypoints - i - 1) * sizeof m->waypoints[0]);
            --m->nwaypoints;
            changed = 1;
        } else {
            ++i;
        }
    }
    return changed;
}

int os_session_set(os_model* m, uint64_t self_peer, const os_session_peer* peers, uint32_t count) {
    uint32_t i;
    int changed = m->self_peer != self_peer;
    m->self_peer = self_peer;

    i = 0;
    while (i < m->nmembers) {
        os_member* mem = &m->members[i];
        if (mem->peer_id != 0 && !peer_listed(peers, count, mem->peer_id)) {
            changed |= drop_peer_waypoints(m, mem->peer_id);
            if (!mem->in_org) {
                drop_member_at(m, i);
                changed = 1;
                continue;
            }
            mem->peer_id = 0;
            mem->status = OS_STATUS_AVAILABLE;
            mem->has_declared_rank = 0;
            mem->pose.valid = 0;
            changed = 1;
        }
        ++i;
    }

    for (i = 0; i < count; ++i) {
        const os_session_peer* p = &peers[i];
        os_member* mem;
        if (p->peer_id == 0 || p->peer_id == self_peer || !os_handle_valid(p->handle)) continue;
        mem = os_find_peer(m, p->peer_id);
        if (mem && strcmp(mem->handle, p->handle) == 0) continue;
        if (mem) {
            mem->peer_id = 0;
            if (!mem->in_org) mem->handle[0] = 0;
        }
        mem = os_find_member(m, p->handle);
        if (!mem) {
            if (m->nmembers >= OS_MAX_MEMBERS) continue;
            mem = &m->members[m->nmembers++];
            memset(mem, 0, sizeof *mem);
            copy_text(mem->handle, sizeof mem->handle, p->handle);
            mem->rank = OS_RANK_GUEST;
        }
        mem->peer_id = p->peer_id;
        changed = 1;
    }

    i = 0;
    while (i < m->nmembers) {
        if (m->members[i].handle[0] == 0) { drop_member_at(m, i); continue; }
        ++i;
    }
    return changed;
}

os_waypoint* os_find_waypoint(os_model* m, uint64_t owner, uint32_t id) {
    uint32_t i;
    for (i = 0; i < m->nwaypoints; ++i) {
        if (m->waypoints[i].owner_peer == owner && m->waypoints[i].id == id) return &m->waypoints[i];
    }
    return NULL;
}

int os_waypoint_put(os_model* m, uint64_t owner, uint32_t id, const char* name, uint64_t zone,
                    const double pos[3]) {
    os_waypoint* wp;
    if (id == 0 || !name || !os_text_valid(name, strlen(name), OS_MAX_WP_NAME) || !os_finite3(pos)) return -1;
    wp = os_find_waypoint(m, owner, id);
    if (!wp) {
        if (m->nwaypoints >= OS_MAX_WAYPOINTS) return -2;
        wp = &m->waypoints[m->nwaypoints++];
        memset(wp, 0, sizeof *wp);
        wp->owner_peer = owner;
        wp->id = id;
    }
    copy_text(wp->name, sizeof wp->name, name);
    wp->zone = zone;
    memcpy(wp->pos, pos, sizeof wp->pos);
    if (owner == 0 && id >= m->next_wp_id) m->next_wp_id = id + 1;
    return 1;
}

int os_waypoint_remove(os_model* m, uint64_t owner, uint32_t id) {
    os_waypoint* wp = os_find_waypoint(m, owner, id);
    size_t i;
    if (!wp) return 0;
    i = (size_t)(wp - m->waypoints);
    memmove(wp, wp + 1, (m->nwaypoints - i - 1) * sizeof *wp);
    --m->nwaypoints;
    return 1;
}

uint32_t os_count_org(const os_model* m) {
    uint32_t i, n = 0;
    for (i = 0; i < m->nmembers; ++i) n += m->members[i].in_org ? 1u : 0u;
    return n;
}

uint32_t os_count_online(const os_model* m) {
    uint32_t i, n = 0;
    for (i = 0; i < m->nmembers; ++i) n += (m->members[i].in_org && m->members[i].peer_id) ? 1u : 0u;
    return n;
}

uint32_t os_count_party(const os_model* m) {
    uint32_t i, n = 0;
    for (i = 0; i < m->nmembers; ++i) n += m->members[i].peer_id ? 1u : 0u;
    return n;
}

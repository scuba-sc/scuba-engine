/* SPDX-License-Identifier: GPL-3.0-only
 * org_social: the roster and waypoint model. Plain C11, no host calls, so the unit tests
 * link it without a host. The plugin (org_social.c) owns one os_model and feeds it from
 * commands, sco.storage and sco.net.
 */
#ifndef ORG_SOCIAL_MODEL_H
#define ORG_SOCIAL_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define OS_MAX_HANDLE    63u  /* bytes, NUL excluded (sco.net player names are at most 64) */
#define OS_MAX_WP_NAME   47u
#define OS_MAX_MEMBERS   64u
#define OS_MAX_WAYPOINTS 64u
#define OS_MAX_PEERS     16u  /* SCO_NET_MAX_PEERS */

/* Org ranks, lowest first. GUEST marks a session peer that is not on the org roster. */
#define OS_RANK_RECRUIT  0u
#define OS_RANK_MEMBER   1u
#define OS_RANK_OFFICER  2u
#define OS_RANK_DIRECTOR 3u
#define OS_RANK_FOUNDER  4u
#define OS_RANK_GUEST    5u
#define OS_ORG_RANKS     5u   /* ranks a roster entry may hold: RECRUIT..FOUNDER */

/* Declared session status (HELLO). */
#define OS_STATUS_AVAILABLE 0u
#define OS_STATUS_BUSY      1u
#define OS_STATUS_AWAY      2u
#define OS_STATUS_COUNT     3u

typedef struct os_pose {
    int      valid;
    uint64_t zone;     /* game.spatial zone id the position is in; 0 = world */
    double   pos[3];   /* metres in that zone's frame */
} os_pose;

typedef struct os_member {
    char     handle[OS_MAX_HANDLE + 1];
    uint32_t rank;          /* OS_RANK_*; GUEST when in_org is 0 */
    int      in_org;        /* on the persisted org roster */
    uint64_t peer_id;       /* sco.net peer id in the current session, 0 = not in session */
    uint32_t status;        /* OS_STATUS_* the peer declared; AVAILABLE until it says otherwise */
    int      has_declared_rank;
    uint32_t declared_rank; /* rank the peer claims for itself (informational, never trusted) */
    os_pose  pose;          /* last shared pose (POSE message) */
} os_member;

typedef struct os_waypoint {
    uint64_t owner_peer;    /* 0 = this player's own waypoint, else the sharing peer */
    uint32_t id;            /* unique per owner */
    char     name[OS_MAX_WP_NAME + 1];
    uint64_t zone;
    double   pos[3];
} os_waypoint;

typedef struct os_session_peer {
    uint64_t peer_id;
    char     handle[OS_MAX_HANDLE + 1];
} os_session_peer;

typedef struct os_model {
    os_member   members[OS_MAX_MEMBERS];
    uint32_t    nmembers;
    os_waypoint waypoints[OS_MAX_WAYPOINTS];
    uint32_t    nwaypoints;
    uint32_t    next_wp_id;   /* next id for an own waypoint (starts at 1) */
    uint64_t    self_peer;    /* 0 = no session */
    uint32_t    self_status;
    os_pose     self_pose;
    int         share_waypoints;
} os_model;

void        os_model_init(os_model* m);
const char* os_rank_name(uint32_t rank);
int         os_rank_parse(const char* name, uint32_t* out);  /* org ranks only; 1 = ok */
const char* os_status_name(uint32_t status);
int         os_status_parse(const char* name, uint32_t* out);
/* 1..max bytes, no control bytes (< 0x20, 0x7f), no leading or trailing space. */
int         os_text_valid(const char* s, size_t len, size_t max);
int         os_handle_valid(const char* s);
int         os_finite3(const double v[3]);

os_member*  os_find_member(os_model* m, const char* handle);
os_member*  os_find_peer(os_model* m, uint64_t peer_id);
/* Adds the handle to the org roster or changes its rank (a guest joins the org). 1 = added,
 * 0 = updated, -1 = bad handle or rank, -2 = roster full. */
int         os_member_set(os_model* m, const char* handle, uint32_t rank);
/* Takes the handle off the org roster; a member still in the session stays as a guest.
 * 1 = removed, 0 = no such org member. */
int         os_member_remove(os_model* m, const char* handle);
/* Replaces the session view: peers (self excluded) with their names. Org members are matched
 * by handle; unknown names become guests; guests no longer present are dropped, as are the
 * waypoints and poses of peers that left. Returns 1 when anything visible changed. */
int         os_session_set(os_model* m, uint64_t self_peer, const os_session_peer* peers, uint32_t count);

/* Inserts or replaces (owner, id). 1 = ok, -1 = bad name/zone/position, -2 = full. */
int         os_waypoint_put(os_model* m, uint64_t owner, uint32_t id, const char* name, uint64_t zone,
                            const double pos[3]);
int         os_waypoint_remove(os_model* m, uint64_t owner, uint32_t id);   /* 1 = removed */
os_waypoint* os_find_waypoint(os_model* m, uint64_t owner, uint32_t id);

uint32_t    os_count_org(const os_model* m);
uint32_t    os_count_online(const os_model* m);   /* org members in the session */
uint32_t    os_count_party(const os_model* m);    /* every peer in the session, self excluded */

#endif

/* SPDX-License-Identifier: GPL-3.0-only
 * org_social JSON documents (schemas in README.md): org_social.roster/1 and
 * org_social.billboards/1. UTF-8 bytes, no NUL. The writer keeps counting past cap so the
 * caller learns the size needed (the sco_api raw-handler handshake).
 */
#ifndef ORG_SOCIAL_JSON_H
#define ORG_SOCIAL_JSON_H

#include "os_model.h"

#include <stddef.h>
#include <stdint.h>

typedef struct os_json {
    char*  p;
    size_t cap;
    size_t len;   /* bytes the document needs; > cap means it did not fit */
} os_json;

/* Converts a position from zone `from` to zone `to`; 1 on success, 0 when it cannot. */
typedef int (*os_frame_fn)(void* ctx, uint64_t from, uint64_t to, const double in[3], double out[3]);

typedef struct os_view {
    uint64_t    frame;       /* zone the billboard positions are expressed in; 0 = world */
    int         has_camera;
    double      camera[3];   /* camera position in `frame` */
    os_frame_fn to_frame;    /* NULL: only positions already in `frame` resolve */
    void*       ctx;
} os_view;

void os_json_init(os_json* j, char* buf, size_t cap);
/* Parses the billboards request text "<frame> [<x> <y> <z>]" (ASCII, decimal, spaces).
 * Empty input means frame 0 with no camera. 1 = ok. */
int  os_view_parse(const char* text, size_t len, os_view* view);

void os_json_roster(os_json* j, const os_model* m, int session_active);
void os_json_billboards(os_json* j, const os_model* m, const os_view* view, int spatial_available);

#endif

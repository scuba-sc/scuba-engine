/* SPDX-License-Identifier: GPL-3.0-only */
#include "os_wire.h"

#include "os_model.h"

#include <string.h>

typedef struct writer { uint8_t* p; size_t cap, len; int bad; } writer;
typedef struct reader { const uint8_t* p; size_t len, pos; int bad; } reader;

static void put_u8(writer* w, uint32_t v) {
    if (w->len + 1 > w->cap) { w->bad = 1; return; }
    w->p[w->len++] = (uint8_t)(v & 0xffu);
}

static void put_u16(writer* w, uint32_t v) { put_u8(w, v); put_u8(w, v >> 8); }
static void put_u32(writer* w, uint32_t v) { put_u16(w, v & 0xffffu); put_u16(w, v >> 16); }
static void put_u64(writer* w, uint64_t v) {
    put_u32(w, (uint32_t)(v & 0xffffffffu));
    put_u32(w, (uint32_t)(v >> 32));
}
static void put_f64(writer* w, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    put_u64(w, bits);
}
static void put_bytes(writer* w, const char* s, size_t n) {
    if (w->len + n > w->cap) { w->bad = 1; return; }
    memcpy(w->p + w->len, s, n);
    w->len += n;
}

static uint32_t get_u8(reader* r) {
    if (r->pos + 1 > r->len) { r->bad = 1; return 0; }
    return r->p[r->pos++];
}
static uint32_t get_u16(reader* r) { uint32_t lo = get_u8(r); return lo | (get_u8(r) << 8); }
static uint32_t get_u32(reader* r) { uint32_t lo = get_u16(r); return lo | (get_u16(r) << 16); }
static uint64_t get_u64(reader* r) { uint64_t lo = get_u32(r); return lo | ((uint64_t)get_u32(r) << 32); }
static double get_f64(reader* r) {
    uint64_t bits = get_u64(r);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
static void get_text(reader* r, char* out, size_t n) {
    if (r->pos + n > r->len) { r->bad = 1; out[0] = 0; return; }
    if (!os_text_valid((const char*)r->p + r->pos, n, sizeof(((os_msg*)0)->text) - 1)) {
        r->bad = 1; out[0] = 0; return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = 0;
    r->pos += n;
}

static int text_ok(const char* s, size_t max) {
    return os_text_valid(s, strlen(s), max);
}

size_t os_wire_encode(const os_msg* msg, uint8_t* out, size_t cap) {
    writer w;
    size_t n, body;
    w.p = out; w.cap = cap; w.len = 0; w.bad = 0;
    put_u8(&w, OS_WIRE_VERSION);
    put_u8(&w, msg->type);
    put_u16(&w, 0);
    switch (msg->type) {
    case OS_MSG_HELLO:
        if (!text_ok(msg->text, OS_MAX_HANDLE) || msg->status >= OS_STATUS_COUNT || msg->rank > OS_RANK_GUEST) return 0;
        n = strlen(msg->text);
        put_u8(&w, msg->status);
        put_u8(&w, msg->rank);
        put_u8(&w, (uint32_t)n);
        put_bytes(&w, msg->text, n);
        break;
    case OS_MSG_WAYPOINT_SET:
        if (msg->id == 0 || !text_ok(msg->text, OS_MAX_WP_NAME) || !os_finite3(msg->pos)) return 0;
        n = strlen(msg->text);
        put_u32(&w, msg->id);
        put_u64(&w, msg->zone);
        put_f64(&w, msg->pos[0]); put_f64(&w, msg->pos[1]); put_f64(&w, msg->pos[2]);
        put_u8(&w, (uint32_t)n);
        put_bytes(&w, msg->text, n);
        break;
    case OS_MSG_WAYPOINT_DEL:
        if (msg->id == 0) return 0;
        put_u32(&w, msg->id);
        break;
    case OS_MSG_POSE:
        if (!os_finite3(msg->pos)) return 0;
        put_u64(&w, msg->zone);
        put_f64(&w, msg->pos[0]); put_f64(&w, msg->pos[1]); put_f64(&w, msg->pos[2]);
        break;
    default:
        return 0;
    }
    if (w.bad) return 0;
    body = w.len - OS_WIRE_HEADER;
    out[2] = (uint8_t)(body & 0xffu);
    out[3] = (uint8_t)((body >> 8) & 0xffu);
    return w.len;
}

int os_wire_decode(const uint8_t* buf, size_t len, os_msg* out) {
    reader r;
    os_msg m;
    size_t n;
    if (!buf || len < OS_WIRE_HEADER) return 0;
    r.p = buf; r.len = len; r.pos = 0; r.bad = 0;
    memset(&m, 0, sizeof m);
    if (get_u8(&r) != OS_WIRE_VERSION) return 0;
    m.type = get_u8(&r);
    if ((size_t)get_u16(&r) != len - OS_WIRE_HEADER) return 0;
    switch (m.type) {
    case OS_MSG_HELLO:
        m.status = get_u8(&r);
        m.rank = get_u8(&r);
        n = get_u8(&r);
        if (r.bad || n > OS_MAX_HANDLE) return 0;
        get_text(&r, m.text, n);
        if (r.bad || m.status >= OS_STATUS_COUNT || m.rank > OS_RANK_GUEST || !text_ok(m.text, OS_MAX_HANDLE)) return 0;
        break;
    case OS_MSG_WAYPOINT_SET:
        m.id = get_u32(&r);
        m.zone = get_u64(&r);
        m.pos[0] = get_f64(&r); m.pos[1] = get_f64(&r); m.pos[2] = get_f64(&r);
        n = get_u8(&r);
        if (r.bad || n > OS_MAX_WP_NAME) return 0;
        get_text(&r, m.text, n);
        if (r.bad || m.id == 0 || !os_finite3(m.pos) || !text_ok(m.text, OS_MAX_WP_NAME)) return 0;
        break;
    case OS_MSG_WAYPOINT_DEL:
        m.id = get_u32(&r);
        if (r.bad || m.id == 0) return 0;
        break;
    case OS_MSG_POSE:
        m.zone = get_u64(&r);
        m.pos[0] = get_f64(&r); m.pos[1] = get_f64(&r); m.pos[2] = get_f64(&r);
        if (r.bad || !os_finite3(m.pos)) return 0;
        break;
    default:
        return 0;
    }
    if (r.pos != len) return 0;
    *out = m;
    return 1;
}

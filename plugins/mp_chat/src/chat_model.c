/* SPDX-License-Identifier: GPL-3.0-only */
#include "chat_model.h"

#include <string.h>

/* Length of the well-formed UTF-8 sequence at s (n bytes available), 0 if malformed: overlongs,
 * surrogates and code points above U+10FFFF are malformed (RFC 3629). */
static size_t utf8_seq(const unsigned char* s, size_t n) {
    const unsigned c = s[0];
    if (c < 0x80u) return 1;
    if (c >= 0xC2u && c <= 0xDFu) return (n >= 2 && (s[1] & 0xC0u) == 0x80u) ? 2u : 0u;
    if (c >= 0xE0u && c <= 0xEFu) {
        if (n < 3 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) return 0;
        if (c == 0xE0u && s[1] < 0xA0u) return 0;
        if (c == 0xEDu && s[1] >= 0xA0u) return 0;
        return 3;
    }
    if (c >= 0xF0u && c <= 0xF4u) {
        if (n < 4 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u || (s[3] & 0xC0u) != 0x80u) return 0;
        if (c == 0xF0u && s[1] < 0x90u) return 0;
        if (c == 0xF4u && s[1] >= 0x90u) return 0;
        return 4;
    }
    return 0;
}

/* C0 controls, DEL, and C1 controls (U+0080..U+009F, encoded C2 80..C2 9F). */
static int is_control(const unsigned char* s, size_t seq) {
    if (seq == 1) return s[0] < 0x20u || s[0] == 0x7Fu;
    return seq == 2 && s[0] == 0xC2u && s[1] < 0xA0u;
}

int mpc_valid_room(const char* name) {
    size_t i, n;
    if (!name) return 0;
    n = strlen(name);
    if (n == 0 || n > MPC_MAX_ROOM) return 0;
    for (i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int mpc_valid_text(const char* s, size_t len) {
    const unsigned char* p = (const unsigned char*)s;
    size_t i = 0;
    if (!s || len == 0 || len > MPC_MAX_TEXT) return 0;
    while (i < len) {
        const size_t seq = utf8_seq(p + i, len - i);
        if (!seq || is_control(p + i, seq)) return 0;
        i += seq;
    }
    return 1;
}

void mpc_append_clean(char* dst, size_t cap, size_t* len, const char* src) {
    const unsigned char* p = (const unsigned char*)src;
    size_t n, i = 0;
    if (!dst || cap == 0 || !len || *len >= cap) return;
    if (!src) { dst[*len] = 0; return; }
    n = strlen(src);
    while (i < n) {
        size_t seq = utf8_seq(p + i, n - i);
        if (seq && !is_control(p + i, seq)) {
            if (*len + seq >= cap) break;
            memcpy(dst + *len, p + i, seq);
            *len += seq;
            i += seq;
        } else {
            if (*len + 1 >= cap) break;
            dst[(*len)++] = '?';
            i += seq ? seq : 1u;
        }
    }
    dst[*len] = 0;
}

size_t mpc_encode(const char* room, const char* text, uint8_t* out, size_t cap) {
    size_t room_len, text_len, total;
    if (!mpc_valid_room(room) || !text || !out) return 0;
    room_len = strlen(room);
    text_len = strlen(text);
    if (!mpc_valid_text(text, text_len)) return 0;
    total = MPC_WIRE_HEADER + room_len + text_len;
    if (total > cap) return 0;
    out[0] = 'M';
    out[1] = 'C';
    out[2] = (uint8_t)MPC_WIRE_VERSION;
    out[3] = (uint8_t)room_len;
    out[4] = (uint8_t)(text_len & 0xFFu);
    out[5] = (uint8_t)(text_len >> 8);
    memcpy(out + MPC_WIRE_HEADER, room, room_len);
    memcpy(out + MPC_WIRE_HEADER + room_len, text, text_len);
    return total;
}

int mpc_decode(const uint8_t* buf, size_t len, char room[MPC_MAX_ROOM + 1u], char text[MPC_MAX_TEXT + 1u]) {
    size_t room_len, text_len;
    if (!buf || !room || !text || len < MPC_WIRE_HEADER || len > MPC_WIRE_MAX) return 0;
    if (buf[0] != 'M' || buf[1] != 'C' || buf[2] != MPC_WIRE_VERSION) return 0;
    room_len = buf[3];
    text_len = (size_t)buf[4] | ((size_t)buf[5] << 8);
    if (room_len == 0 || room_len > MPC_MAX_ROOM || text_len == 0 || text_len > MPC_MAX_TEXT) return 0;
    if (len != MPC_WIRE_HEADER + room_len + text_len) return 0;
    memcpy(room, buf + MPC_WIRE_HEADER, room_len);
    room[room_len] = 0;
    if (!mpc_valid_room(room)) return 0;
    if (!mpc_valid_text((const char*)buf + MPC_WIRE_HEADER + room_len, text_len)) return 0;
    memcpy(text, buf + MPC_WIRE_HEADER + room_len, text_len);
    text[text_len] = 0;
    return 1;
}

void mpc_init(mpc_state* st, uint32_t visible) {
    memset(st, 0, sizeof *st);
    st->visible = visible;
    (void)mpc_room_index(st, MPC_DEFAULT_ROOM, 1);
    st->current = 0;
}

int mpc_room_index(mpc_state* st, const char* name, int create) {
    uint32_t i;
    if (!mpc_valid_room(name)) return -1;
    for (i = 0; i < st->nrooms; ++i)
        if (!strcmp(st->rooms[i].name, name)) return (int)i;
    if (!create || st->nrooms == MPC_MAX_ROOMS) return -1;
    i = st->nrooms++;
    memset(&st->rooms[i], 0, sizeof st->rooms[i]);
    memcpy(st->rooms[i].name, name, strlen(name) + 1u);
    st->rooms[i].follow = 1;
    return (int)i;
}

mpc_room* mpc_current(mpc_state* st) { return &st->rooms[st->current]; }

const mpc_entry* mpc_at(const mpc_room* room, uint32_t i) {
    if (i >= room->count) return NULL;
    return &room->entries[(room->head + i) % MPC_HISTORY];
}

uint32_t mpc_tail_row(uint32_t count, uint32_t visible) {
    return (visible && count > visible) ? count - visible : 0u;
}

void mpc_add(mpc_state* st, mpc_room* room, uint32_t severity, const char* sender, const char* text) {
    static const char* const badges[] = { "", "[info] ", "[warn] ", "[error] " };
    mpc_entry* e;
    size_t len = 0;
    if (severity > MPC_SEV_ERROR) severity = MPC_SEV_ERROR;
    if (room->count == MPC_HISTORY) {
        room->head = (room->head + 1u) % MPC_HISTORY;
        --room->count;
        if (!room->follow && room->first_row > 0) --room->first_row;
    }
    e = &room->entries[(room->head + room->count) % MPC_HISTORY];
    ++room->count;
    e->severity = severity;
    e->line[0] = 0;
    if (severity == MPC_SEV_NONE) {
        mpc_append_clean(e->line, sizeof e->line, &len, sender && *sender ? sender : "?");
        mpc_append_clean(e->line, sizeof e->line, &len, ": ");
    } else {
        mpc_append_clean(e->line, sizeof e->line, &len, badges[severity]);
    }
    mpc_append_clean(e->line, sizeof e->line, &len, text);
    if (room->follow) room->first_row = mpc_tail_row(room->count, st->visible);
}

const mpc_entry* mpc_last_chat(const mpc_room* room) {
    uint32_t i = room->count;
    while (i > 0) {
        const mpc_entry* e = mpc_at(room, --i);
        if (e->severity == MPC_SEV_NONE) return e;
    }
    return NULL;
}

void mpc_clear(mpc_state* st, mpc_room* room) {
    (void)st;
    room->head = 0;
    room->count = 0;
    room->first_row = 0;
    room->follow = 1;
    room->unread = 0;
}

int mpc_page(mpc_state* st, mpc_room* room, int32_t row) {
    uint32_t r;
    if (!st->visible || row < 0) return 0;
    r = (uint32_t)row;
    if (r != 0 && r >= room->count) return 0;
    room->first_row = r;
    room->follow = r >= mpc_tail_row(room->count, st->visible);
    return 1;
}

uint32_t mpc_max_severity(const mpc_room* room, uint32_t first, uint32_t n) {
    uint32_t i, sev = MPC_SEV_NONE;
    for (i = first; i < room->count && i - first < n; ++i) {
        const mpc_entry* e = mpc_at(room, i);
        if (e->severity > sev) sev = e->severity;
    }
    return sev;
}

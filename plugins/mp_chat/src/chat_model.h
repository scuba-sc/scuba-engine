/* SPDX-License-Identifier: GPL-3.0-only
 * mp_chat model: room/channel bookkeeping, the chat wire format and text validation. Pure C11,
 * no host calls, so it is unit-tested on its own (tests/test_chat_model.c).
 */
#ifndef MP_CHAT_MODEL_H
#define MP_CHAT_MODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPC_MAX_ROOM 24u      /* room name bytes, NUL excluded: [a-z0-9_-] */
#define MPC_MAX_TEXT 200u     /* message bytes (UTF-8), NUL excluded */
#define MPC_MAX_NAME 64u      /* player name bytes, as sco.net (SCO_NET_MAX_NAME) */
#define MPC_LINE 256u         /* one history line with its NUL: fits a widget item (255 bytes) */
#define MPC_HISTORY 256u      /* lines kept per room: SCO_WIDGETS_MAX_ITEMS */
#define MPC_MAX_ROOMS 8u
#define MPC_DEFAULT_ROOM "general"

/* Wire: 'M' 'C' version(1) room_len(1) text_len(2, little-endian) room text. */
#define MPC_WIRE_VERSION 1u
#define MPC_WIRE_HEADER 6u
#define MPC_WIRE_MAX (MPC_WIRE_HEADER + MPC_MAX_ROOM + MPC_MAX_TEXT)

/* Same numbers as SCO_WIDGET_SEVERITY_*. */
#define MPC_SEV_NONE 0u
#define MPC_SEV_INFO 1u
#define MPC_SEV_WARNING 2u
#define MPC_SEV_ERROR 3u

typedef struct mpc_entry {
    uint32_t severity; /* MPC_SEV_NONE: a chat line; otherwise a local notice */
    char line[MPC_LINE];
} mpc_entry;

typedef struct mpc_room {
    char name[MPC_MAX_ROOM + 1u];
    mpc_entry entries[MPC_HISTORY]; /* ring: head is the oldest */
    uint32_t head, count;
    uint32_t first_row; /* first visible row when paged */
    int follow;         /* 1: the view sticks to the newest rows */
    uint32_t unread;    /* lines added while another room was current */
} mpc_room;

typedef struct mpc_state {
    mpc_room rooms[MPC_MAX_ROOMS];
    uint32_t nrooms, current;
    uint32_t visible; /* rows per page; 0 = no paging */
} mpc_state;

/* Empty state with the default room current. */
void mpc_init(mpc_state* st, uint32_t visible);

/* 1 if name is 1..MPC_MAX_ROOM bytes of [a-z0-9_-]. */
int mpc_valid_room(const char* name);
/* 1 if s[0..len) is 1..MPC_MAX_TEXT bytes of well-formed UTF-8 with no C0, DEL or C1 controls. */
int mpc_valid_text(const char* s, size_t len);
/* Appends src to dst (cap bytes including the NUL; *len bytes used) as clean UTF-8: invalid or
 * control sequences become '?', and copying stops before a sequence that does not fit. */
void mpc_append_clean(char* dst, size_t cap, size_t* len, const char* src);

/* Encodes one message; returns its byte count, or 0 for an invalid room/text or a short buffer. */
size_t mpc_encode(const char* room, const char* text, uint8_t* out, size_t cap);
/* Decodes and validates one message from an untrusted peer. 1 on success. */
int mpc_decode(const uint8_t* buf, size_t len, char room[MPC_MAX_ROOM + 1u], char text[MPC_MAX_TEXT + 1u]);

/* Index of the room called name; with create, adds it when missing. -1: invalid name, or full. */
int mpc_room_index(mpc_state* st, const char* name, int create);
mpc_room* mpc_current(mpc_state* st);
/* Line i of a room, 0 = oldest; NULL past the end. */
const mpc_entry* mpc_at(const mpc_room* room, uint32_t i);
/* Adds a line. A chat line reads "sender: text"; a notice "[info] text" / "[warn] ..." /
 * "[error] ...". The oldest line is dropped when the room is full. Keeps the page in view. */
void mpc_add(mpc_state* st, mpc_room* room, uint32_t severity, const char* sender, const char* text);
/* The newest chat line (severity NONE) of a room, or NULL. */
const mpc_entry* mpc_last_chat(const mpc_room* room);
void mpc_clear(mpc_state* st, mpc_room* room);

/* First row of the last page. */
uint32_t mpc_tail_row(uint32_t count, uint32_t visible);
/* Applies a page request (new first visible row). 0: row out of range, nothing changes. */
int mpc_page(mpc_state* st, mpc_room* room, int32_t row);
/* Highest severity among rows [first, first + n) of a room. */
uint32_t mpc_max_severity(const mpc_room* room, uint32_t first, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* MP_CHAT_MODEL_H */

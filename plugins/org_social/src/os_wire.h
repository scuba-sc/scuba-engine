/* SPDX-License-Identifier: GPL-3.0-only
 * org_social wire schema v1 (README.md "Message schema" is the normative description).
 * Every message: u8 version (1), u8 type, u16 body length (LE), then the body. Multi-byte
 * integers are little-endian; doubles are IEEE-754 binary64 sent as their LE bit pattern.
 */
#ifndef ORG_SOCIAL_WIRE_H
#define ORG_SOCIAL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define OS_WIRE_VERSION 1u
#define OS_WIRE_HEADER  4u

#define OS_MSG_HELLO        1u  /* reliable channel org_social.roster */
#define OS_MSG_WAYPOINT_SET 2u  /* reliable channel org_social.roster */
#define OS_MSG_WAYPOINT_DEL 3u  /* reliable channel org_social.roster */
#define OS_MSG_POSE         4u  /* unreliable channel org_social.pose */

#define OS_WIRE_MAX_ROSTER  128u  /* largest roster-channel message (WAYPOINT_SET with a 47-byte name) */
#define OS_WIRE_MAX_POSE    36u

typedef struct os_msg {
    uint32_t type;
    uint32_t status;        /* HELLO */
    uint32_t rank;          /* HELLO: declared rank, OS_RANK_* (GUEST = not in an org) */
    char     text[64];      /* HELLO: handle; WAYPOINT_SET: name */
    uint32_t id;            /* WAYPOINT_SET / WAYPOINT_DEL */
    uint64_t zone;          /* WAYPOINT_SET / POSE */
    double   pos[3];        /* WAYPOINT_SET / POSE */
} os_msg;

/* Bytes written, or 0 when the message is invalid or does not fit in cap. */
size_t os_wire_encode(const os_msg* msg, uint8_t* out, size_t cap);
/* 1 and *out filled when buf is one well-formed v1 message; 0 otherwise (out untouched). */
int    os_wire_decode(const uint8_t* buf, size_t len, os_msg* out);

#endif

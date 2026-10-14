/* SPDX-License-Identifier: GPL-3.0-only */
#include "chat_model.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

static mpc_state st;

static int valid(const char* s) { return mpc_valid_text(s, strlen(s)); }

static void test_validation(void) {
    char long_room[MPC_MAX_ROOM + 2u];
    char long_text[MPC_MAX_TEXT + 2u];
    CHECK(mpc_valid_room("general"));
    CHECK(mpc_valid_room("ops-team_2"));
    CHECK(!mpc_valid_room(""));
    CHECK(!mpc_valid_room("General"));
    CHECK(!mpc_valid_room("a.b"));
    CHECK(!mpc_valid_room(NULL));
    memset(long_room, 'a', sizeof long_room - 1u);
    long_room[sizeof long_room - 1u] = 0;
    CHECK(!mpc_valid_room(long_room));
    long_room[MPC_MAX_ROOM] = 0;
    CHECK(mpc_valid_room(long_room));

    CHECK(valid("hello"));
    CHECK(valid("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x9A\x80"));
    CHECK(!valid(""));
    CHECK(!valid("line\nbreak"));
    CHECK(!valid("del\x7F"));
    CHECK(!valid("c1 \xC2\x85"));
    CHECK(!valid("overlong \xC0\x80"));
    CHECK(!valid("overlong \xE0\x80\x80"));
    CHECK(!valid("surrogate \xED\xA0\x80"));
    CHECK(!valid("too big \xF4\x90\x80\x80"));
    CHECK(!valid("cut \xE2\x82"));
    memset(long_text, 'x', sizeof long_text - 1u);
    long_text[sizeof long_text - 1u] = 0;
    CHECK(!valid(long_text));
    long_text[MPC_MAX_TEXT] = 0;
    CHECK(valid(long_text));
}

static void test_clean(void) {
    char buf[8];
    size_t len = 0;
    buf[0] = 0;
    mpc_append_clean(buf, sizeof buf, &len, "ab\ncd");
    CHECK(!strcmp(buf, "ab?cd"));
    mpc_append_clean(buf, sizeof buf, &len, "\xE2\x82\xAC");
    CHECK(len == 5u && !strcmp(buf, "ab?cd"));
    mpc_append_clean(buf, sizeof buf, &len, "\xC3\xA9z");
    CHECK(len == 7u && !strcmp(buf, "ab?cd\xC3\xA9"));
    len = 0;
    mpc_append_clean(buf, sizeof buf, &len, "\xFF\xFEok");
    CHECK(!strcmp(buf, "??ok"));
}

static void test_wire(void) {
    uint8_t wire[MPC_WIRE_MAX];
    char room[MPC_MAX_ROOM + 1u], text[MPC_MAX_TEXT + 1u];
    size_t n = mpc_encode("lobby", "hi \xC3\xA9", wire, sizeof wire);
    CHECK(n == MPC_WIRE_HEADER + 5u + 5u);
    CHECK(mpc_decode(wire, n, room, text));
    CHECK(!strcmp(room, "lobby") && !strcmp(text, "hi \xC3\xA9"));
    CHECK(!mpc_decode(wire, n - 1u, room, text));
    CHECK(!mpc_decode(wire, n + 1u, room, text));
    CHECK(!mpc_decode(wire, 3, room, text));
    wire[2] = 2;
    CHECK(!mpc_decode(wire, n, room, text));
    wire[2] = MPC_WIRE_VERSION;
    wire[0] = 'X';
    CHECK(!mpc_decode(wire, n, room, text));
    wire[0] = 'M';
    wire[MPC_WIRE_HEADER] = 'L';
    CHECK(!mpc_decode(wire, n, room, text));
    wire[MPC_WIRE_HEADER] = 'l';
    wire[n - 1u] = 0x0A;
    CHECK(!mpc_decode(wire, n, room, text));
    CHECK(mpc_encode("Bad Room", "x", wire, sizeof wire) == 0u);
    CHECK(mpc_encode("ok", "tab\there", wire, sizeof wire) == 0u);
    CHECK(mpc_encode("ok", "x", wire, 6) == 0u);
    n = mpc_encode("ok", "x", wire, sizeof wire);
    wire[3] = 0;
    CHECK(!mpc_decode(wire, n, room, text));
}

static void test_rooms(void) {
    char name[8];
    unsigned i;
    mpc_init(&st, 8);
    CHECK(st.nrooms == 1u && !strcmp(mpc_current(&st)->name, MPC_DEFAULT_ROOM));
    CHECK(mpc_room_index(&st, "lobby", 0) == -1);
    CHECK(mpc_room_index(&st, "lobby", 1) == 1);
    CHECK(mpc_room_index(&st, "lobby", 1) == 1);
    CHECK(mpc_room_index(&st, "No", 1) == -1);
    for (i = 2; i < MPC_MAX_ROOMS; ++i) {
        snprintf(name, sizeof name, "r%u", i);
        CHECK(mpc_room_index(&st, name, 1) == (int)i);
    }
    CHECK(mpc_room_index(&st, "overflow", 1) == -1);
}

static void test_history_and_paging(void) {
    mpc_room* room;
    char text[16];
    unsigned i;
    mpc_init(&st, 8);
    room = mpc_current(&st);
    mpc_add(&st, room, MPC_SEV_NONE, "Pilot", "hello");
    mpc_add(&st, room, MPC_SEV_INFO, NULL, "Pilot joined");
    CHECK(!strcmp(mpc_at(room, 0)->line, "Pilot: hello"));
    CHECK(!strcmp(mpc_at(room, 1)->line, "[info] Pilot joined"));
    CHECK(mpc_at(room, 2) == NULL);
    CHECK(!strcmp(mpc_last_chat(room)->line, "Pilot: hello"));
    CHECK(mpc_max_severity(room, 0, 8) == MPC_SEV_INFO);
    CHECK(room->first_row == 0u && room->follow);

    for (i = 0; i < 20; ++i) {
        snprintf(text, sizeof text, "m%u", i);
        mpc_add(&st, room, MPC_SEV_NONE, "A", text);
    }
    CHECK(room->count == 22u);
    CHECK(room->first_row == 14u);
    CHECK(mpc_page(&st, room, 0));
    CHECK(room->first_row == 0u && !room->follow);
    mpc_add(&st, room, MPC_SEV_WARNING, NULL, "w");
    CHECK(room->first_row == 0u);
    CHECK(mpc_max_severity(room, 0, 8) == MPC_SEV_INFO);
    CHECK(mpc_max_severity(room, 15, 8) == MPC_SEV_WARNING);
    CHECK(!mpc_page(&st, room, 23));
    CHECK(!mpc_page(&st, room, -1));
    CHECK(mpc_page(&st, room, 15));
    CHECK(room->follow);
    CHECK(mpc_page(&st, room, 3));

    for (i = 0; i < 300; ++i) mpc_add(&st, room, MPC_SEV_NONE, "B", "x");
    CHECK(room->count == MPC_HISTORY);
    CHECK(room->first_row == 0u);
    CHECK(!strcmp(mpc_at(room, MPC_HISTORY - 1u)->line, "B: x"));

    mpc_clear(&st, room);
    CHECK(room->count == 0u && room->follow && mpc_last_chat(room) == NULL);
    CHECK(mpc_page(&st, room, 0));
    CHECK(!mpc_page(&st, room, 1));
}

static void test_no_paging(void) {
    mpc_room* room;
    mpc_init(&st, 0);
    room = mpc_current(&st);
    mpc_add(&st, room, MPC_SEV_NONE, "A", "x");
    CHECK(room->first_row == 0u);
    CHECK(!mpc_page(&st, room, 0));
}

int main(void) {
    test_validation();
    test_clean();
    test_wire();
    test_rooms();
    test_history_and_paging();
    test_no_paging();
    if (failures) {
        fprintf(stderr, "mp_chat_model: %d check(s) failed\n", failures);
        return 1;
    }
    printf("mp_chat_model: all checks passed\n");
    return 0;
}

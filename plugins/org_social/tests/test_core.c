/* SPDX-License-Identifier: GPL-3.0-only
 * The org_social core without a host: model rules, wire schema v1 round trips and refusals,
 * JSON documents and the billboards request parser. Synthetic data only. */
#include "os_json.h"
#include "os_model.h"
#include "os_wire.h"

#include <stdio.h>
#include <string.h>

static int passed, failed;
#define CHECK(c) do { if (c) ++passed; else { ++failed; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static int contains(const char* doc, const char* needle) { return strstr(doc, needle) != NULL; }

static void test_model(void) {
    static os_model m;
    os_session_peer peers[2];
    const double p[3] = { 1.0, 2.0, 3.0 };
    uint32_t rank = 99;
    os_model_init(&m);
    CHECK(os_rank_parse("officer", &rank) && rank == OS_RANK_OFFICER);
    CHECK(!os_rank_parse("guest", &rank));
    CHECK(!os_handle_valid("") && !os_handle_valid(" lead") && !os_handle_valid("tab\there"));
    CHECK(os_member_set(&m, "Alpha", OS_RANK_FOUNDER) == 1);
    CHECK(os_member_set(&m, "Alpha", OS_RANK_DIRECTOR) == 0);
    CHECK(os_member_set(&m, "Beta", OS_ORG_RANKS) == -1);
    CHECK(os_member_set(&m, "Beta", OS_RANK_RECRUIT) == 1);

    memset(peers, 0, sizeof peers);
    peers[0].peer_id = 2; strcpy(peers[0].handle, "Alpha");
    peers[1].peer_id = 3; strcpy(peers[1].handle, "Gamma");
    CHECK(os_session_set(&m, 1, peers, 2) == 1);
    CHECK(os_count_org(&m) == 2 && os_count_online(&m) == 1 && os_count_party(&m) == 2);
    CHECK(os_find_peer(&m, 3) && os_find_peer(&m, 3)->rank == OS_RANK_GUEST);
    CHECK(os_waypoint_put(&m, 3, 7, "Shared", 0, p) == 1);
    CHECK(os_waypoint_put(&m, 0, 0, "Bad", 0, p) == -1);
    CHECK(os_waypoint_put(&m, 0, 4, "Own", 5, p) == 1 && m.next_wp_id == 5);

    CHECK(os_session_set(&m, 1, peers, 1) == 1);   /* Gamma left: guest and its waypoint dropped */
    CHECK(!os_find_member(&m, "Gamma") && !os_find_waypoint(&m, 3, 7) && os_find_waypoint(&m, 0, 4));
    CHECK(os_member_remove(&m, "Alpha") == 1);     /* still in session: stays as a guest */
    CHECK(os_find_member(&m, "Alpha") && !os_find_member(&m, "Alpha")->in_org);
    CHECK(os_member_remove(&m, "Alpha") == 0);
    CHECK(os_session_set(&m, 0, NULL, 0) == 1 && !os_find_member(&m, "Alpha") && os_count_party(&m) == 0);
    CHECK(os_waypoint_remove(&m, 0, 4) == 1 && os_waypoint_remove(&m, 0, 4) == 0);
}

static void test_wire(void) {
    os_msg in, out;
    uint8_t buf[OS_WIRE_MAX_ROSTER];
    size_t n;
    static const uint8_t hello[] = { 1, 1, 6, 0, 1, 2, 3, 'A', 'b', 'c' };
    memset(&in, 0, sizeof in);
    in.type = OS_MSG_WAYPOINT_SET; in.id = 0x01020304u; in.zone = 0x1122334455667788ull;
    in.pos[0] = 1.5; in.pos[1] = -2.25; in.pos[2] = 1e6;
    memset(in.text, 'w', OS_MAX_WP_NAME); in.text[OS_MAX_WP_NAME] = 0;
    n = os_wire_encode(&in, buf, sizeof buf);
    CHECK(n == 4u + 4u + 8u + 24u + 1u + OS_MAX_WP_NAME && n <= OS_WIRE_MAX_ROSTER);
    CHECK(buf[0] == 1 && buf[1] == OS_MSG_WAYPOINT_SET && buf[2] == (uint8_t)(n - 4) && buf[3] == 0);
    CHECK(buf[4] == 0x04 && buf[7] == 0x01 && buf[8] == 0x88 && buf[15] == 0x11);   /* little-endian */
    CHECK(os_wire_decode(buf, n, &out) && out.id == in.id && out.zone == in.zone);
    CHECK(out.pos[0] == 1.5 && out.pos[1] == -2.25 && out.pos[2] == 1e6 && strcmp(out.text, in.text) == 0);
    CHECK(!os_wire_decode(buf, n - 1, &out));
    buf[0] = 2; CHECK(!os_wire_decode(buf, n, &out)); buf[0] = 1;
    CHECK(os_wire_encode(&in, buf, 20) == 0);

    CHECK(os_wire_decode(hello, sizeof hello, &out) && out.type == OS_MSG_HELLO && out.status == OS_STATUS_BUSY &&
          out.rank == OS_RANK_OFFICER && strcmp(out.text, "Abc") == 0);
    memcpy(buf, hello, sizeof hello); buf[8] = 0;
    CHECK(!os_wire_decode(buf, sizeof hello, &out));
    memset(&in, 0, sizeof in);
    in.type = OS_MSG_POSE; in.zone = 9; in.pos[2] = 4.0;
    n = os_wire_encode(&in, buf, sizeof buf);
    CHECK(n == OS_WIRE_MAX_POSE && os_wire_decode(buf, n, &out) && out.zone == 9 && out.pos[2] == 4.0);
    in.type = 77; CHECK(os_wire_encode(&in, buf, sizeof buf) == 0);
    in.type = OS_MSG_HELLO; strcpy(in.text, "bad\x01name"); CHECK(os_wire_encode(&in, buf, sizeof buf) == 0);
}

static int shift_frame(void* ctx, uint64_t from, uint64_t to, const double v[3], double out[3]) {
    (void)ctx;
    if (from == 99 || to == 99) return 0;
    out[0] = v[0] + 100.0 * (double)from - 100.0 * (double)to;
    out[1] = v[1];
    out[2] = v[2];
    return 1;
}

static void test_json(void) {
    static os_model m;
    static char doc[8192];
    os_json j;
    os_view view;
    os_session_peer peer;
    const double a[3] = { 3.0, 4.0, 0.0 }, b[3] = { 1.0, 0.0, 0.0 };
    os_model_init(&m);
    (void)os_member_set(&m, "Quote\"d", OS_RANK_MEMBER);
    memset(&peer, 0, sizeof peer); peer.peer_id = 2; strcpy(peer.handle, "Quote\"d");
    (void)os_session_set(&m, 1, &peer, 1);
    (void)os_waypoint_put(&m, 0, 1, "Same", 0, a);
    (void)os_waypoint_put(&m, 0, 2, "Moon", 1, b);
    (void)os_waypoint_put(&m, 0, 3, "Lost", 99, b);

    os_json_init(&j, doc, sizeof doc - 1);
    os_json_roster(&j, &m, 1);
    CHECK(j.len < sizeof doc); doc[j.len] = 0;
    CHECK(contains(doc, "\"schema\":\"org_social.roster/1\""));
    CHECK(contains(doc, "\"handle\":\"Quote\\\"d\",\"rank\":\"member\",\"in_org\":true,\"online\":true,\"peer_id\":2"));
    CHECK(contains(doc, "\"counts\":{\"org\":1,\"online\":1,\"party\":1}"));

    os_json_init(&j, NULL, 0);
    os_json_roster(&j, &m, 1);
    CHECK(j.len > 100);   /* size probe with no buffer */

    memset(&view, 0, sizeof view);
    CHECK(os_view_parse("0 0 0 0", 7, &view) && view.has_camera && view.frame == 0);
    CHECK(os_view_parse("", 0, &view) && !view.has_camera);
    CHECK(!os_view_parse("0 1 2", 5, &view) && !os_view_parse("x", 1, &view) && !os_view_parse("0 nan 0 0", 9, &view));
    CHECK(!os_view_parse("0\0junk", 6, &view));
    CHECK(!os_view_parse("18446744073709551616", 20, &view));
    CHECK(os_view_parse("0 0 0 0", 7, &view));
    view.to_frame = shift_frame;
    os_json_init(&j, doc, sizeof doc - 1);
    os_json_billboards(&j, &m, &view, 1);
    CHECK(j.len < sizeof doc); doc[j.len] = 0;
    CHECK(contains(doc, "\"label\":\"Same\",\"color\":\"#4fc3f7\",\"zone\":0,\"resolved\":true,\"method\":\"same_frame\",\"pos\":[3,4,0],\"distance_m\":5,"));
    CHECK(contains(doc, "\"label\":\"Moon\",\"color\":\"#4fc3f7\",\"zone\":1,\"resolved\":true,\"method\":\"pose_to_zone\",\"pos\":[101,0,0]"));
    CHECK(contains(doc, "\"label\":\"Lost\",\"color\":\"#4fc3f7\",\"zone\":99,\"resolved\":false,\"method\":\"unresolved\",\"pos\":null"));
}

int main(void) {
    test_model();
    test_wire();
    test_json();
    printf("org_social core: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

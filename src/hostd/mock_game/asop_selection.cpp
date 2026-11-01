// Synthetic fixed-document editor; no real file or native terminal is touched.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_asop_selection.h"
#include "sco/net/sha2.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace scuba::hostd::mock_game {
namespace {
struct Receipt { uint64_t id, key, revision, committedRevision; std::vector<std::string> classes; uint8_t digest[32]; uint32_t phase, reason; };
struct StateData {
    uint64_t session = 0, revision = 1, semantic = 1, lease = 0, nextLease = 0, nextId = 0, highKey = 0;
    sco_plugin* editor = nullptr; const void* lastEditor = nullptr; uint64_t lastLease = 0;
    bool present = false; std::vector<std::string> classes, catalogue; std::vector<Receipt> receipts; uint64_t pending = 0;
} g;
bool EqualFold(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
// The mock has no fleet provider: any matching scene vehicle is conservatively busy until
// removed from the scene, including while unstreamed or pending removal.
bool Busy(const std::string& cls) {
    for (const auto& [id, entity] : GetScene().entities) {
        (void)id;
        if (entity.vehicle && EqualFold(entity.cls, cls)) return true;
    }
    return false;
}
// Older SDKs can report an edit conflict but cannot distinguish its busy reason.
#ifdef SC_ASOP_SELECTION_REASON_BUSY
constexpr uint32_t kBusyReason = SC_ASOP_SELECTION_REASON_BUSY;
#else
constexpr uint32_t kBusyReason = SC_ASOP_SELECTION_REASON_CONFLICT;
#endif
bool RemovesBusy(const std::vector<std::string>& next) {
    for (const auto& saved : g.classes) {
        if (std::none_of(next.begin(), next.end(), [&](const auto& cls) { return EqualFold(saved, cls); }) && Busy(saved)) return true;
    }
    return false;
}
void Digest(uint8_t* out) { std::string s; for (auto& x : g.classes) { s += x; s += '\n'; } sco_sha256_digest(s.data(), s.size(), out); }
sco_result Check(sco_plugin* self) { if (!OnGame()) return SCO_WRONG_THREAD; if (!LivePlugin(self)) return SCO_BAD_ARG; return Ready("game.asop.selection.file") ? SCO_OK : SCO_UNAVAILABLE; }
sco_result State(sco_plugin* self, sc_asop_selection_state* out) {
    auto r = Check(self); if (r != SCO_OK) return r;
    if (!out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    sc_asop_selection_state v{}; v.size = sizeof(v); v.flags = SC_ASOP_SELECTION_FILE_KNOWN | (g.present ? SC_ASOP_SELECTION_FILE_PRESENT : 0);
    v.session = g.session; v.file_revision = g.revision; v.catalogue_generation = 1; v.semantic_revision = g.semantic; v.file_count = static_cast<uint32_t>(g.classes.size()); Digest(v.file_digest); *out = v; return SCO_OK;
}
sco_result Refresh(sco_plugin* self) { return Check(self); }
sco_result Rows(sco_plugin* self, const sc_asop_selection_read* read, sc_asop_selection_row* out, uint32_t stride, uint32_t capacity, uint32_t* count, uint32_t* more) {
    auto r = Check(self); if (r != SCO_OK) return r;
    if (!read || read->size < sizeof(*read) || read->reserved || !count || !more || count == more || capacity > SC_ASOP_SELECTION_MAX_PAGE ||
        (capacity && (!out || stride < sizeof(*out) || stride > SC_ASOP_SELECTION_MAX_STRIDE || stride % alignof(sc_asop_selection_row) || reinterpret_cast<uintptr_t>(out) % alignof(sc_asop_selection_row)))) return SCO_BAD_ARG;
    const std::vector<std::string>* values = &g.classes; uint64_t revision = g.revision;
    if (read->kind == SC_ASOP_SELECTION_ROWS_CATALOGUE) { values = &g.catalogue; revision = 1; }
    else if (read->kind != SC_ASOP_SELECTION_ROWS_FILE) return SCO_BAD_ARG;
    if (!read->session || read->session != g.session || !read->revision || read->revision != revision) return SCO_NOT_FOUND;
    if (read->offset > values->size()) return SCO_BAD_ARG;
    uint32_t available = static_cast<uint32_t>(values->size() - read->offset), n = std::min(capacity, available); auto* bytes = reinterpret_cast<unsigned char*>(out);
    for (uint32_t i = 0; i < n; ++i) { uint32_t size; std::memcpy(&size, bytes + static_cast<size_t>(i) * stride, sizeof(size)); if (size < sizeof(*out) || (*values)[read->offset+i].size() > SC_ASOP_SELECTION_MAX_CLASS_BYTES) return SCO_BAD_ARG; }
    for (uint32_t i = 0; i < n; ++i) {
        sc_asop_selection_row row{}; row.size = sizeof(row);
#ifdef SC_ASOP_SELECTION_ROW_BUSY
        if (Busy((*values)[read->offset+i])) row.flags |= SC_ASOP_SELECTION_ROW_BUSY;
#endif
        std::memcpy(row.class_name, (*values)[read->offset+i].c_str(), (*values)[read->offset+i].size()+1);
        std::memcpy(bytes + static_cast<size_t>(i)*stride, &row, sizeof(row));
    }
    *count = n; *more = available - n; return SCO_OK;
}
sco_result Acquire(sco_plugin* self, uint64_t session, uint64_t* out) {
    auto r = Check(self); if (r != SCO_OK) return r; if (!out) return SCO_BAD_ARG; if (!session || session != g.session) return SCO_NOT_FOUND;
    if (g.editor && g.editor != self) return SCO_TOO_MANY;
    if (!g.editor) { g.editor = self; g.lease = ++g.nextLease; g.highKey = 0; g.receipts.clear(); g.pending = 0; } *out = g.lease; return SCO_OK;
}
void Revoke(const void* owner) { if (g.editor != owner) return; for (auto& r : g.receipts) if (r.id == g.pending) r.phase = SC_ASOP_SELECTION_EDIT_REVOKED; g.pending = 0; g.lastEditor = owner; g.lastLease = g.lease; g.editor = nullptr; g.lease = 0; }
sco_result Release(sco_plugin* self, uint64_t lease) { auto r = Check(self); if (r != SCO_OK) return r; if (!lease) return SCO_BAD_ARG; if (g.editor == self && g.lease == lease) { Revoke(self); return SCO_OK; } return g.lastEditor == self && g.lastLease == lease ? SCO_OK : SCO_NOT_FOUND; }
sco_result Replace(sco_plugin* self, uint64_t lease, const sc_asop_selection_request* req, uint64_t* out) {
    auto r = Check(self); if (r != SCO_OK) return r;
    if (!lease || !req || req->size < sizeof(*req) || req->flags || !req->client_key || !out || req->count > SC_ASOP_SELECTION_MAX_CLASSES ||
        (req->count && (!req->classes || req->class_stride < sizeof(sc_asop_selection_class) || req->class_stride > SC_ASOP_SELECTION_MAX_STRIDE || req->class_stride % alignof(sc_asop_selection_class) || reinterpret_cast<uintptr_t>(req->classes) % alignof(sc_asop_selection_class)))) return SCO_BAD_ARG;
    std::vector<std::string> classes; const auto* bytes = reinterpret_cast<const unsigned char*>(req->classes);
    for (uint32_t i = 0; i < req->count; ++i) {
        sc_asop_selection_class item; std::memcpy(&item, bytes + static_cast<size_t>(i)*req->class_stride, sizeof(item)); const char* end = static_cast<const char*>(std::memchr(item.class_name, 0, sizeof(item.class_name)));
        if (item.size < sizeof(item) || item.flags || !end || end == item.class_name) return SCO_BAD_ARG;
        std::string s(item.class_name, static_cast<size_t>(end - item.class_name)); for (char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return SCO_BAD_ARG;
        for (const auto& prior : classes) { if (EqualFold(prior, s)) return SCO_BAD_ARG; } classes.push_back(s);
    }
    if (g.editor != self || g.lease != lease) return SCO_NOT_FOUND;
    for (const auto& saved : g.receipts) if (saved.key == req->client_key) { if (req->session != g.session || saved.revision != req->file_revision || std::memcmp(saved.digest, req->file_digest, 32) || saved.classes != classes) return SCO_BAD_ARG; *out = saved.id; return SCO_OK; }
    if (req->client_key <= g.highKey) return SCO_NOT_FOUND;
    uint8_t digest[32]; Digest(digest);
    if (req->session != g.session || req->file_revision != g.revision || std::memcmp(digest, req->file_digest, 32)) return SCO_NOT_FOUND;
    if (g.pending) return SCO_TOO_MANY;
    Receipt receipt{}; receipt.id = ++g.nextId; receipt.key = req->client_key; receipt.revision = req->file_revision; receipt.classes = std::move(classes); std::memcpy(receipt.digest, req->file_digest, 32); receipt.phase = SC_ASOP_SELECTION_EDIT_ACCEPTED;
    g.pending = receipt.id; g.highKey = receipt.key; *out = receipt.id; g.receipts.push_back(std::move(receipt)); if (g.receipts.size() > 32) g.receipts.erase(g.receipts.begin()); return SCO_OK;
}
sco_result Status(sco_plugin* self, uint64_t lease, uint64_t id, sc_asop_selection_receipt* out) {
    auto r = Check(self); if (r != SCO_OK) return r; if (!lease || !id || !out || out->size < sizeof(*out)) return SCO_BAD_ARG; if (g.editor != self || g.lease != lease) return SCO_NOT_FOUND;
    for (const auto& item : g.receipts) if (item.id == id) { sc_asop_selection_receipt v{}; v.size = sizeof(v); v.phase = item.phase; v.session = g.session; v.request_id = id; v.client_key = item.key; v.reason = item.reason; v.file_revision = item.phase == SC_ASOP_SELECTION_FILE_COMMITTED ? item.committedRevision : item.revision; *out = v; return SCO_OK; }
    return SCO_NOT_FOUND;
}
void Reset() { auto lease = g.nextLease, id = g.nextId; g = StateData{}; g.nextLease = lease; g.nextId = id; g.session = GetScene().session; g.catalogue.assign(GetScene().classes.begin(), GetScene().classes.end()); }
void Tick() { if (!g.pending) return; for (auto& item : g.receipts) if (item.id == g.pending) { if (!Ready("game.asop.selection.file") || !g.editor || !LivePlugin(g.editor)) { item.phase = SC_ASOP_SELECTION_EDIT_REVOKED; } else if (item.revision != g.revision) { item.phase = SC_ASOP_SELECTION_EDIT_CONFLICT; item.reason = SC_ASOP_SELECTION_REASON_CONFLICT; } else if (RemovesBusy(item.classes)) { item.phase = SC_ASOP_SELECTION_EDIT_CONFLICT; item.reason = kBusyReason; } else { if (g.classes != item.classes) ++g.semantic; g.classes = item.classes; g.present = true; ++g.revision; item.committedRevision = g.revision; item.phase = SC_ASOP_SELECTION_FILE_COMMITTED; } break; } g.pending = 0; }
const sc_asop_selection_v1 table{sizeof(table), 0, State, Refresh, Rows, Acquire, Release, Replace, Status};
}
const ServiceDef& AsopSelectionService() { static const ServiceDef def{SC_ASOP_SELECTION_NAME, SC_ASOP_SELECTION_VERSION, &table, Reset, Revoke, Tick}; return def; }
}

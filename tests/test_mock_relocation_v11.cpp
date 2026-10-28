#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_game_spatial.h"
#include "sc_relocation.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <set>

namespace mg = scuba::hostd::mock_game;
namespace scuba::hostd::mock_game {
static Scene scene;
static std::set<std::string> caps{"game.relocation", "game.relocation.position", "game.spatial"};
Scene& GetScene() { return scene; }
bool OnGame() { return true; }
bool LivePlugin(const sco_plugin* p) { return p != nullptr; }
bool Ready(const char* c) { return caps.count(c) != 0; }
bool Finite(const double* a, size_t n) { for (size_t i=0;i<n;++i) if (!std::isfinite(a[i])) return false; return true; }
bool NormalizeQuat(const double* a, Quat& q) { double n=0; for(size_t i=0;i<4;++i) n+=a[i]*a[i]; if (!std::isfinite(n)||n<1e-24) return false; for(size_t i=0;i<4;++i) q[i]=a[i]/std::sqrt(n); return true; }
bool ZoneKnown(uint64_t z) { return z==0 || scene.zones.count(z); }
bool ZoneStreamed(uint64_t z) { return z && scene.zones.count(z); }
uint64_t RootOf(uint64_t z) { std::set<uint64_t> seen; while(z && scene.zones.count(z) && seen.insert(z).second) { auto p=scene.zones.at(z).parent; if(!p) return z; z=p; } return 0; }
bool ZoneToZone(uint64_t, uint64_t, const Vec3& v, Vec3& o) { o=v; return true; }
Entity* FindEntity(uint64_t id) { auto it=scene.entities.find(id); return it==scene.entities.end()?nullptr:&it->second; }
bool MovePlayer(uint64_t z, const Vec3& p, const Quat* q) { auto* e=FindEntity(scene.player); if(!e) return false; e->zone=z; e->pos=p; if(q) e->rot=*q; return true; }
}
int main() {
    auto* self=reinterpret_cast<sco_plugin*>(1);
    auto& s=mg::GetScene(); s.zones.emplace(1,mg::Zone{1,0,"root",{}}); s.player=42; s.entities[42].zone=1;
    mg::GameSpatialService().reset(); mg::RelocationService().reset();
    const auto& def=mg::RelocationService();
    assert(def.version==SC_RELOCATION_VERSION_1_1);
    auto* r=static_cast<const sc_relocation_v1_1*>(def.table);
    assert(r->size>=sizeof(*r) && r->request_position);
    auto* spatial=static_cast<const sc_game_spatial_v1*>(mg::GameSpatialService().table);
    sc_game_zone z{}; z.size=sizeof(z); assert(spatial->zone_record(1,&z));
    sc_relocation_position_request req{}; req.size=sizeof(req); req.timeout_ms=1000; req.zone_id=1; req.target_epoch=z.epoch; req.pos[0]=12;
    uint64_t ticket=0; assert(r->request_position(self,&req,&ticket)==SCO_OK && ticket);
    def.tick(); assert(s.entities[42].pos[0]==12 && s.entities[42].rot[3]==1);
    def.tick(); sc_relocation_status st{}; st.size=sizeof(st); assert(r->status(self,ticket,&st)==SCO_OK && st.state==SC_RELOCATION_COMPLETE);
}

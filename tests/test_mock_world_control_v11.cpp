#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_world_control.h"
#include <cassert>
#include <cmath>
namespace mg = scuba::hostd::mock_game;
namespace scuba::hostd::mock_game {
static Scene scene;
Scene& GetScene() { return scene; }
bool OnGame() { return true; }
bool LivePlugin(const sco_plugin* p) { return p != nullptr; }
bool Ready(const char*) { return true; }
bool Finite(const double* a, size_t n) { for(size_t i=0;i<n;++i) if(!std::isfinite(a[i])) return false; return true; }
bool ZoneStreamed(uint64_t z) { return scene.zones.count(z); }
bool ZoneToZone(uint64_t, uint64_t, const Vec3& v, Vec3& out) { out=v; return true; }
Entity* FindEntity(uint64_t id) { auto i=scene.entities.find(id); return i==scene.entities.end()?nullptr:&i->second; }
bool NormalizeQuat(const double* a, Quat& q) { for(size_t i=0;i<4;++i) q[i]=a[i]; return true; }
Quat QuatMul(const Quat& a, const Quat&) { return a; }
sco_result CastRay(uint64_t, const double*, const double*, double, const std::set<uint64_t>&, sc_world_hit*, std::string&) { return SCO_NOT_FOUND; }
}
int main() {
    auto self=reinterpret_cast<sco_plugin*>(1);
    auto& s=mg::GetScene(); s.zones.emplace(1,mg::Zone{1,2,"zone",{}}); s.zones.emplace(2,mg::Zone{2,0,"root",{}});
    s.entities[1].zone=1; s.entities[1].pos={2,0,1}; s.entities[1].radius=0.5;
    s.entities[2].zone=1; s.entities[2].pos={4,0,1}; s.entities[2].radius=0.5;
    auto& def=mg::WorldControlService(); assert(def.version==SC_WORLD_CONTROL_VERSION_1_1);
    auto* api=static_cast<const sc_world_control_v1*>(def.table); assert(api->raycast_filtered_many);
    double from[3]={0,0,1}, dir[3]={1,0,0}; sc_world_hits hits{}; hits.size=sizeof(hits); hits.count=1;
    assert(api->raycast_filtered_many(self,1,from,dir,10,nullptr,0,&hits)==SCO_OK);
    assert(hits.count==1 && hits.more==1 && hits.hits[0].distance==1.5);
}

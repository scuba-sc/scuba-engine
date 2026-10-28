#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include "sc_gamerules.h"
#include <cassert>
#include <cstring>
namespace mg = scuba::hostd::mock_game;
namespace scuba::hostd::mock_game {
static Scene scene;
Scene& GetScene() { return scene; }
bool OnGame() { return true; }
bool LivePlugin(const sco_plugin* p) { return p != nullptr; }
bool Ready(const char*) { return false; }
bool Published() { return true; }
std::vector<ServiceInfo> Services() { return {}; }
}
int main() {
    auto* self = reinterpret_cast<sco_plugin*>(1);
    const auto& def=mg::GameRulesService();
    assert(def.version==SC_GAMERULES_VERSION_1_1);
    const auto* t=static_cast<const sc_gamerules_v1*>(def.table);
    uint32_t count=99,total=99;
    assert(t->list_params(self,0,nullptr,0,0,&count,&total)==SCO_OK);
    assert(count==0 && total==15);
    sc_gamerules_param_info records[2]{}; records[0].size=sizeof(records[0]); records[1].size=sizeof(records[1]);
    assert(t->list_params(self,0,records,sizeof(records[0]),2,&count,&total)==SCO_OK);
    assert(count==2 && !std::strcmp(records[0].name,"capturableDebug"));
}

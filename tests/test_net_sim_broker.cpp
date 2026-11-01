#include "check.h"
#include "hostd/net_sim/broker.h"

#include <stdexcept>
#include <string>

using scuba::hostd::net_sim::Broker;

int main(int argc, char** argv) {
    CHECK(argc == 2);
    Broker broker(argv[1]);
    broker.Start(2, "", false);
    CHECK(broker.Stop() == false);

    bool restartRefused = false;
    try {
        broker.Start(2, "", false);
    } catch (const std::runtime_error& error) {
        restartRefused = std::string(error.what()).find("new sessions are disabled") != std::string::npos;
    }
    CHECK(restartRefused);
    CHECK(broker.Stop() == false);
    return CheckExit("net_sim_broker_unclean_shutdown");
}

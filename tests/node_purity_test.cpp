#include "check.hpp"

#include <fstream>
#include <string>

int main()
{
    const char* files[]{
        "include/sim/node.hpp",
        "include/sim/effects.hpp",
        "include/domain/local_ledger.hpp",
        "include/domain/trade.hpp",
        "include/domain/log_records.hpp",
        "include/domain/durable_log.hpp",
        "include/domain/replay.hpp",
        "include/domain/log_records.hpp",
        "include/domain/durable_log.hpp",
        "include/domain/replay.hpp",
    };
    for (const char* file : files) {
        std::ifstream input{file};
        CHECK(input.is_open());
        std::string line;
        while (std::getline(input, line)) {
            CHECK(line.find("#include \"sim/network_model.hpp\"") ==
                  std::string::npos);
            CHECK(line.find("#include \"sim/deterministic_rng.hpp\"") ==
                  std::string::npos);
            CHECK(line.find("#include \"sim/virtual_clock.hpp\"") ==
                  std::string::npos);
            CHECK(line.find("#include \"sim/simulator.hpp\"") ==
                  std::string::npos);
            if (std::string{file} ==
                "include/domain/local_ledger.hpp") {
                CHECK(line.find("#include \"sim/event_queue.hpp\"") ==
                      std::string::npos);
                CHECK(line.find("#include \"sim/effects.hpp\"") ==
                      std::string::npos);
            }
        }
    }
    std::ifstream simulator_source{"src/simulator.cpp"};
    CHECK(simulator_source.is_open());
    std::string line;
    int timer_scheduling_sites = 0;
    while (std::getline(simulator_source, line)) {
        if (line.find("            EventKind::fire_timer,") !=
            std::string::npos) {
            ++timer_scheduling_sites;
        }
    }
    CHECK(timer_scheduling_sites == 1);
    return 0;
}

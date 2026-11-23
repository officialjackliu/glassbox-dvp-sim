#include "check.hpp"

#include <fstream>
#include <string>

int main()
{
    const char* files[]{
        "include/sim/node.hpp",
        "include/sim/effects.hpp",
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

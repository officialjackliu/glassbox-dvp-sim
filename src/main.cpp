#include "sim/core_types.hpp"

#include <iostream>

int main()
{
    const glassbox::TradeId trade_id{1};
    const glassbox::Cents cash{100};
    std::cout << "glassbox-dvp-sim\n";
    std::cout << "trade=" << trade_id.value()
              << " cents=" << cash.value() << '\n';
    return 0;
}

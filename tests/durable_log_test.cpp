#include "domain/durable_log.hpp"
#include "check.hpp"

#include <array>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>

namespace {

glassbox::Trade make_trade(glassbox::TradeId id)
{
    glassbox::TradeIdAllocator allocator;
    if (id.value() > 1) {
        allocator.observe(glassbox::TradeId{id.value() - 1});
    }
    glassbox::TradeRequest request{
        glassbox::NodeId::bank_a,
        glassbox::NodeId::bank_b,
        "UST_10Y",
        glassbox::Units{10},
        glassbox::Cents{100},
        100'000,
        "test",
    };
    return std::get<glassbox::Trade>(
        glassbox::validate_and_assign(request, 0, allocator));
}

} // namespace

int main()
{
    glassbox::DurableLog log;
    CHECK(log.entries().size() == 0);
    CHECK(log.size() == 0);

    const auto first = log.append(
        glassbox::Proposed{make_trade(glassbox::TradeId{1})},
        100);
    const auto second = log.append(
        glassbox::DecisionCommit{
            glassbox::TradeId{1},
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
        },
        200);

    CHECK(first && first->lsn == 1);
    CHECK(first->durable_at_us == 100);
    CHECK(second && second->lsn == 2);
    CHECK(second->durable_at_us == 200);
    CHECK(log.size() == 2);

    const auto snapshot = log.snapshot();
    CHECK(snapshot.size() == 2);
    CHECK(log.append(
        glassbox::End{glassbox::TradeId{1}},
        300));
    CHECK(snapshot.size() == 2);
    CHECK(log.snapshot().size() == 3);
    CHECK(log.entries()[0].lsn == 1);
    CHECK(log.entries()[1].lsn == 2);
    CHECK(log.entries()[2].lsn == 3);

    CHECK(glassbox::record_kind(snapshot[0].record) ==
          "PROPOSED");
    CHECK(glassbox::record_kind(snapshot[1].record) ==
          "DECISION_COMMIT");
    const auto final_snapshot = log.snapshot();
    CHECK(final_snapshot.size() == 3);
    CHECK(glassbox::record_kind(final_snapshot[2].record) ==
          "END");

    const std::array<glassbox::LogRecord, 7> all_kinds{
        glassbox::Prepared{
            make_trade(glassbox::TradeId{1}),
            glassbox::Leg::buyer,
        },
        glassbox::Committed{
            glassbox::TradeId{1},
            glassbox::Leg::buyer,
        },
        glassbox::Aborted{glassbox::TradeId{1}},
        glassbox::Proposed{
            make_trade(glassbox::TradeId{1}),
        },
        glassbox::DecisionCommit{
            glassbox::TradeId{1},
            glassbox::NodeId::bank_a,
            glassbox::NodeId::bank_b,
        },
        glassbox::DecisionAbort{
            glassbox::TradeId{1},
            glassbox::ReasonCode::expired,
        },
        glassbox::End{glassbox::TradeId{1}},
    };
    const std::array<std::string_view, 7> kind_names{
        "PREPARED",
        "COMMITTED",
        "ABORTED",
        "PROPOSED",
        "DECISION_COMMIT",
        "DECISION_ABORT",
        "END",
    };
    for (std::size_t i = 0; i < all_kinds.size(); ++i) {
        CHECK(glassbox::record_kind(all_kinds[i]) ==
              kind_names[i]);
    }

    std::ifstream header{
        "include/domain/durable_log.hpp"};
    CHECK(header.is_open());
    std::string line;
    while (std::getline(header, line)) {
        CHECK(line.find("erase") == std::string::npos);
        CHECK(line.find("clear") == std::string::npos);
        CHECK(line.find("float") == std::string::npos);
        CHECK(line.find("double") == std::string::npos);
    }

    return 0;
}

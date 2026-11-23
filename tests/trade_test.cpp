#include "domain/trade.hpp"
#include "check.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

static_assert(!std::is_constructible_v<
              glassbox::Trade,
              glassbox::TradeId,
              glassbox::NodeId,
              glassbox::NodeId,
              glassbox::Asset,
              glassbox::Units,
              glassbox::Cents,
              glassbox::SimTimeUs,
              std::string>);

namespace {

class StubCoordinator {
  public:
    struct Result {
        std::vector<glassbox::Trade> accepted;
        std::vector<glassbox::TradeRejectReason> rejected;
    };

    [[nodiscard]] Result submit_batch(
        const std::vector<glassbox::TradeRequest>& requests,
        glassbox::SimTimeUs now)
    {
        Result result;
        for (const glassbox::TradeRequest& request : requests) {
            glassbox::TradeResult trade_result =
                glassbox::validate_and_assign(
                    request,
                    now,
                    allocator_);
            if (const auto* trade =
                    std::get_if<glassbox::Trade>(&trade_result)) {
                result.accepted.push_back(*trade);
            } else {
                result.rejected.push_back(
                    std::get<glassbox::TradeRejectReason>(
                        trade_result));
            }
        }
        return result;
    }

  private:
    glassbox::TradeIdAllocator allocator_;
};

[[nodiscard]] glassbox::TradeRequest valid_request()
{
    return glassbox::TradeRequest{
        glassbox::NodeId::bank_a,
        glassbox::NodeId::bank_b,
        "UST_10Y",
        glassbox::Units{10},
        glassbox::Cents{25'000},
        1'000,
        "bank_a-desk",
    };
}

} // namespace

int main()
{
    // Asset parsing.
    CHECK(glassbox::parse_asset("UST_10Y") ==
          glassbox::Asset::ust_10y);
    CHECK(!glassbox::parse_asset(""));
    CHECK(!glassbox::parse_asset("ust_10y"));
    CHECK(!glassbox::parse_asset("UST_10Y "));
    CHECK(!glassbox::parse_asset("AAPL"));
    CHECK(glassbox::to_string(glassbox::Asset::ust_10y) ==
          "UST_10Y");

    // Exact, distinct trace labels for every trade rejection reason.
    {
        const std::array<std::pair<
                          glassbox::TradeRejectReason,
                          std::string_view>,
                          7> labels{
            std::pair{
                glassbox::TradeRejectReason::unknown_node,
                "UNKNOWN_NODE"},
            std::pair{
                glassbox::TradeRejectReason::same_party,
                "SAME_PARTY"},
            std::pair{
                glassbox::TradeRejectReason::invalid_asset,
                "INVALID_ASSET"},
            std::pair{
                glassbox::TradeRejectReason::invalid_quantity,
                "INVALID_QUANTITY"},
            std::pair{
                glassbox::TradeRejectReason::invalid_price,
                "INVALID_PRICE"},
            std::pair{
                glassbox::TradeRejectReason::expired,
                "EXPIRED"},
            std::pair{
                glassbox::TradeRejectReason::id_exhausted,
                "ID_EXHAUSTED"},
        };

        for (std::size_t i = 0; i < labels.size(); ++i) {
            CHECK(glassbox::to_string(labels[i].first) ==
                  labels[i].second);
            for (std::size_t j = i + 1;
                 j < labels.size();
                 ++j) {
                CHECK(labels[i].second != labels[j].second);
            }
        }
    }

    // Valid request produces an exact Trade.
    {
        glassbox::TradeIdAllocator allocator;
        const auto request = valid_request();
        glassbox::TradeResult result =
            glassbox::validate_and_assign(request, 500, allocator);
        const auto* trade = std::get_if<glassbox::Trade>(&result);
        CHECK(trade != nullptr);
        CHECK(trade->trade_id().value() == 1);
        CHECK(trade->buyer() == glassbox::NodeId::bank_a);
        CHECK(trade->seller() == glassbox::NodeId::bank_b);
        CHECK(trade->asset() == glassbox::Asset::ust_10y);
        CHECK(trade->qty().value() == 10);
        CHECK(trade->cash_cents().value() == 25'000);
        CHECK(trade->valid_until_us() == 1'000);
        CHECK(trade->proposer() == "bank_a-desk");
        CHECK(!glassbox::is_expired(*trade, 999));
        CHECK(glassbox::is_expired(*trade, 1'000));
        CHECK(glassbox::is_expired(*trade, 1'001));
    }

    // Unknown-node checks.
    {
        glassbox::TradeRequest request = valid_request();
        request.buyer = glassbox::NodeId::coordinator;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);
        request = valid_request();
        request.seller = glassbox::NodeId::coordinator;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);
        request = valid_request();
        request.buyer =
            static_cast<glassbox::NodeId>(42);
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);
        request = valid_request();
        request.seller =
            static_cast<glassbox::NodeId>(42);
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);
    }

    // Same-party checks for every participant.
    {
        for (const glassbox::NodeId party :
             {glassbox::NodeId::bank_a,
              glassbox::NodeId::bank_b,
              glassbox::NodeId::bank_c}) {
            glassbox::TradeRequest request = valid_request();
            request.buyer = party;
            request.seller = party;
            CHECK(glassbox::validate(request, 0) ==
                  glassbox::TradeRejectReason::same_party);
        }
    }

    // Individual quantity, price, asset and deadline boundaries.
    {
        glassbox::TradeRequest request = valid_request();
        request.asset = "";
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_asset);
        request.asset = "ust_10y";
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_asset);
        request.asset = "UST_10Y ";
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_asset);
        request.asset = "AAPL";
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_asset);

        request = valid_request();
        request.qty = glassbox::Units{0};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_quantity);
        request.qty = glassbox::Units{-1};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_quantity);
        request.qty = glassbox::Units{1};
        CHECK(!glassbox::validate(request, 0));
        request.qty =
            glassbox::Units{std::numeric_limits<std::int64_t>::max()};
        CHECK(!glassbox::validate(request, 0));

        request = valid_request();
        request.cash_cents = glassbox::Cents{0};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_price);
        request.cash_cents = glassbox::Cents{-1};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_price);
        request.cash_cents = glassbox::Cents{1};
        CHECK(!glassbox::validate(request, 0));

        request = valid_request();
        request.valid_until_us = -1;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::expired);
        request.valid_until_us = 0;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::expired);
        request.valid_until_us = 1;
        CHECK(!glassbox::validate(request, 0));
        request.valid_until_us =
            std::numeric_limits<glassbox::SimTimeUs>::max();
        CHECK(!glassbox::validate(request, 0));
    }

    // Fixed-order precedence advances one field at a time.
    {
        glassbox::TradeRequest request{
            glassbox::NodeId::coordinator,
            glassbox::NodeId::coordinator,
            "",
            glassbox::Units{-1},
            glassbox::Cents{-1},
            0,
            "",
        };
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);

        request.buyer = glassbox::NodeId::bank_a;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::unknown_node);

        request.seller = glassbox::NodeId::bank_a;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::same_party);

        request.seller = glassbox::NodeId::bank_b;
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_asset);

        request.asset = "UST_10Y";
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_quantity);

        request.qty = glassbox::Units{1};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::invalid_price);

        request.cash_cents = glassbox::Cents{1};
        CHECK(glassbox::validate(request, 0) ==
              glassbox::TradeRejectReason::expired);

        request.valid_until_us = 1;
        CHECK(!glassbox::validate(request, 0));
    }

    // Deadline is_expired boundaries.
    {
        glassbox::TradeIdAllocator allocator;
        glassbox::TradeRequest request = valid_request();
        request.valid_until_us = 500;
        const auto result =
            glassbox::validate_and_assign(request, 501, allocator);
        CHECK(std::holds_alternative<glassbox::TradeRejectReason>(
                  result));
        CHECK(std::get<glassbox::TradeRejectReason>(result) ==
              glassbox::TradeRejectReason::expired);

        request.valid_until_us = 502;
        const auto trade_result =
            glassbox::validate_and_assign(request, 501, allocator);
        const auto* trade =
            std::get_if<glassbox::Trade>(&trade_result);
        CHECK(trade != nullptr);
        CHECK(!glassbox::is_expired(*trade, 501));
        CHECK(glassbox::is_expired(*trade, 502));
        CHECK(glassbox::is_expired(*trade, 503));
    }

    // Allocator sequence and observation.
    {
        glassbox::TradeIdAllocator allocator;
        const auto id1 = allocator.next();
        const auto id2 = allocator.next();
        const auto id3 = allocator.next();
        CHECK(id1 && id1->value() == 1);
        CHECK(id2 && id2->value() == 2);
        CHECK(id3 && id3->value() == 3);

        glassbox::TradeIdAllocator many;
        std::array<bool, 1001> unique{};
        for (int i = 0; i < 1000; ++i) {
            const auto id = many.next();
            CHECK(id.has_value());
            CHECK(id->value() >= 1);
            CHECK(id->value() < 1001);
            CHECK(!unique[id->value()]);
            unique[id->value()] = true;
        }
        CHECK(many.next().has_value());

        glassbox::TradeIdAllocator observed;
        observed.observe(glassbox::TradeId{100});
        const auto observed_next = observed.next();
        CHECK(observed_next && observed_next->value() == 101);
        observed.observe(glassbox::TradeId{5});
        CHECK(observed.next().value_or(glassbox::TradeId{0}).value() ==
              102);

        glassbox::TradeIdAllocator near_max;
        near_max.observe(
            glassbox::TradeId{
                std::numeric_limits<glassbox::TradeIdValue>::max() - 2});
        const auto near_last = near_max.next();
        CHECK(near_last &&
              near_last->value() ==
                  std::numeric_limits<glassbox::TradeIdValue>::max() - 1);
        const auto max_last = near_max.next();
        CHECK(max_last &&
              max_last->value() ==
                  std::numeric_limits<glassbox::TradeIdValue>::max());
        CHECK(!near_max.next());

        glassbox::TradeIdAllocator exhausted;
        exhausted.observe(
            glassbox::TradeId{
                std::numeric_limits<glassbox::TradeIdValue>::max()});
        CHECK(!exhausted.next());
    }

    // Rejected requests do not consume IDs.
    {
        glassbox::TradeIdAllocator allocator;
        glassbox::TradeRequest rejected = valid_request();
        rejected.qty = glassbox::Units{0};
        const auto rejected_result =
            glassbox::validate_and_assign(rejected, 0, allocator);
        CHECK(std::holds_alternative<glassbox::TradeRejectReason>(
                  rejected_result));
        CHECK(std::get<glassbox::TradeRejectReason>(rejected_result) ==
              glassbox::TradeRejectReason::invalid_quantity);

        const auto accepted_result =
            glassbox::validate_and_assign(valid_request(), 0, allocator);
        const auto* accepted =
            std::get_if<glassbox::Trade>(&accepted_result);
        CHECK(accepted != nullptr);
        CHECK(accepted->trade_id().value() == 1);
    }

    // Exhaustion through validate_and_assign never yields a Trade.
    {
        glassbox::TradeIdAllocator allocator;
        allocator.observe(
            glassbox::TradeId{
                std::numeric_limits<glassbox::TradeIdValue>::max()});

        for (int i = 0; i < 2; ++i) {
            const auto result =
                glassbox::validate_and_assign(
                    valid_request(),
                    0,
                    allocator);
            CHECK(std::holds_alternative<
                      glassbox::TradeRejectReason>(result));
            CHECK(std::get<glassbox::TradeRejectReason>(result) ==
                  glassbox::TradeRejectReason::id_exhausted);
            CHECK(!std::holds_alternative<glassbox::Trade>(result));
        }
    }

    // Stub coordinator mixed batch and replay determinism.
    {
        std::vector<glassbox::TradeRequest> requests;
        auto valid1 = valid_request();
        requests.push_back(valid1);

        auto invalid_asset = valid_request();
        invalid_asset.asset = "AAPL";
        requests.push_back(invalid_asset);

        auto valid2 = valid_request();
        valid2.buyer = glassbox::NodeId::bank_c;
        requests.push_back(valid2);

        auto invalid_price = valid_request();
        invalid_price.cash_cents = glassbox::Cents{0};
        requests.push_back(invalid_price);

        StubCoordinator first;
        StubCoordinator second;
        const auto first_result = first.submit_batch(requests, 500);
        const auto second_result = second.submit_batch(requests, 500);

        CHECK(first_result.accepted.size() == 2);
        CHECK(first_result.rejected.size() == 2);
        CHECK(first_result.accepted == second_result.accepted);
        CHECK(first_result.rejected == second_result.rejected);
        CHECK(first_result.accepted[0].trade_id().value() == 1);
        CHECK(first_result.accepted[1].trade_id().value() == 2);
        CHECK(first_result.rejected[0] ==
              glassbox::TradeRejectReason::invalid_asset);
        CHECK(first_result.rejected[1] ==
              glassbox::TradeRejectReason::invalid_price);
    }

    // Static grep for no float/double and private-only Trade constructor.
    {
        std::ifstream header{"include/domain/trade.hpp"};
        std::ifstream source{"src/domain/trade.cpp"};
        CHECK(header.is_open());
        CHECK(source.is_open());
        std::string line;
        while (std::getline(header, line)) {
            CHECK(line.find("float") == std::string::npos);
            CHECK(line.find("double") == std::string::npos);
        }
        while (std::getline(source, line)) {
            CHECK(line.find("float") == std::string::npos);
            CHECK(line.find("double") == std::string::npos);
        }
    }

    return 0;
}

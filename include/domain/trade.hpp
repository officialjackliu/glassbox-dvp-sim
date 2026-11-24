#pragma once

// Pure trade request validation and trade ID assignment.
//
// TradeRequest is unvalidated input. Trade is constructible only through
// validate_and_assign(), after fixed-order validation and ID allocation.

#include "sim/core_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace glassbox {

// The one tradable asset in this model: a 10-year government bond.
enum class Asset : std::uint8_t {
    ust_10y,
};

// Returns the Asset named `asset` ("UST_10Y"), or std::nullopt.
[[nodiscard]] constexpr std::optional<Asset> parse_asset(
    std::string_view asset) noexcept
{
    if (asset == "UST_10Y") {
        return Asset::ust_10y;
    }
    return std::nullopt;
}

// Returns the asset's name, "UST_10Y".
[[nodiscard]] constexpr std::string_view to_string(Asset) noexcept
{
    return "UST_10Y";
}

// A trade ticket as a client submits it, not yet checked: `buyer` pays
// `cash_cents` to `seller` for `qty` bonds, if settled before
// `valid_until_us`.
struct TradeRequest {
    NodeId buyer;
    NodeId seller;
    std::string asset;
    Units qty;
    Cents cash_cents;
    SimTimeUs valid_until_us;
    std::string proposer;
};

// Why validate() refused a request, in check order.
enum class TradeRejectReason : std::uint8_t {
    unknown_node,
    same_party,
    invalid_asset,
    invalid_quantity,
    invalid_price,
    expired,
    id_exhausted,
};

class Trade;
class TradeIdAllocator;

// A validated Trade, or why the request was refused.
using TradeResult = std::variant<Trade, TradeRejectReason>;

// Returns the trace spelling of `reason`, e.g. "SAME_PARTY".
[[nodiscard]] constexpr std::string_view to_string(
    TradeRejectReason reason) noexcept
{
    switch (reason) {
    case TradeRejectReason::unknown_node:
        return "UNKNOWN_NODE";
    case TradeRejectReason::same_party:
        return "SAME_PARTY";
    case TradeRejectReason::invalid_asset:
        return "INVALID_ASSET";
    case TradeRejectReason::invalid_quantity:
        return "INVALID_QUANTITY";
    case TradeRejectReason::invalid_price:
        return "INVALID_PRICE";
    case TradeRejectReason::expired:
        return "EXPIRED";
    case TradeRejectReason::id_exhausted:
        return "ID_EXHAUSTED";
    }
    return "UNKNOWN_NODE";
}

// A validated trade with its assigned id. Immutable; the only way to make one
// is validate_and_assign(), so holding a Trade proves it passed validation.
class Trade {
  public:
    // The trade's fields, as validated.
    [[nodiscard]] TradeId trade_id() const noexcept;
    [[nodiscard]] NodeId buyer() const noexcept;
    [[nodiscard]] NodeId seller() const noexcept;
    [[nodiscard]] Asset asset() const noexcept;
    [[nodiscard]] Units qty() const noexcept;
    [[nodiscard]] Cents cash_cents() const noexcept;
    [[nodiscard]] SimTimeUs valid_until_us() const noexcept;
    [[nodiscard]] const std::string& proposer() const noexcept;

    [[nodiscard]] friend bool operator==(
        const Trade&,
        const Trade&) noexcept = default;

  private:
    friend TradeResult validate_and_assign(
        const TradeRequest& request,
        SimTimeUs now,
        TradeIdAllocator& allocator);

    Trade(TradeId trade_id,
          NodeId buyer,
          NodeId seller,
          Asset asset,
          Units qty,
          Cents cash_cents,
          SimTimeUs valid_until_us,
          std::string proposer) noexcept;

    TradeId trade_id_;
    NodeId buyer_;
    NodeId seller_;
    Asset asset_;
    Units qty_;
    Cents cash_cents_;
    SimTimeUs valid_until_us_;
    std::string proposer_;
};

// Hands out trade ids 1, 2, 3, ... for the coordinator.
class TradeIdAllocator {
  public:
    // Returns the next unused id, or std::nullopt once ids are used up.
    [[nodiscard]] std::optional<TradeId> next() noexcept;
    // Records that `existing` is taken (after crash recovery), so next() returns
    // only larger ids.
    void observe(TradeId existing) noexcept;

  private:
    TradeId next_ = TradeId{1};
    bool exhausted_ = false;
};

// Checks `request` at time `now` in a fixed order: known banks, two different
// parties, asset, quantity, price, deadline. Returns the first failure, or
// std::nullopt if the request is valid.
[[nodiscard]] std::optional<TradeRejectReason> validate(
    const TradeRequest& request,
    SimTimeUs now) noexcept;

// Returns true if the trade's deadline has passed: valid_until_us <= now.
[[nodiscard]] bool is_expired(
    const Trade& trade,
    SimTimeUs now) noexcept;

// Validates `request` and, if it passes, assigns the next id from
// `allocator`. A refused request does not use up an id.
[[nodiscard]] TradeResult validate_and_assign(
    const TradeRequest& request,
    SimTimeUs now,
    TradeIdAllocator& allocator);

} // namespace glassbox

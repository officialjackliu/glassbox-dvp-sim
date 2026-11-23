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

enum class Asset : std::uint8_t {
    ust_10y,
};

[[nodiscard]] constexpr std::optional<Asset> parse_asset(
    std::string_view asset) noexcept
{
    if (asset == "UST_10Y") {
        return Asset::ust_10y;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view to_string(Asset) noexcept
{
    return "UST_10Y";
}

struct TradeRequest {
    NodeId buyer;
    NodeId seller;
    std::string asset;
    Units qty;
    Cents cash_cents;
    SimTimeUs valid_until_us;
    std::string proposer;
};

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

using TradeResult = std::variant<Trade, TradeRejectReason>;

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

class Trade {
  public:
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

class TradeIdAllocator {
  public:
    [[nodiscard]] std::optional<TradeId> next() noexcept;
    void observe(TradeId existing) noexcept;

  private:
    TradeId next_ = TradeId{1};
    bool exhausted_ = false;
};

[[nodiscard]] std::optional<TradeRejectReason> validate(
    const TradeRequest& request,
    SimTimeUs now) noexcept;

[[nodiscard]] bool is_expired(
    const Trade& trade,
    SimTimeUs now) noexcept;

[[nodiscard]] TradeResult validate_and_assign(
    const TradeRequest& request,
    SimTimeUs now,
    TradeIdAllocator& allocator);

} // namespace glassbox

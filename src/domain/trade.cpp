#include "domain/trade.hpp"

#include <limits>

namespace glassbox {

namespace {

[[nodiscard]] constexpr bool is_participant(NodeId node) noexcept
{
    switch (node) {
    case NodeId::bank_a:
    case NodeId::bank_b:
    case NodeId::bank_c:
        return true;
    case NodeId::coordinator:
        return false;
    }
    return false;
}

} // namespace

Trade::Trade(TradeId trade_id,
             NodeId buyer,
             NodeId seller,
             Asset asset,
             Units qty,
             Cents cash_cents,
             SimTimeUs valid_until_us,
             std::string proposer) noexcept
    : trade_id_(trade_id),
      buyer_(buyer),
      seller_(seller),
      asset_(asset),
      qty_(qty),
      cash_cents_(cash_cents),
      valid_until_us_(valid_until_us),
      proposer_(std::move(proposer))
{
}

TradeId Trade::trade_id() const noexcept
{
    return trade_id_;
}

NodeId Trade::buyer() const noexcept
{
    return buyer_;
}

NodeId Trade::seller() const noexcept
{
    return seller_;
}

Asset Trade::asset() const noexcept
{
    return asset_;
}

Units Trade::qty() const noexcept
{
    return qty_;
}

Cents Trade::cash_cents() const noexcept
{
    return cash_cents_;
}

SimTimeUs Trade::valid_until_us() const noexcept
{
    return valid_until_us_;
}

const std::string& Trade::proposer() const noexcept
{
    return proposer_;
}

std::optional<TradeId> TradeIdAllocator::next() noexcept
{
    if (exhausted_) {
        return std::nullopt;
    }

    const TradeIdValue current = next_.value();
    if (current == std::numeric_limits<TradeIdValue>::max()) {
        exhausted_ = true;
        return TradeId{current};
    }

    if (current > std::numeric_limits<TradeIdValue>::max() - 1) {
        exhausted_ = true;
        return std::nullopt;
    }
    next_ = TradeId{current + 1};
    return TradeId{current};
}

void TradeIdAllocator::observe(TradeId existing) noexcept
{
    if (exhausted_) {
        return;
    }
    if (existing.value() >= next_.value()) {
        if (existing.value() ==
            std::numeric_limits<TradeIdValue>::max()) {
            exhausted_ = true;
            return;
        }
        next_ = TradeId{existing.value() + 1};
    }
}

std::optional<TradeRejectReason> validate(
    const TradeRequest& request,
    SimTimeUs now) noexcept
{
    if (!is_participant(request.buyer) ||
        !is_participant(request.seller)) {
        return TradeRejectReason::unknown_node;
    }
    if (request.buyer == request.seller) {
        return TradeRejectReason::same_party;
    }
    if (!parse_asset(request.asset)) {
        return TradeRejectReason::invalid_asset;
    }
    if (request.qty.value() <= 0) {
        return TradeRejectReason::invalid_quantity;
    }
    if (request.cash_cents.value() <= 0) {
        return TradeRejectReason::invalid_price;
    }
    if (request.valid_until_us <= now) {
        return TradeRejectReason::expired;
    }
    return std::nullopt;
}

bool is_expired(const Trade& trade, SimTimeUs now) noexcept
{
    return trade.valid_until_us() <= now;
}

TradeResult validate_and_assign(
    const TradeRequest& request,
    SimTimeUs now,
    TradeIdAllocator& allocator)
{
    const auto rejection = validate(request, now);
    if (rejection) {
        return *rejection;
    }

    const auto trade_id = allocator.next();
    if (!trade_id) {
        return TradeRejectReason::id_exhausted;
    }
    const Asset asset = *parse_asset(request.asset);
    return Trade{
        *trade_id,
        request.buyer,
        request.seller,
        asset,
        request.qty,
        request.cash_cents,
        request.valid_until_us,
        request.proposer,
    };
}

} // namespace glassbox

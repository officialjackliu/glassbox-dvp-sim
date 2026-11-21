#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace glassbox {

using SimTimeUs = std::int64_t;
using TradeIdValue = std::int64_t;
using SequenceValue = std::uint64_t;

enum class ReasonCode : std::uint8_t {
    none,
    insufficient_funds,
    insufficient_bonds,
    account_frozen,
    expired,
    vote_timeout,
    already_aborted,
    unknown_trade,
};

enum class NodeId : std::uint8_t {
    coordinator,
    goldman,
    jpm,
    citadel,
};

constexpr std::string_view to_string(NodeId node) noexcept
{
    switch (node) {
    case NodeId::coordinator:
        return "COORD";
    case NodeId::goldman:
        return "GOLDMAN";
    case NodeId::jpm:
        return "JPM";
    case NodeId::citadel:
        return "CITADEL";
    }
    return "UNKNOWN";
}

constexpr std::optional<NodeId> node_id_from_string(std::string_view name) noexcept
{
    if (name == "COORD") {
        return NodeId::coordinator;
    }
    if (name == "GOLDMAN") {
        return NodeId::goldman;
    }
    if (name == "JPM") {
        return NodeId::jpm;
    }
    if (name == "CITADEL") {
        return NodeId::citadel;
    }
    return std::nullopt;
}

namespace detail {

template <typename Tag, typename Underlying>
class ImmutableValue {
  public:
    template <std::integral Input>
    constexpr explicit ImmutableValue(Input value) noexcept
        : value_(static_cast<Underlying>(value))
    {
    }

    [[nodiscard]] constexpr Underlying value() const noexcept
    {
        return value_;
    }

    [[nodiscard]] friend constexpr bool operator==(const ImmutableValue&,
                                                   const ImmutableValue&) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(const ImmutableValue& lhs,
                                                    const ImmutableValue& rhs) noexcept
    {
        return lhs.value_ <=> rhs.value_;
    }

  private:
    Underlying value_;
};

} // namespace detail

struct TradeIdTag {};
struct SequenceTag {};
struct CentsTag {};
struct UnitsTag {};

class TradeId : public detail::ImmutableValue<TradeIdTag, TradeIdValue> {
  public:
    using detail::ImmutableValue<TradeIdTag, TradeIdValue>::ImmutableValue;
};

class SequenceId : public detail::ImmutableValue<SequenceTag, SequenceValue> {
  public:
    using detail::ImmutableValue<SequenceTag, SequenceValue>::ImmutableValue;
};

class Cents : public detail::ImmutableValue<CentsTag, std::int64_t> {
  public:
    using detail::ImmutableValue<CentsTag, std::int64_t>::ImmutableValue;
};

class Units : public detail::ImmutableValue<UnitsTag, std::int64_t> {
  public:
    using detail::ImmutableValue<UnitsTag, std::int64_t>::ImmutableValue;
};

static_assert(std::is_same_v<SimTimeUs, std::int64_t>);
static_assert(std::is_same_v<TradeIdValue, std::int64_t>);
static_assert(std::is_same_v<decltype(Cents{0}.value()), std::int64_t>);
static_assert(std::is_same_v<decltype(Units{0}.value()), std::int64_t>);
static_assert(!std::is_floating_point_v<SimTimeUs>);
static_assert(!std::is_floating_point_v<TradeIdValue>);
static_assert(!std::is_floating_point_v<decltype(Cents{0}.value())>);
static_assert(!std::is_floating_point_v<decltype(Units{0}.value())>);

[[nodiscard]] constexpr std::optional<std::int64_t>
checked_add_i64(std::int64_t lhs, std::int64_t rhs) noexcept
{
    if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
        return std::nullopt;
    }
    if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] constexpr std::optional<std::int64_t>
checked_sub_i64(std::int64_t lhs, std::int64_t rhs) noexcept
{
    if (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) {
        return std::nullopt;
    }
    if (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs) {
        return std::nullopt;
    }
    return lhs - rhs;
}

[[nodiscard]] constexpr std::optional<std::int64_t>
checked_multiply_i64(std::int64_t lhs, std::int64_t rhs) noexcept
{
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    if (lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    if (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }

    if (lhs > 0) {
        if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() / rhs) {
            return std::nullopt;
        }
        if (rhs < 0 && rhs < std::numeric_limits<std::int64_t>::min() / lhs) {
            return std::nullopt;
        }
    } else {
        if (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() / rhs) {
            return std::nullopt;
        }
        if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::max() / rhs) {
            return std::nullopt;
        }
    }
    return lhs * rhs;
}

template <std::integral To, std::integral From>
[[nodiscard]] constexpr std::optional<To> checked_narrow_cast(From value) noexcept
{
    if constexpr (sizeof(To) < sizeof(From)) {
        if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
            value > static_cast<From>(std::numeric_limits<To>::max())) {
            return std::nullopt;
        }
    } else if constexpr (std::is_signed_v<To> && std::is_unsigned_v<From>) {
        if (value > static_cast<From>(std::numeric_limits<To>::max())) {
            return std::nullopt;
        }
    } else if constexpr (std::is_unsigned_v<To> && std::is_signed_v<From>) {
        if (value < 0 ||
            static_cast<std::uint64_t>(value) >
                static_cast<std::uint64_t>(std::numeric_limits<To>::max())) {
            return std::nullopt;
        }
    }
    return static_cast<To>(value);
}

template <typename T>
struct Outcome {
    std::optional<T> value{};
    ReasonCode reason = ReasonCode::none;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return reason == ReasonCode::none;
    }

    [[nodiscard]] static constexpr Outcome success(T success_value)
    {
        return Outcome{std::optional<T>{success_value}, ReasonCode::none};
    }

    [[nodiscard]] static constexpr Outcome failure(ReasonCode failure_reason)
    {
        return Outcome{std::nullopt, failure_reason};
    }
};

} // namespace glassbox

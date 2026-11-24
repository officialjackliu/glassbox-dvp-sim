#pragma once

// Shared immutable domain types and checked integer operations.
//
// This header is the boundary for simulation IDs, virtual time, money, and
// quantities. Import it in domain, protocol, and simulator code rather than
// introducing floating-point or ad-hoc integer aliases.

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace glassbox {

// Simulated (virtual) time in microseconds since the scenario started. Never
// wall-clock time; only the event loop advances it.
using SimTimeUs = std::int64_t;
// The integers behind TradeId and SequenceId.
using TradeIdValue = std::int64_t;
using SequenceValue = std::uint64_t;

// Why a trade was refused or aborted. Carried by VOTE(no) and ABORT messages
// and shown in the trace; `none` means there is no failure.
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

// One node's state for one trade. The coordinator uses collecting, committing,
// aborting and done; participants use preparing, prepared, committed and
// aborted. `none` means the node has no record of the trade.
enum class TradeState : std::uint8_t {
    none,
    collecting,
    committing,
    aborting,
    done,
    preparing,
    prepared,
    committed,
    aborted,
};

// Wire/trace spelling of reason codes (DESIGN.md section 5.1).
[[nodiscard]] constexpr std::string_view to_string(ReasonCode reason) noexcept
{
    switch (reason) {
    case ReasonCode::none:
        return "NONE";
    case ReasonCode::insufficient_funds:
        return "INSUFFICIENT_FUNDS";
    case ReasonCode::insufficient_bonds:
        return "INSUFFICIENT_BONDS";
    case ReasonCode::account_frozen:
        return "ACCOUNT_FROZEN";
    case ReasonCode::expired:
        return "EXPIRED";
    case ReasonCode::vote_timeout:
        return "VOTE_TIMEOUT";
    case ReasonCode::already_aborted:
        return "ALREADY_ABORTED";
    case ReasonCode::unknown_trade:
        return "UNKNOWN_TRADE";
    }
    return "UNKNOWN";
}

// Returns the trace spelling of `state`, e.g. "PREPARED".
[[nodiscard]] constexpr std::string_view to_string(TradeState state) noexcept
{
    switch (state) {
    case TradeState::none:
        return "NONE";
    case TradeState::collecting:
        return "COLLECTING";
    case TradeState::committing:
        return "COMMITTING";
    case TradeState::aborting:
        return "ABORTING";
    case TradeState::done:
        return "DONE";
    case TradeState::preparing:
        return "PREPARING";
    case TradeState::prepared:
        return "PREPARED";
    case TradeState::committed:
        return "COMMITTED";
    case TradeState::aborted:
        return "ABORTED";
    }
    return "UNKNOWN";
}

// The four nodes of the simulation: the coordinator and three participant
// banks.
enum class NodeId : std::uint8_t {
    coordinator,
    bank_a,
    bank_b,
    bank_c,
};

// Returns the trace name of `node`: "COORD", "BANK_A", "BANK_B" or "BANK_C".
constexpr std::string_view to_string(NodeId node) noexcept
{
    switch (node) {
    case NodeId::coordinator:
        return "COORD";
    case NodeId::bank_a:
        return "BANK_A";
    case NodeId::bank_b:
        return "BANK_B";
    case NodeId::bank_c:
        return "BANK_C";
    }
    return "UNKNOWN";
}

// Converts a trace/display node name back into an enum value.
// Event, log, and trace code call this at configuration or parsing boundaries.
constexpr std::optional<NodeId> node_id_from_string(std::string_view name) noexcept
{
    if (name == "COORD") {
        return NodeId::coordinator;
    }
    if (name == "BANK_A") {
        return NodeId::bank_a;
    }
    if (name == "BANK_B") {
        return NodeId::bank_b;
    }
    if (name == "BANK_C") {
        return NodeId::bank_c;
    }
    return std::nullopt;
}

namespace detail {

// Strongly typed integer. Each `Tag` makes a distinct type, so a TradeId cannot
// be passed where Cents is expected. The value is fixed at construction;
// values compare by their integers.
template <typename Tag, typename Underlying>
class ImmutableValue {
  public:
    // Wraps `value` (converted to `Underlying`).
    template <std::integral Input>
    constexpr explicit ImmutableValue(Input value) noexcept
        : value_(static_cast<Underlying>(value))
    {
    }

    // Returns the wrapped integer.
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

// Tags that make each ImmutableValue below a distinct type.
struct TradeIdTag {};
struct SequenceTag {};
struct CentsTag {};
struct UnitsTag {};

// Identifier of one trade, assigned by the coordinator (1, 2, ...).
class TradeId : public detail::ImmutableValue<TradeIdTag, TradeIdValue> {
  public:
    using detail::ImmutableValue<TradeIdTag, TradeIdValue>::ImmutableValue;
};

// Monotonic number of a scheduled event; orders events at the same time.
class SequenceId : public detail::ImmutableValue<SequenceTag, SequenceValue> {
  public:
    using detail::ImmutableValue<SequenceTag, SequenceValue>::ImmutableValue;
};

// An amount of money in cents: Cents{2487500} is $24,875.00.
class Cents : public detail::ImmutableValue<CentsTag, std::int64_t> {
  public:
    using detail::ImmutableValue<CentsTag, std::int64_t>::ImmutableValue;
};

// A quantity of bonds.
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

// Returns lhs + rhs, or std::nullopt if the result would overflow int64_t.
[[nodiscard]] constexpr std::optional<std::int64_t>
checked_add_i64(std::int64_t lhs, std::int64_t rhs) noexcept
{
    // Called by ledger and virtual-time calculations before state is mutated.
    if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
        return std::nullopt;
    }
    if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

// Returns lhs - rhs, or std::nullopt if the result would overflow int64_t.
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

// Returns lhs * rhs, or std::nullopt if the result would overflow int64_t.
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

// Returns `value` converted to `To`, or std::nullopt if it does not fit.
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

// Either a value or the ReasonCode explaining why there is none.
template <typename T>
struct Outcome {
    std::optional<T> value{};
    ReasonCode reason = ReasonCode::none;

    // Returns true if there is no failure reason.
    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return reason == ReasonCode::none;
    }

    // Returns a successful Outcome holding `success_value`.
    [[nodiscard]] static constexpr Outcome success(T success_value)
    {
        return Outcome{std::optional<T>{success_value}, ReasonCode::none};
    }

    // Returns a failed Outcome carrying `failure_reason`.
    [[nodiscard]] static constexpr Outcome failure(ReasonCode failure_reason)
    {
        return Outcome{std::nullopt, failure_reason};
    }
};

} // namespace glassbox

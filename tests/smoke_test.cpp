#include "sim/core_types.hpp"
#include "check.hpp"

#include <limits>
#include <type_traits>

// Compile-time and runtime checks for immutable core values and checked integer
// boundaries. CMake registers this executable with CTest as smoke_test.
int main()
{
    static_assert(!std::is_constructible_v<glassbox::Cents, double>);
    static_assert(!std::is_constructible_v<glassbox::Units, float>);

    static_assert(glassbox::checked_add_i64(10, 20).value_or(-1) == 30);
    static_assert(!glassbox::checked_add_i64(
        std::numeric_limits<std::int64_t>::max(), 1));
    static_assert(!glassbox::checked_sub_i64(
        std::numeric_limits<std::int64_t>::min(), 1));
    static_assert(!glassbox::checked_multiply_i64(
        std::numeric_limits<std::int64_t>::max(), 2));

    static_assert(glassbox::checked_narrow_cast<std::uint8_t>(255).value_or(0) == 255);
    static_assert(!glassbox::checked_narrow_cast<std::uint8_t>(256));
    static_assert(!glassbox::checked_narrow_cast<std::uint8_t>(-1));

    constexpr glassbox::TradeId trade_id{42};
    constexpr glassbox::SequenceId seq{7};
    constexpr glassbox::Cents cents{100000};
    constexpr glassbox::Units units{25000};

    static_assert(trade_id.value() == 42);
    static_assert(seq.value() == 7);
    static_assert(cents.value() == 100000);
    static_assert(units.value() == 25000);

    static_assert(glassbox::node_id_from_string("COORD") == glassbox::NodeId::coordinator);
    static_assert(glassbox::node_id_from_string("BANK_A") == glassbox::NodeId::bank_a);
    static_assert(glassbox::node_id_from_string("BANK_B") == glassbox::NodeId::bank_b);
    static_assert(glassbox::node_id_from_string("BANK_C") == glassbox::NodeId::bank_c);
    static_assert(!glassbox::node_id_from_string("UNKNOWN"));

    static_assert(glassbox::to_string(glassbox::NodeId::coordinator) == "COORD");
    static_assert(glassbox::to_string(glassbox::NodeId::bank_a) == "BANK_A");

    constexpr auto success =
        glassbox::Outcome<glassbox::TradeId>::success(glassbox::TradeId{42});
    constexpr auto failure = glassbox::Outcome<glassbox::TradeId>::failure(
        glassbox::ReasonCode::insufficient_funds);
    static_assert(success.ok());
    static_assert(success.value.has_value());
    static_assert(success.value.value().value() == 42);
    static_assert(!failure.ok());
    static_assert(!failure.value.has_value());
    static_assert(failure.reason == glassbox::ReasonCode::insufficient_funds);

    CHECK(glassbox::checked_narrow_cast<std::int32_t>(std::int64_t{42}) == std::int32_t{42});

    return 0;
}

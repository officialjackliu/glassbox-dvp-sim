#include "sim/timers.hpp"
#include "check.hpp"

#include <cstdint>
#include <limits>

int main()
{
    glassbox::BackoffPolicy policy{1'000, 8'000};
    CHECK(glassbox::validate(policy));

    CHECK(glassbox::backoff_delay_us(policy, 0).value_or(-1) == 1'000);
    CHECK(glassbox::backoff_delay_us(policy, 1).value_or(-1) == 2'000);
    CHECK(glassbox::backoff_delay_us(policy, 2).value_or(-1) == 4'000);
    CHECK(glassbox::backoff_delay_us(policy, 3).value_or(-1) == 8'000);
    CHECK(glassbox::backoff_delay_us(policy, 200).value_or(-1) == 8'000);
    CHECK(glassbox::backoff_delay_us(policy,
                                     std::numeric_limits<std::uint32_t>::max())
              .value_or(-1) == 8'000);

    glassbox::BackoffPolicy exact{5'000, 5'000};
    CHECK(glassbox::validate(exact));
    CHECK(glassbox::backoff_delay_us(exact, 0).value_or(-1) == 5'000);
    CHECK(glassbox::backoff_delay_us(exact,
                                     std::numeric_limits<std::uint32_t>::max())
              .value_or(-1) == 5'000);

    CHECK(!glassbox::validate(glassbox::BackoffPolicy{0, 8'000}));
    CHECK(!glassbox::validate(glassbox::BackoffPolicy{9'000, 8'000}));
    CHECK(!glassbox::backoff_delay_us(
        glassbox::BackoffPolicy{0, 8'000}, 1));

    for (std::uint32_t attempt = 0; attempt <= 200; ++attempt) {
        const auto delay =
            glassbox::backoff_delay_us(policy, attempt);
        CHECK(delay.has_value());
        CHECK(*delay >= 1'000);
        CHECK(*delay <= 8'000);
    }
    return 0;
}

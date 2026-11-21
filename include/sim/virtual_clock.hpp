#pragma once

#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace glassbox {

class VirtualClock {
  public:
    [[nodiscard]] constexpr SimTimeUs now_us() const noexcept
    {
        return now_us_;
    }

    [[nodiscard]] std::optional<Event> pop_next(EventQueue& queue) noexcept
    {
        auto event = queue.pop();
        if (!event) {
            return std::nullopt;
        }

        if (event->time_us >= now_us_) {
            now_us_ = event->time_us;
        }
        return event;
    }

  private:
    SimTimeUs now_us_ = 0;
};

using MeasuredNs = std::uint64_t;

template <typename Handler>
[[nodiscard]] MeasuredNs measure_handler(Handler&& handler, MeasuredNs& measured_ns)
{
    const auto start = std::chrono::steady_clock::now();
    std::invoke(std::forward<Handler>(handler));
    const auto finish = std::chrono::steady_clock::now();
    measured_ns = static_cast<MeasuredNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    return measured_ns;
}

struct SyntheticTraceEvent {
    SimTimeUs sim_time_us = 0;
    std::uint64_t measured_ns = 0;
    std::string kind;
};

} // namespace glassbox

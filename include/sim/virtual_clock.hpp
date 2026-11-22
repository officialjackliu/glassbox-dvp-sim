#pragma once

// Virtual simulation time and isolated wall-clock handler measurement.
//
// VirtualClock only advances from popped events. measure_handler separately
// samples std::chrono::steady_clock and must never feed that value back into
// protocol, network, or scheduling decisions.

#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace glassbox {

class VirtualClock {
  public:
    // Read by event-loop and trace code to stamp simulation-time fields.
    [[nodiscard]] constexpr SimTimeUs now_us() const noexcept
    {
        return now_us_;
    }

    [[nodiscard]] std::optional<Event> pop_next(EventQueue& queue)
    {
        const auto earliest = queue.peek();
        if (earliest && earliest->time_us < now_us_) {
            throw std::invalid_argument(
                "event timestamp is earlier than the virtual clock");
        }
        // Called once per discrete-event loop iteration. The queue, rather than
        // wall time, determines the next event and the value returned here.
        auto event = queue.pop();
        if (!event) {
            return std::nullopt;
        }

        now_us_ = event->time_us;
        return event;
    }

  private:
    SimTimeUs now_us_ = 0;
};

using MeasuredNs = std::uint64_t;

template <typename Handler>
[[nodiscard]] MeasuredNs measure_handler(Handler&& handler)
{
    const auto start = std::chrono::steady_clock::now();
    std::invoke(std::forward<Handler>(handler));
    const auto finish = std::chrono::steady_clock::now();
    return static_cast<MeasuredNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

struct SyntheticTraceEvent {
    SimTimeUs sim_time_us = 0;
    std::uint64_t measured_ns = 0;
    std::string kind;
};

// Writes the small trace format used by the integration smoke executable.
// Future JSONL writer code should retain sim_time_us and measured_ns as
// separate fields and never sum or compare them as one time quantity.
inline void write_synthetic_trace(std::ostream& output,
                                  const SyntheticTraceEvent& event)
{
    output << "{\"sim_time_us\":" << event.sim_time_us
           << ",\"measured_ns\":" << event.measured_ns
           << ",\"kind\":\"" << event.kind << "\"}\n";
}

} // namespace glassbox

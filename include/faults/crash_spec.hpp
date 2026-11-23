#pragma once

// Declarative crash points (DESIGN.md section 7.1).
//
// A CrashSpec fires once, on the `occurrence`-th event that matches it: a
// message of `trigger` type delivered to `node` (or a timer of `trigger`
// kind firing on `node`), optionally for one trade. The crash point is
// relative to the Effects batch that handler returns:
//   BEFORE_LOG             the node crashes while handling: nothing from the
//                          batch is logged or sent.
//   AFTER_LOG_BEFORE_SEND  the batch's records become durable, then the node
//                          crashes before any message of the batch leaves.
//   AFTER_FIRST_SEND       records durable, the first message leaves, then
//                          the node crashes; the rest of the batch is lost.
// restart_after_us schedules a restart that long after the crash (nullopt:
// the node stays down).

#include "sim/core_types.hpp"
#include "sim/event_queue.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace glassbox {

enum class CrashPoint : std::uint8_t {
    before_log,
    after_log_before_send,
    after_first_send,
};

[[nodiscard]] constexpr std::string_view to_string(CrashPoint point) noexcept
{
    switch (point) {
    case CrashPoint::before_log:
        return "BEFORE_LOG";
    case CrashPoint::after_log_before_send:
        return "AFTER_LOG_BEFORE_SEND";
    case CrashPoint::after_first_send:
        return "AFTER_FIRST_SEND";
    }
    return "UNKNOWN";
}

using CrashTrigger = std::variant<MessageType, TimerKind>;

struct CrashSpec {
    NodeId node;
    std::optional<TradeId> trade_id;
    CrashTrigger trigger;
    CrashPoint point;
    std::optional<SimTimeUs> restart_after_us;
    std::uint32_t occurrence = 1;
};

} // namespace glassbox

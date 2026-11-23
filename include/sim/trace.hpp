#pragma once

#include "sim/core_types.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <string>
#include <optional>

namespace glassbox {

struct TraceRecord {
    std::uint64_t seq = 0;
    SimTimeUs sim_time_us = 0;
    std::string kind;
    std::string handler;
    std::uint64_t measured_ns = 0;
    std::optional<std::string> reason;

    // Glass-box context (DESIGN.md section 9). Unset when not applicable.
    std::optional<NodeId> node;
    std::optional<TradeId> trade_id;
    std::optional<MessageType> msg_type;
    std::optional<std::uint64_t> msg_id;
    std::optional<NodeId> from;
    std::optional<NodeId> to;
    std::optional<std::string> log_record;
    std::optional<std::string> detail;
};

class TraceSink {
  public:
    virtual ~TraceSink() = default;
    virtual void write(const TraceRecord& record) = 0;
};

class NullTraceSink : public TraceSink {
  public:
    void write(const TraceRecord&) override {}
};

} // namespace glassbox

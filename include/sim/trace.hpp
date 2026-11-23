#pragma once

#include "sim/core_types.hpp"

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

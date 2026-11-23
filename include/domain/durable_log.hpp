#pragma once

#include "domain/log_records.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace glassbox {

struct LogEntry {
    std::uint64_t lsn;
    SimTimeUs durable_at_us;
    LogRecord record;

    [[nodiscard]] friend bool operator==(
        const LogEntry&,
        const LogEntry&) noexcept = default;
};

class DurableLog {
  public:
    [[nodiscard]] std::optional<LogEntry> append(
        LogRecord record,
        SimTimeUs durable_at_us) noexcept;

    [[nodiscard]] std::span<const LogEntry> entries() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<LogEntry> snapshot() const;

  private:
    std::vector<LogEntry> entries_;
    std::uint64_t next_lsn_ = 1;
    bool exhausted_ = false;
};

} // namespace glassbox

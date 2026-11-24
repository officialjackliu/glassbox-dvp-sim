#pragma once

// A node's simulated disk: an append-only list of log records, each numbered
// with a log sequence number (LSN) and stamped with the simulated time it
// became durable. Records are never changed or removed; the simulator keeps one
// DurableLog per node and it survives that node's crashes.

#include "domain/log_records.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace glassbox {

// One durable record with its position in the log and when it became durable.
struct LogEntry {
    std::uint64_t lsn;
    SimTimeUs durable_at_us;
    LogRecord record;

    [[nodiscard]] friend bool operator==(
        const LogEntry&,
        const LogEntry&) noexcept = default;
};

// Append-only log owned by the simulator, one per node.
//
//   DurableLog log;
//   auto entry = log.append(Prepared{trade, Leg::buyer}, now);  // lsn 1
//   for (const LogEntry& e : log.entries()) { ... }
class DurableLog {
  public:
    // Appends `record` with the next LSN (starting at 1) and returns the new
    // entry, or std::nullopt once LSNs are used up (nothing is appended).
    [[nodiscard]] std::optional<LogEntry> append(
        LogRecord record,
        SimTimeUs durable_at_us) noexcept;

    // Returns the entries in LSN order. The span is invalidated by append().
    [[nodiscard]] std::span<const LogEntry> entries() const noexcept;
    // Returns the number of entries.
    [[nodiscard]] std::size_t size() const noexcept;
    // Returns a copy of all entries.
    [[nodiscard]] std::vector<LogEntry> snapshot() const;

  private:
    std::vector<LogEntry> entries_;
    std::uint64_t next_lsn_ = 1;
    bool exhausted_ = false;
};

} // namespace glassbox

#include "domain/durable_log.hpp"

#include <limits>

namespace glassbox {

std::optional<LogEntry> DurableLog::append(
    LogRecord record,
    SimTimeUs durable_at_us) noexcept
{
    if (exhausted_) {
        return std::nullopt;
    }
    const std::uint64_t lsn = next_lsn_;
    entries_.push_back(
        LogEntry{
            lsn,
            durable_at_us,
            std::move(record),
        });
    if (lsn == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
    } else {
        ++next_lsn_;
    }
    return entries_.back();
}

std::span<const LogEntry> DurableLog::entries() const noexcept
{
    return std::span<const LogEntry>{entries_};
}

std::size_t DurableLog::size() const noexcept
{
    return entries_.size();
}

std::vector<LogEntry> DurableLog::snapshot() const
{
    return entries_;
}

} // namespace glassbox

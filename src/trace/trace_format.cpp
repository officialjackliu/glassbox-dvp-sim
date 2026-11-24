// Renders a TraceRecord as one line for `--scenario` output.

#include "trace/trace_format.hpp"

namespace glassbox {

std::string format_trace_line(const TraceRecord& record)
{
    std::string line = std::to_string(record.seq);
    line += " t=";
    line += std::to_string(record.sim_time_us);
    line += " ";
    line += record.kind;
    if (record.node) {
        line += " ";
        line += to_string(*record.node);
    }
    if (record.trade_id) {
        line += " trade=";
        line += std::to_string(record.trade_id->value());
    }
    if (record.msg_type) {
        line += " msg=";
        line += to_string(*record.msg_type);
        if (record.msg_id) {
            line += "#";
            line += std::to_string(*record.msg_id);
        }
    }
    if (record.from && record.to) {
        line += " ";
        line += to_string(*record.from);
        line += "->";
        line += to_string(*record.to);
    }
    if (record.log_record) {
        line += " log=";
        line += *record.log_record;
    }
    if (record.reason) {
        line += " reason=";
        line += *record.reason;
    }
    if (record.detail) {
        line += " detail=";
        line += *record.detail;
    }
    line += " @";
    line += record.handler;
    return line;
}

} // namespace glassbox

#pragma once

// One-line text rendering of a trace record for the CLI.
// measured_ns is deliberately not printed: simulated time and measured
// compute time are never mixed in a display (DESIGN.md section 9).

#include "sim/trace.hpp"

#include <string>

namespace glassbox {

// Format: seq t=<us> KIND node trade=<id> msg=<TYPE>#<id> <FROM>-><TO>
//         log=<RECORD> reason=<R> detail=<D> @<handler>
// Fields that are unset are omitted.
[[nodiscard]] std::string format_trace_line(const TraceRecord& record);

} // namespace glassbox

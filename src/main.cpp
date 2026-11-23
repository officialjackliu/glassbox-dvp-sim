// glassbox-dvp-sim command-line runner (minimal, Task 15).
//
//   glassbox-dvp-sim --list
//   glassbox-dvp-sim --scenario S1 [--seed 1]
//
// Prints one line per trace record, then the final state of every node.
// Exit codes: 0 quiesced, 2 usage error, 3 event bound hit (not quiesced).

#include "scenarios/scenario.hpp"
#include "trace/trace_format.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

class StdoutSink : public glassbox::TraceSink {
  public:
    void write(const glassbox::TraceRecord& record) override
    {
        std::cout << glassbox::format_trace_line(record) << '\n';
    }
};

int usage()
{
    std::cerr << "usage: glassbox-dvp-sim --list\n"
                 "       glassbox-dvp-sim --scenario <ID> [--seed <N>]\n";
    return 2;
}

[[nodiscard]] std::optional<std::uint64_t> parse_seed(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

void print_final_state(const glassbox::ScenarioRun& run)
{
    const glassbox::ProtocolNodes& nodes = run.nodes();
    for (const auto& [id, state] : nodes.coordinator->trades()) {
        std::cout << "final COORD trade=" << id.value() << " "
                  << glassbox::to_string(state.state) << '\n';
    }
    for (const glassbox::Participant* bank : nodes.banks()) {
        const glassbox::LocalLedger& ledger = bank->ledger();
        std::cout << "final " << glassbox::to_string(bank->self())
                  << " cash_total=" << ledger.cash_total().value()
                  << " cash_locked=" << ledger.cash_locked().value()
                  << " bonds_total=" << ledger.bonds_total().value()
                  << " bonds_locked=" << ledger.bonds_locked().value();
        for (const auto& [id, trade] : bank->trades()) {
            std::cout << " trade" << id.value() << "="
                      << glassbox::to_string(trade.state);
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::optional<std::string_view> scenario_id;
    std::uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--list") {
            for (const glassbox::ScenarioInfo& info :
                 glassbox::scenario_registry()) {
                std::cout << info.id << "  " << info.name << '\n';
            }
            return 0;
        }
        if (arg == "--scenario" && i + 1 < argc) {
            scenario_id = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            const auto parsed = parse_seed(argv[++i]);
            if (!parsed) {
                return usage();
            }
            seed = *parsed;
        } else {
            return usage();
        }
    }
    if (!scenario_id) {
        return usage();
    }
    const glassbox::ScenarioInfo* info = glassbox::find_scenario(*scenario_id);
    if (!info) {
        std::cerr << "unknown scenario: " << *scenario_id << '\n';
        return 2;
    }

    StdoutSink out;
    glassbox::ScenarioRun run{info->make(seed), &out};
    const bool quiesced = run.run();
    print_final_state(run);
    std::cout << "scenario=" << info->id << " seed=" << seed
              << " quiesced=" << (quiesced ? "yes" : "no") << '\n';
    return quiesced ? 0 : 3;
}

#include "cli/dials.hpp"

#include "sim/deterministic_rng.hpp"
#include "sim/network_model.hpp"

#include <charconv>
#include <optional>
#include <variant>

namespace glassbox::cli {

namespace {

constexpr SimTimeUs k_max_duration_us = 3'600'000'000; // one hour

[[nodiscard]] std::optional<std::int64_t> parse_int(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || // also rejects empty text
        end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

struct Parsed {
    DialStatus status = DialStatus::ok;
    std::int64_t value = 0;
};

// "<integer><us|ms|s>", 1 us to one hour.
[[nodiscard]] Parsed parse_duration(std::string_view text)
{
    std::int64_t scale = 0;
    std::string_view digits;
    if (text.ends_with("us")) {
        scale = 1;
        digits = text.substr(0, text.size() - 2);
    } else if (text.ends_with("ms")) {
        scale = 1'000;
        digits = text.substr(0, text.size() - 2);
    } else if (text.ends_with("s")) {
        scale = 1'000'000;
        digits = text.substr(0, text.size() - 1);
    } else {
        return {DialStatus::bad_value, 0};
    }
    const auto number = parse_int(digits);
    if (!number) {
        return {DialStatus::bad_value, 0};
    }
    const auto us = checked_multiply_i64(*number, scale);
    if (!us || *us < 1 || *us > k_max_duration_us) {
        return {DialStatus::out_of_range, 0};
    }
    return {DialStatus::ok, *us};
}

// "<integer>%", 0 to 100, as parts per million.
[[nodiscard]] Parsed parse_percent(std::string_view text)
{
    if (!text.ends_with("%")) {
        return {DialStatus::bad_value, 0};
    }
    const auto number = parse_int(text.substr(0, text.size() - 1));
    if (!number) {
        return {DialStatus::bad_value, 0};
    }
    if (*number < 0 || *number > 100) {
        return {DialStatus::out_of_range, 0};
    }
    return {DialStatus::ok, *number * 10'000};
}

// "COORD-BANK_B": two different nodes.
[[nodiscard]] std::optional<std::pair<NodeId, NodeId>> parse_link(
    std::string_view text)
{
    const std::size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto a = node_id_from_string(text.substr(0, dash));
    const auto b = node_id_from_string(text.substr(dash + 1));
    if (!a || !b || *a == *b) {
        return std::nullopt;
    }
    return std::pair{*a, *b};
}

// The link as it stands once the scenario's own t = 0 actions have run.
[[nodiscard]] LinkConfig link_at_start(const ScenarioSetup& setup,
                                       NodeId from,
                                       NodeId to)
{
    DeterministicRng rng{setup.config.seed};
    const NetworkModel defaults{rng};
    LinkConfig link = defaults.link(from, to);
    for (const TimedAction& action : setup.script) {
        if (action.at != 0) {
            continue;
        }
        if (const auto* set = std::get_if<ScriptSetLink>(&action.action.action);
            set && set->from == from && set->to == to) {
            link = LinkConfig{set->base_latency_us, set->jitter_us,
                              set->drop_probability_ppm};
        }
    }
    return link;
}

[[nodiscard]] TimedAction set_link(NodeId from, NodeId to, const LinkConfig& l)
{
    return TimedAction{
        0, ScriptActionEvent{ScriptSetLink{from, to, l.base_latency_us,
                                           l.jitter_us,
                                           l.drop_probability_ppm}}};
}

} // namespace

std::string_view to_string(DialStatus status) noexcept
{
    switch (status) {
    case DialStatus::ok:
        return "ok";
    case DialStatus::unknown_key:
        return "unknown setting (or nothing to apply it to in this scenario)";
    case DialStatus::bad_value:
        return "bad value";
    case DialStatus::out_of_range:
        return "value out of range";
    }
    return "?";
}

DialStatus apply_dial(ScenarioSetup& setup, std::string_view assignment)
{
    const std::size_t eq = assignment.find('=');
    if (eq == std::string_view::npos) {
        return DialStatus::bad_value;
    }
    const std::string_view key = assignment.substr(0, eq);
    const std::string_view value = assignment.substr(eq + 1);

    if (key == "vote_timeout") {
        const Parsed d = parse_duration(value);
        if (d.status != DialStatus::ok) {
            return d.status;
        }
        setup.config.timers.vote_timeout_us = d.value;
        return DialStatus::ok;
    }
    if (key == "restart_after") {
        if (setup.crashes.empty()) {
            return DialStatus::unknown_key;
        }
        const Parsed d = parse_duration(value);
        if (d.status != DialStatus::ok) {
            return d.status;
        }
        for (CrashSpec& crash : setup.crashes) {
            crash.restart_after_us = d.value;
        }
        return DialStatus::ok;
    }
    const bool latency = key.starts_with("latency.");
    const bool drop = key.starts_with("drop.");
    if (!latency && !drop) {
        return DialStatus::unknown_key;
    }
    const auto link = parse_link(key.substr(latency ? 8 : 5));
    if (!link) {
        return DialStatus::bad_value;
    }
    const Parsed v = latency ? parse_duration(value) : parse_percent(value);
    if (v.status != DialStatus::ok) {
        return v.status;
    }
    const auto [a, b] = *link;
    LinkConfig forward = link_at_start(setup, a, b);
    LinkConfig back = link_at_start(setup, b, a);
    if (latency) {
        forward.base_latency_us = v.value;
        back.base_latency_us = v.value;
    } else {
        forward.drop_probability_ppm = static_cast<std::uint32_t>(v.value);
        back.drop_probability_ppm = static_cast<std::uint32_t>(v.value);
    }
    setup.script.push_back(set_link(a, b, forward));
    setup.script.push_back(set_link(b, a, back));
    return DialStatus::ok;
}

} // namespace glassbox::cli

// Glass-box player: every scenario has an intro card that matches what the
// scenario really does, and the card fits the terminal.

#include "cli/guide.hpp"
#include "cli/screen.hpp"
#include "scenarios/scenario_library.hpp"
#include "check.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using glassbox::cli::find_guide;
using glassbox::cli::ScenarioGuide;
using Lines = std::vector<std::string>;

[[nodiscard]] bool mentions(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

void test_every_scenario_has_a_guide()
{
    std::set<std::string_view> titles;
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        const ScenarioGuide* g = find_guide(info.id);
        CHECK(g != nullptr);
        CHECK(g->id == info.id);
        CHECK(!g->title.empty());
        CHECK(g->what_it_shows.size() > 60);
        CHECK(!g->watch_for.empty());
        CHECK(titles.insert(g->title).second);
    }
    CHECK(titles.size() == glassbox::scenario_registry().size());
    CHECK(find_guide("S0") == nullptr);
    CHECK(find_guide("S15") == nullptr);
    CHECK(find_guide("") == nullptr);
    CHECK(find_guide("s1") == nullptr);
    for (const std::string_view id : glassbox::cli::k_tour) {
        CHECK(glassbox::find_scenario(id) != nullptr);
        CHECK(find_guide(id) != nullptr);
    }
}

void test_guides_match_the_scenarios()
{
    // The numbers the cards quote are the scenarios' own constants.
    CHECK(glassbox::k_s4_slow_link_us == 300'000);
    CHECK(mentions(find_guide("S4")->what_it_shows, "300 ms"));
    CHECK(mentions(find_guide("S4")->what_it_shows, "500 ms vote timer"));
    CHECK(glassbox::make_s4_slow_link_tight_timeout(1).config.timers.vote_timeout_us ==
          500'000);
    CHECK(glassbox::k_s5_node_up_us == 3'000'000);
    CHECK(mentions(find_guide("S5")->what_it_shows, "back at 3 s"));
    CHECK(glassbox::k_s6_restart_after_us == 5'000'000);
    CHECK(mentions(find_guide("S6")->what_it_shows, "down for 5 s"));
    CHECK(glassbox::k_s9_restart_after_us == 5'000'000);
    CHECK(mentions(find_guide("S9")->what_it_shows, "For 5 s"));
    CHECK(glassbox::k_s14_heal_at_us == 2'000'000);
    CHECK(mentions(find_guide("S14")->what_it_shows, "restored at 2 s"));
    // S13: BANK_B has bonds for one standard trade, not two.
    CHECK(glassbox::k_s13_seller_bonds >= glassbox::k_standard_qty);
    CHECK(glassbox::k_s13_seller_bonds < 2 * glassbox::k_standard_qty);
    CHECK(mentions(find_guide("S13")->what_it_shows, "only one"));
    // S2: one cent short.
    CHECK(glassbox::make_s2_buyer_lacks_funds(1).genesis.bank_a.cash_total()
              .value() == glassbox::k_standard_price_cents - 1);
    CHECK(mentions(find_guide("S2")->what_it_shows, "one cent less"));
    // S3: it is the seller that is frozen.
    CHECK(glassbox::make_s3_account_frozen(1).genesis.bank_b.frozen());
    CHECK(mentions(find_guide("S3")->what_it_shows, "BANK_B's account is frozen"));
}

void test_intro_card_exact()
{
    const auto rec = glassbox::cli::record(glassbox::make_s3_account_frozen(1));
    Lines got;
    for (std::string line : glassbox::cli::draw_intro(
             rec, *find_guide("S3"), glassbox::cli::ScreenOptions{100, false})) {
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        got.push_back(line);
    }
    const Lines expected{
        "  S3 · Seller's account is frozen",
        "",
        "  What happens",
        "    BANK_B's account is frozen, so it refuses the trade and votes NO. "
        "BANK_A has already locked",
        "    its cash; it gets ABORT and releases it.",
        "",
        "  The trade",
        "    #1  BANK_A buys 25 UST 10Y bonds from BANK_B for $24,875.00",
        "",
        "  The nodes",
        "    COORD   New York   coordinator: collects votes, decides, tells "
        "both banks",
        "    BANK_A  New York   cash $1,000,000.00 · bonds 25,000",
        "    BANK_B  London     cash $1,000,000.00 · bonds 25,000 · account "
        "FROZEN",
        "",
        "  Watch for",
        "    A refusal by one side undoes the other side's lock.",
        "",
        "  How it works",
        "    Two-phase commit: each bank checks and locks its side, then votes. "
        "Only if both vote YES does",
        "    the coordinator write COMMIT to disk and tell them to settle; "
        "otherwise everyone releases.",
        "    Either both legs move or neither does.",
    };
    if (got != expected) {
        for (const std::string& l : got) {
            std::fprintf(stderr, "%s|\n", l.c_str());
        }
    }
    CHECK(got == expected);

    // Several trades: plural heading, one line each.
    const auto s13 = glassbox::cli::record(glassbox::make_s13_lock_conflict(1));
    const Lines cards = glassbox::cli::draw_intro(
        s13, *find_guide("S13"), glassbox::cli::ScreenOptions{100, false});
    bool plural = false;
    int tickets = 0;
    for (const std::string& l : cards) {
        plural = plural || l.starts_with("  The trades ");
        tickets += l.starts_with("    #") ? 1 : 0;
    }
    CHECK(plural);
    CHECK(tickets == 2);
}

void test_cards_fit()
{
    for (const glassbox::ScenarioInfo& info : glassbox::scenario_registry()) {
        const auto rec = glassbox::cli::record(info.make(1));
        for (const int width : {80, 100, 200}) {
            const Lines mono = glassbox::cli::draw_intro(
                rec, *find_guide(info.id), {width, false});
            const Lines color = glassbox::cli::draw_intro(
                rec, *find_guide(info.id), {width, true});
            CHECK(mono.size() == color.size());
            for (const std::string& l : mono) {
                CHECK(glassbox::cli::visible_width(l) ==
                      static_cast<std::size_t>(width));
            }
            for (const std::string& l : color) {
                CHECK(glassbox::cli::visible_width(l) ==
                      static_cast<std::size_t>(width));
            }
        }
    }
}

} // namespace

int main()
{
    test_every_scenario_has_a_guide();
    test_guides_match_the_scenarios();
    test_intro_card_exact();
    test_cards_fit();
    return 0;
}

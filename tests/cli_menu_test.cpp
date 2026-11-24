// The command-line front end: --help documents every flag and setting, the
// how-to-play menu understands its commands and returns after each one, and
// bad usage is rejected. Runs the real binary (argv[1]) with piped input.

#include "check.hpp"

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string g_binary;

struct Result {
    int code = -1;
    std::string out; // stdout and stderr together
};

[[nodiscard]] Result run(const std::string& args, const std::string& input = "")
{
    {
        std::ofstream in{"cli_menu_test.in"};
        in << input;
    }
    const std::string command = "'" + g_binary + "' " + args +
                                " < cli_menu_test.in > cli_menu_test.out 2>&1";
    const int status = std::system(command.c_str());
    Result r;
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::ifstream out{"cli_menu_test.out"};
    std::stringstream text;
    text << out.rdbuf();
    r.out = text.str();
    return r;
}

[[nodiscard]] bool has(const Result& r, const std::string& part)
{
    const bool found = r.out.find(part) != std::string::npos;
    if (!found) {
        std::fprintf(stderr, "missing: %s\n--- output ---\n%s\n", part.c_str(),
                     r.out.c_str());
    }
    return found;
}

[[nodiscard]] int count(const Result& r, const std::string& part)
{
    int n = 0;
    for (std::size_t at = r.out.find(part); at != std::string::npos;
         at = r.out.find(part, at + 1)) {
        ++n;
    }
    return n;
}

void test_help_documents_everything()
{
    const Result help = run("--help");
    CHECK(help.code == 0);
    for (const char* flag :
         {"--menu", "--tour", "--scenario S6 --watch", "--scenario S6 --story",
          "--list", "--help", "--seed N", "--speed S", "--paused", "--no-color",
          "--set KEY=VALUE"}) {
        CHECK(has(help, flag));
    }
    for (const char* setting :
         {"vote_timeout=30ms", "latency.COORD-BANK_B=80ms",
          "drop.COORD-BANK_B=20%", "restart_after=2s",
          "Nodes: COORD, BANK_A, BANK_B, BANK_C"}) {
        CHECK(has(help, setting));
    }
    for (const char* key : {"space      pause / resume", "n, Enter   next event",
                            "+ / -      faster / slower", "r          restart",
                            "q          stop and go back"}) {
        CHECK(has(help, key));
    }
    CHECK(has(help, "make watch S=S6"));
    CHECK(has(help, "EXIT CODES  0 finished · 2 bad usage · 3 the run did not "
                    "finish"));
    CHECK(has(help, "docs/MANUAL.md"));
    CHECK(run("-h").out == help.out);
    // Without a terminal and without a scenario: the manual, not the menu.
    const Result bare = run("");
    CHECK(bare.code == 0);
    CHECK(bare.out == help.out);
}

void test_menu_commands()
{
    const Result r = run("--menu",
                         "speed fast\nseed 7\nset vote_timeout=30ms\n"
                         "set bogus=1\nspeed 3\nseed x\nr 1\n\nzzz\nq\n");
    CHECK(r.code == 0);
    CHECK(has(r, "GLASS-BOX DvP SETTLEMENT · how to play"));
    CHECK(has(r, "     1  Happy path: both legs settle"));
    CHECK(has(r, "    14  Network partition"));
    CHECK(has(r, "  Now: speed normal (100%) · seed 1 · settings: none"));
    CHECK(has(r, "  Type 1 and press Enter to start with the happy path."));
    CHECK(has(r, "  Speed is now fast (200%)."));
    CHECK(has(r, "  Seed is now 7."));
    CHECK(has(r, "  Setting added: vote_timeout=30ms."));
    CHECK(has(r, "  Now: speed fast (200%) · seed 7 · settings: "
                 "vote_timeout=30ms"));
    CHECK(has(r, "  bogus=1: unknown setting (or nothing to apply it to in "
                 "this scenario). Type settings to see what you can set."));
    CHECK(has(r, "  Speed must be slow, normal, fast, or 25 to 800."));
    CHECK(has(r, "  The seed must be a whole number, e.g. seed 7."));
    // r 1 reads S1 with the settings and seed: it aborts at 31.1 ms.
    CHECK(has(r, "S1 Happy path · seed 7 · vote_timeout=30ms"));
    CHECK(has(r, "[     31.100 ms] Vote timer runs out → COORD decides ABORT"));
    CHECK(has(r, "    #1  CANCELLED (vote timeout)"));
    CHECK(has(r, "  Press Enter to go back to the menu."));
    CHECK(has(r, "  Finished S1. Pick another scenario, or q to quit."));
    CHECK(has(r, "  Unknown command: \"zzz\". Type a number from 1 to 14, t "
                 "for the tour, or q."));
    // Piped input is echoed after the prompt.
    CHECK(has(r, "\n  > speed fast\n"));
    // The menu came back after every command: one prompt per command read
    // (nine, the last one answered with q).
    CHECK(count(r, "\n  > ") == 9);
}

void test_scenario_names_and_errors()
{
    // Number, S-name and watch forms all pick the scenario; without a
    // terminal, watching prints the story and waits for Enter.
    for (const char* pick : {"6", "S6", "s6", "watch 6", "w S6"}) {
        const Result r = run("--menu", std::string{pick} + "\n\nq\n");
        CHECK(r.code == 0);
        CHECK(has(r, "not a terminal: printing the story instead"));
        CHECK(has(r, "S6 In-doubt participant · seed 1"));
        CHECK(has(r, "  Finished S6."));
        CHECK(count(r, "  Press Enter to go back to the menu.") == 1);
        CHECK(count(r, "\n  > ") == 2);
    }
    // Spaces around a command or its argument do not matter.
    const Result spaced = run("--menu", "  speed fast   \n\t6 \n\nq\n");
    CHECK(has(spaced, "  Speed is now fast (200%)."));
    CHECK(has(spaced, "  Finished S6."));
    for (const char* bad : {"0", "15", "S0", "s15", "watch", "r", "r x"}) {
        const Result r = run("--menu", std::string{bad} + "\nq\n");
        CHECK(has(r, "  There is no such scenario. Pick a number from 1 to 14."));
    }
    // A setting that does not apply to the scenario: reported, not run.
    const Result crash = run("--menu", "set restart_after=2s\n1\nq\n");
    CHECK(has(crash, "  Setting added: restart_after=2s."));
    CHECK(has(crash, "  Could not start S1: restart_after=2s: unknown setting "
                     "(or nothing to apply it to in this scenario). Type clear "
                     "to remove settings."));
    CHECK(crash.out.find("S1 Happy path · seed 1") == std::string::npos);
    const Result cleared = run("--menu", "set restart_after=2s\nclear\nq\n");
    CHECK(has(cleared, "  Settings cleared."));
    CHECK(count(cleared, "settings: none") == 2);
    // Help pages wait for Enter and return.
    const Result pages = run("--menu", "settings\n\nkeys\n\nhelp\n\n?\n\nq\n");
    CHECK(has(pages, "Settings change the simulated system before a scenario"));
    CHECK(has(pages, "  While a scenario plays:"));
    CHECK(has(pages, "START HERE"));
    CHECK(count(pages, "  Press Enter to go back to the menu.") == 4);
    // The tour needs a terminal.
    const Result tour = run("--menu", "t\nq\n");
    CHECK(has(tour, "  The tour needs a terminal."));
    // Quit words and the end of input both leave with 0.
    for (const char* quit : {"q\n", "Q\n", "quit\n", "exit\n", "  q  \n", ""}) {
        const Result r = run("--menu", quit);
        CHECK(r.code == 0);
        CHECK(count(r, "\n  > ") == 1);
    }
    // Options given on the command line start the menu with them.
    const Result opts = run("--menu --speed slow --seed 3 --set drop.COORD-BANK_A=10%",
                            "q\n");
    CHECK(has(opts, "  Now: speed slow (50%) · seed 3 · settings: "
                    "drop.COORD-BANK_A=10%"));
}

void test_usage_errors()
{
    for (const char* args :
         {"--watch", "--story", "--tour --scenario S1", "--menu --tour",
          "--menu --scenario S1", "--scenario S1 --watch --story",
          "--tour --set vote_timeout=1ms", "--bogus", "--speed 3",
          "--seed x"}) {
        const Result r = run(args);
        CHECK(r.code == 2);
        CHECK(has(r, "usage: glassbox-dvp-sim"));
    }
    const Result unknown = run("--scenario S99");
    CHECK(unknown.code == 2);
    CHECK(has(unknown, "unknown scenario: S99"));
    const Result bad_set = run("--scenario S1 --set bogus=1");
    CHECK(bad_set.code == 2);
    CHECK(has(bad_set, "--set bogus=1: unknown setting"));
    // The plain trace still works and is unchanged in shape.
    const Result trace = run("--scenario S1");
    CHECK(trace.code == 0);
    CHECK(has(trace, "1 t=0 SCRIPT_ACTION @Simulator::dispatch_script\n"));
    CHECK(has(trace, "scenario=S1 seed=1 quiesced=yes\n"));
}

} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    g_binary = argv[1];
    test_help_documents_everything();
    test_menu_commands();
    test_scenario_names_and_errors();
    test_usage_errors();
    std::remove("cli_menu_test.in");
    std::remove("cli_menu_test.out");
    return 0;
}

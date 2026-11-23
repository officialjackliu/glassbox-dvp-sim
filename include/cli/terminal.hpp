#pragma once

// Glass-box player: full-screen drawing and single key presses (POSIX).
//
// When stdin and stdout are terminals, Terminal switches to the alternate
// screen, hides the cursor and reads keys without Enter (Ctrl-C is read as
// quit so the terminal is always restored). Otherwise it does nothing
// special and wait_key() just waits.

#include <termios.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glassbox::cli {

enum class Key : std::uint8_t {
    none,
    pause,   // space, p
    next,    // n, Enter, right arrow
    faster,  // +, =, up arrow
    slower,  // -, _, down arrow
    restart, // r
    quit,    // q, Esc, Ctrl-C
};

// The key for the bytes of one read (an arrow key arrives as three bytes).
[[nodiscard]] Key key_for(std::string_view bytes) noexcept;

class Terminal {
  public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Redraws the whole screen with these lines.
    void show(const std::vector<std::string>& lines);
    // Waits up to timeout_ms for a key; Key::none on timeout.
    [[nodiscard]] Key wait_key(std::int64_t timeout_ms);
    // Terminal width in columns (100 if unknown).
    [[nodiscard]] int columns() const;

    // stdin and stdout are both terminals.
    [[nodiscard]] static bool interactive();

  private:
    bool active_ = false;
    termios saved_{};
    std::string pending_; // bytes read but not yet returned as keys
};

// Splits off the first key of `bytes` (three bytes for an arrow key, else
// one) and returns its length; 0 for empty input.
[[nodiscard]] std::size_t first_key_length(std::string_view bytes) noexcept;

} // namespace glassbox::cli

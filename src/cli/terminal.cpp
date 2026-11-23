#include "cli/terminal.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace glassbox::cli {

Key key_for(std::string_view bytes) noexcept
{
    if (bytes.empty()) {
        return Key::none;
    }
    if (bytes == "\x1b[C") {
        return Key::next;
    }
    if (bytes == "\x1b[A") {
        return Key::faster;
    }
    if (bytes == "\x1b[B") {
        return Key::slower;
    }
    switch (bytes.front()) {
    case ' ':
    case 'p':
    case 'P':
        return Key::pause;
    case 'n':
    case 'N':
    case '\r':
    case '\n':
        return Key::next;
    case '+':
    case '=':
        return Key::faster;
    case '-':
    case '_':
        return Key::slower;
    case 'r':
    case 'R':
        return Key::restart;
    case 'q':
    case 'Q':
    case '\x03': // Ctrl-C (signals are off in raw mode)
        return Key::quit;
    case '\x1b':
        return bytes.size() == 1 ? Key::quit : Key::none;
    default:
        return Key::none;
    }
}

bool Terminal::interactive()
{
    return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;
}

Terminal::Terminal()
{
    if (!interactive() || tcgetattr(STDIN_FILENO, &saved_) != 0) {
        return;
    }
    termios raw = saved_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return;
    }
    active_ = true;
    // Alternate screen, hide cursor.
    std::fputs("\x1b[?1049h\x1b[?25l", stdout);
    std::fflush(stdout);
}

Terminal::~Terminal()
{
    if (!active_) {
        return;
    }
    std::fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    std::fflush(stdout);
    static_cast<void>(tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_));
}

void Terminal::show(const std::vector<std::string>& lines)
{
    std::string frame = active_ ? "\x1b[H" : "";
    for (const std::string& line : lines) {
        frame += line;
        frame += active_ ? "\x1b[K\r\n" : "\n";
    }
    if (active_) {
        frame += "\x1b[J";
    }
    std::fwrite(frame.data(), 1, frame.size(), stdout);
    std::fflush(stdout);
}

std::size_t first_key_length(std::string_view bytes) noexcept
{
    if (bytes.empty()) {
        return 0;
    }
    if (bytes.size() >= 3 && bytes[0] == '\x1b' && bytes[1] == '[') {
        return 3;
    }
    return 1;
}

Key Terminal::wait_key(std::int64_t timeout_ms)
{
    const int timeout = static_cast<int>(
        std::clamp<std::int64_t>(timeout_ms, 0, 60'000));
    if (!active_) {
        static_cast<void>(poll(nullptr, 0, timeout));
        return Key::none;
    }
    if (pending_.empty()) {
        pollfd in{STDIN_FILENO, POLLIN, 0};
        if (poll(&in, 1, timeout) <= 0) {
            return Key::none;
        }
        char buffer[64];
        const ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0) {
            return Key::none;
        }
        pending_.append(buffer, static_cast<std::size_t>(n));
    }
    // One key per call; the rest waits for the next call.
    const std::size_t len = first_key_length(pending_);
    const Key key = key_for(std::string_view{pending_}.substr(0, len));
    pending_.erase(0, len);
    return key;
}

int Terminal::columns() const
{
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
    return 100;
}

} // namespace glassbox::cli

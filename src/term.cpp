#include "term.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define GN_ISATTY _isatty
#define GN_FILENO _fileno
#else
#include <termios.h>
#include <unistd.h>
#define GN_ISATTY isatty
#define GN_FILENO fileno
#endif

namespace term {
namespace {

Options g_opts;
std::string g_noninteractive_reason;

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string_view s(v);
    return s != "0" && s != "false" && s != "no";
}

constexpr Glyphs kUnicode{"✔", "✘", "⚠", "ℹ", "→", "•", "─", "│", "╭", "╰", "…"};
constexpr Glyphs kAscii{"[ok]", "[x]", "[!]", "[i]", "->", "*", "-", "|", "+", "+", "..."};

std::string sgr(const char* code, std::string_view s) {
    if (!g_opts.color) return std::string(s);
    std::string out = "\x1b[";
    out += code;
    out += 'm';
    out += s;
    out += "\x1b[0m";
    return out;
}

#ifdef _WIN32
bool enable_vt(DWORD handle_id) {
    HANDLE h = GetStdHandle(handle_id);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#endif

}  // namespace

void init(const Options& opts) {
    g_opts = opts;

    // https://no-color.org: any non-empty NO_COLOR disables colour; so does a non-terminal stdout.
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color && *no_color) g_opts.color = false;
    const char* term_env = std::getenv("TERM");
    if (term_env && std::string_view(term_env) == "dumb") g_opts.color = false;
    if (!GN_ISATTY(GN_FILENO(stdout))) g_opts.color = false;

    // The usual ways of saying "nobody is there to answer": our own flag and variable, Debian's
    // DEBIAN_FRONTEND, the CI variable every CI system sets, and a stdin that isn't a terminal.
    const char* frontend = std::getenv("DEBIAN_FRONTEND");
    if (!g_opts.interactive) g_noninteractive_reason = "--noninteractive";
    else if (env_set("GN_NONINTERACTIVE")) g_noninteractive_reason = "GN_NONINTERACTIVE";
    else if (frontend && std::string_view(frontend) == "noninteractive")
        g_noninteractive_reason = "DEBIAN_FRONTEND=noninteractive";
    else if (env_set("CI")) g_noninteractive_reason = "CI";
    else if (!GN_ISATTY(GN_FILENO(stdin))) g_noninteractive_reason = "stdin is not a terminal";
    g_opts.interactive = g_noninteractive_reason.empty();

#ifdef _WIN32
    if (g_opts.color && !enable_vt(STD_OUTPUT_HANDLE)) g_opts.color = false;
    enable_vt(STD_ERROR_HANDLE);
    if (g_opts.unicode) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
#endif
}

bool unicode() { return g_opts.unicode; }
bool color() { return g_opts.color; }
bool interactive() { return g_opts.interactive; }
const std::string& noninteractive_reason() { return g_noninteractive_reason; }

std::string bold(std::string_view s) { return sgr("1", s); }
std::string dim(std::string_view s) { return sgr("2", s); }
std::string red(std::string_view s) { return sgr("31", s); }
std::string green(std::string_view s) { return sgr("32", s); }
std::string yellow(std::string_view s) { return sgr("33", s); }
std::string blue(std::string_view s) { return sgr("34", s); }
std::string magenta(std::string_view s) { return sgr("35", s); }
std::string cyan(std::string_view s) { return sgr("36", s); }

const Glyphs& g() { return g_opts.unicode ? kUnicode : kAscii; }

std::size_t width(std::string_view s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {  // skip "ESC [ ... m"
            while (i < s.size() && s[i] != 'm') ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++n;  // count lead bytes only
    }
    return n;
}

std::string repeat(const char* piece, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += piece;
    return out;
}

void success(std::string_view msg) { std::cout << green(g().ok) << ' ' << msg << '\n'; }
void error(std::string_view msg) { std::cerr << red(g().fail) << ' ' << red(msg) << '\n'; }
void warn(std::string_view msg) { std::cerr << yellow(g().warn) << ' ' << msg << '\n'; }
void info(std::string_view msg) { std::cout << blue(g().info) << ' ' << msg << '\n'; }

namespace {
constexpr std::size_t kRule = 78;
}

void section_begin(std::string_view title) {
    std::string head = std::string(g().top_left) + g().dash + ' ';
    std::size_t used = width(head) + width(title) + 1;
    std::cout << dim(head) << bold(cyan(title)) << ' '
              << dim(repeat(g().dash, used < kRule ? kRule - used : 3)) << '\n';
}

void row(std::string_view key, std::string_view value, std::size_t key_width) {
    std::size_t kw = width(key);
    std::string pad(kw < key_width ? key_width - kw : 1, ' ');
    std::cout << dim(g().vbar) << ' ' << dim(key) << pad << value << '\n';
}

void section_end() { std::cout << dim(std::string(g().bottom_left) + repeat(g().dash, kRule - 1)) << "\n\n"; }

bool stdin_is_tty() { return GN_ISATTY(GN_FILENO(stdin)) != 0; }
bool stdout_is_tty() { return GN_ISATTY(GN_FILENO(stdout)) != 0; }
bool stderr_is_tty() { return GN_ISATTY(GN_FILENO(stderr)) != 0; }

void clear_screen() {
    std::cout << std::flush;
#ifdef _WIN32
    // Consoles without VT processing (old conhost) don't understand the escape codes.
    if (!enable_vt(STD_OUTPUT_HANDLE)) {
        std::system("cls");
        return;
    }
#endif
    std::cout << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
}

void binary_stdout() {
#ifdef _WIN32
    std::fflush(stdout);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool prompt(std::string_view label, std::string& out) {
    if (!interactive()) return false;
    std::cerr << cyan(g().arrow) << ' ' << bold(label) << ' ' << std::flush;
    bool ok = static_cast<bool>(std::getline(std::cin, out));
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return ok;
}

bool prompt_secret(std::string_view label, std::string& out) {
    if (!interactive()) return false;
    if (!stdin_is_tty()) return prompt(label, out);
    std::cerr << cyan(g().arrow) << ' ' << bold(label) << ' ' << std::flush;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    bool ok = static_cast<bool>(std::getline(std::cin, out));
    SetConsoleMode(h, mode);
#else
    termios old{};
    tcgetattr(STDIN_FILENO, &old);
    termios quiet = old;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    bool ok = static_cast<bool>(std::getline(std::cin, out));
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
#endif
    std::cerr << '\n';
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return ok;
}

}  // namespace term

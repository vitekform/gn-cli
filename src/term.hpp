// Terminal output: colours, Unicode-or-ASCII glyphs, and prompts.
#pragma once

#include <string>
#include <string_view>

namespace term {

struct Options {
    bool unicode = true;
    bool color = true;
    bool interactive = true;  // false: --noninteractive
};

// Call once at start-up, before anything is printed.
void init(const Options& opts);

bool unicode();
bool color();
// Whether gn-cli may ask questions: on unless --noninteractive, $GN_NONINTERACTIVE,
// DEBIAN_FRONTEND=noninteractive, $CI, or a stdin that isn't a terminal says otherwise.
bool interactive();
// Why interactive() is off, for error messages ("" when it is on).
const std::string& noninteractive_reason();

// SGR wrappers; return the text unchanged when colour is off.
std::string bold(std::string_view s);
std::string dim(std::string_view s);
std::string red(std::string_view s);
std::string green(std::string_view s);
std::string yellow(std::string_view s);
std::string blue(std::string_view s);
std::string magenta(std::string_view s);
std::string cyan(std::string_view s);

// Glyphs, with an ASCII stand-in for --no-unicode.
struct Glyphs {
    const char* ok;
    const char* fail;
    const char* warn;
    const char* info;
    const char* arrow;
    const char* bullet;
    const char* dash;       // horizontal rule piece
    const char* vbar;       // left edge of a box
    const char* top_left;
    const char* bottom_left;
    const char* ellipsis;
};
const Glyphs& g();

// Display width of a UTF-8 string without ANSI escapes (one column per code point).
std::size_t width(std::string_view s);
std::string repeat(const char* piece, std::size_t n);

void success(std::string_view msg);
void error(std::string_view msg);
void warn(std::string_view msg);
void info(std::string_view msg);

// A titled section: "╭─ Title ───" … "╰───"; rows go through row().
void section_begin(std::string_view title);
void row(std::string_view key, std::string_view value, std::size_t key_width = 20);
void section_end();

// Reads a line from stdin, after printing the prompt to stderr. Returns false on EOF,
// and straight away when not interactive.
bool prompt(std::string_view label, std::string& out);
// Same, without echoing what is typed (when stdin is a terminal).
bool prompt_secret(std::string_view label, std::string& out);

bool stdin_is_tty();
bool stdout_is_tty();
bool stderr_is_tty();

// Clear the screen and its scrollback, cursor to the top left.
void clear_screen();

// Stop Windows from translating "\n" in binary data written to stdout. No-op elsewhere.
void binary_stdout();

}  // namespace term

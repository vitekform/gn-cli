#include "lineedit.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <fstream>
#include <iostream>

#include "term.hpp"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace lineedit {
namespace {

constexpr std::size_t kHistoryMax = 1000;
std::vector<std::string> g_history;

bool plain_read(const std::string& prompt, std::string& out) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, out)) return false;
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

}  // namespace

void history_load(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) g_history.push_back(line);
    if (g_history.size() > kHistoryMax) g_history.erase(g_history.begin(), g_history.end() - kHistoryMax);
}

void history_add(const std::string& line) {
    if (line.empty() || line.find_first_not_of(' ') == std::string::npos) return;
    if (!g_history.empty() && g_history.back() == line) return;
    g_history.push_back(line);
    if (g_history.size() > kHistoryMax) g_history.erase(g_history.begin());
}

void history_save(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    for (const auto& line : g_history) out << line << '\n';
}

#ifdef _WIN32

bool read_line(const std::string& prompt, std::string& out, const Completer&) { return plain_read(prompt, out); }

#else

namespace {

// The line as code points, each kept as its UTF-8 bytes, so the cursor moves by characters.
using Glyphs = std::vector<std::string>;

Glyphs to_glyphs(const std::string& s) {
    Glyphs g;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        g.push_back(s.substr(i, n));
        i += n;
    }
    return g;
}

std::string join(const Glyphs& g, std::size_t from = 0, std::size_t to = std::string::npos) {
    std::string s;
    for (std::size_t i = from; i < std::min(to, g.size()); ++i) s += g[i];
    return s;
}

std::size_t columns() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

// Raw mode for the duration of one read_line; restored however it ends.
class RawMode {
public:
    RawMode() {
        ok_ = tcgetattr(STDIN_FILENO, &old_) == 0;
        if (!ok_) return;
        termios raw = old_;
        raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        ok_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
    }
    ~RawMode() {
        if (ok_) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_);
    }
    bool ok() const { return ok_; }

private:
    termios old_{};
    bool ok_ = false;
};

int read_byte() {
    unsigned char c;
    for (;;) {
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (n == 0) return -1;
        if (errno != EINTR) return -1;
    }
}

class Editor {
public:
    Editor(const std::string& prompt, const Completer& complete) : prompt_(prompt), complete_(complete) {
        prompt_width_ = term::width(prompt_);
    }

    bool run(std::string& out) {
        history_index_ = g_history.size();
        refresh();
        for (;;) {
            int c = read_byte();
            if (c < 0) return finish(out, false);
            if (c != '\t') tabs_ = 0;
            switch (c) {
                case '\r':
                case '\n':
                    return finish(out, true);
                case 3:  // Ctrl-C: drop the line
                    std::cout << "^C";
                    line_.clear();
                    return finish(out, true);
                case 4:  // Ctrl-D: end of input on an empty line, else delete forward
                    if (line_.empty()) return finish(out, false);
                    erase_at(pos_);
                    break;
                case 127:
                case 8:
                    if (pos_ > 0) erase_at(--pos_);
                    break;
                case 1: pos_ = 0; break;                 // Ctrl-A
                case 5: pos_ = line_.size(); break;      // Ctrl-E
                case 2: if (pos_ > 0) --pos_; break;     // Ctrl-B
                case 6: if (pos_ < line_.size()) ++pos_; break;  // Ctrl-F
                case 11: line_.resize(pos_); break;      // Ctrl-K
                case 21:                                 // Ctrl-U
                    line_.erase(line_.begin(), line_.begin() + static_cast<std::ptrdiff_t>(pos_));
                    pos_ = 0;
                    break;
                case 23: delete_word_back(); break;      // Ctrl-W
                case 12:                                 // Ctrl-L
                    term::clear_screen();
                    break;
                case 16: history_step(-1); break;        // Ctrl-P
                case 14: history_step(+1); break;        // Ctrl-N
                case '\t': tab(); break;
                case 27: escape(); break;
                default:
                    if (c >= 32) insert(read_rest_of_glyph(c));
            }
            refresh();
        }
    }

private:
    std::string read_rest_of_glyph(int first) {
        std::string g(1, static_cast<char>(first));
        unsigned char c = static_cast<unsigned char>(first);
        std::size_t more = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : 0;
        for (std::size_t i = 0; i < more; ++i) {
            int next = read_byte();
            if (next < 0) break;
            g += static_cast<char>(next);
        }
        return g;
    }

    void insert(const std::string& glyph) {
        line_.insert(line_.begin() + static_cast<std::ptrdiff_t>(pos_), glyph);
        ++pos_;
    }

    void erase_at(std::size_t i) {
        if (i < line_.size()) line_.erase(line_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void delete_word_back() {
        std::size_t start = pos_;
        while (start > 0 && line_[start - 1] == " ") --start;
        while (start > 0 && line_[start - 1] != " ") --start;
        line_.erase(line_.begin() + static_cast<std::ptrdiff_t>(start), line_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = start;
    }

    void history_step(int dir) {
        if (g_history.empty()) return;
        if (history_index_ == g_history.size()) draft_ = join(line_);
        std::size_t next = dir < 0 ? (history_index_ == 0 ? 0 : history_index_ - 1)
                                   : std::min(history_index_ + 1, g_history.size());
        if (next == history_index_) return;
        history_index_ = next;
        line_ = to_glyphs(history_index_ == g_history.size() ? draft_ : g_history[history_index_]);
        pos_ = line_.size();
    }

    // ESC [ A/B/C/D/H/F, ESC [ 1~ 3~ 4~ 7~ 8~, ESC O H/F, Alt-b/f/d.
    void escape() {
        int a = read_byte();
        if (a == 'b' || a == 'f') {  // Alt-b / Alt-f: by words
            if (a == 'b') {
                while (pos_ > 0 && line_[pos_ - 1] == " ") --pos_;
                while (pos_ > 0 && line_[pos_ - 1] != " ") --pos_;
            } else {
                while (pos_ < line_.size() && line_[pos_] == " ") ++pos_;
                while (pos_ < line_.size() && line_[pos_] != " ") ++pos_;
            }
            return;
        }
        if (a != '[' && a != 'O') return;
        int b = read_byte();
        if (b >= '0' && b <= '9') {
            int t = read_byte();
            while (t != '~' && t >= 0 && t != 27 && !(t >= 'A' && t <= 'Z')) t = read_byte();  // e.g. ESC[1;5C
            if (t != '~') return;
            if (b == '1' || b == '7') pos_ = 0;
            else if (b == '4' || b == '8') pos_ = line_.size();
            else if (b == '3') erase_at(pos_);
            return;
        }
        switch (b) {
            case 'A': history_step(-1); break;
            case 'B': history_step(+1); break;
            case 'C': if (pos_ < line_.size()) ++pos_; break;
            case 'D': if (pos_ > 0) --pos_; break;
            case 'H': pos_ = 0; break;
            case 'F': pos_ = line_.size(); break;
            default: break;
        }
    }

    // One candidate: take it (plus a space, unless it is a folder). Several: extend to their
    // common prefix; a second Tab lists them.
    void tab() {
        if (!complete_) return;
        std::string before = join(line_, 0, pos_);
        std::size_t word_start = before.size();
        std::vector<std::string> options = complete_(before, word_start);
        if (options.empty()) return;
        std::string typed = before.substr(std::min(word_start, before.size()));
        std::string after = join(line_, pos_);
        auto replace_word = [&](const std::string& with) {
            Glyphs head = to_glyphs(before.substr(0, word_start) + with);
            pos_ = head.size();
            Glyphs tail = to_glyphs(after);
            head.insert(head.end(), tail.begin(), tail.end());
            line_ = std::move(head);
        };
        if (options.size() == 1) {
            const std::string& only = options[0];
            bool dir = !only.empty() && (only.back() == '/' || only.back() == '\\');
            replace_word(only + (dir || (!after.empty() && after[0] == ' ') ? "" : " "));
            return;
        }
        std::string common = options[0];
        for (const auto& o : options) {
            std::size_t n = 0;
            while (n < common.size() && n < o.size() && common[n] == o[n]) ++n;
            common.resize(n);
        }
        if (common.size() > typed.size()) {
            replace_word(common);
            return;
        }
        if (++tabs_ < 2) {
            std::cout << '\a' << std::flush;
            return;
        }
        // List them in columns under the line, then redraw the prompt.
        std::size_t widest = 0;
        for (const auto& o : options) widest = std::max(widest, term::width(o));
        std::size_t per_row = std::max<std::size_t>(1, columns() / (widest + 2));
        std::cout << "\r\n";
        for (std::size_t i = 0; i < options.size(); ++i) {
            std::cout << options[i] << std::string(widest + 2 - term::width(options[i]), ' ');
            if ((i + 1) % per_row == 0 || i + 1 == options.size()) std::cout << "\r\n";
        }
        tabs_ = 0;
    }

    // Single-line rendering: the visible window scrolls sideways when the line is too long.
    void refresh() {
        std::size_t cols = columns();
        std::size_t room = cols > prompt_width_ + 2 ? cols - prompt_width_ - 1 : 1;
        if (pos_ < offset_) offset_ = pos_;
        if (pos_ >= offset_ + room) offset_ = pos_ - room + 1;
        std::string shown = join(line_, offset_, offset_ + room);
        std::cout << '\r' << prompt_ << shown << "\x1b[K\r";
        std::size_t col = prompt_width_ + (pos_ - offset_);
        if (col > 0) std::cout << "\x1b[" << col << 'C';
        std::cout << std::flush;
    }

    bool finish(std::string& out, bool got_line) {
        pos_ = line_.size();
        offset_ = 0;
        refresh();
        std::cout << "\r\n" << std::flush;
        out = join(line_);
        return got_line;
    }

    std::string prompt_;
    std::size_t prompt_width_ = 0;
    const Completer& complete_;
    Glyphs line_;
    std::size_t pos_ = 0, offset_ = 0;
    std::size_t history_index_ = 0;
    std::string draft_;
    int tabs_ = 0;
};

}  // namespace

bool read_line(const std::string& prompt, std::string& out, const Completer& complete) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return plain_read(prompt, out);
    const char* t = std::getenv("TERM");
    if (t && std::string_view(t) == "dumb") return plain_read(prompt, out);
    std::cout << std::flush;
    RawMode raw;
    if (!raw.ok()) return plain_read(prompt, out);
    Editor editor(prompt, complete);
    return editor.run(out);
}

#endif

}  // namespace lineedit

// Helpers shared by the command implementations: formatting, authentication, prompts,
// progress, and moving files up and down in parts.
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "api.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "http.hpp"
#include "term.hpp"

namespace commands {

using nlohmann::json;
namespace fs = std::filesystem;

// ── text ───────────────────────────────────────────────────────────────────

const char* none();  // "—", or "-" without Unicode
std::string trim(std::string s);
std::string format_epoch(double seconds);
// Jackson may send an Instant/LocalDateTime as an ISO string, epoch seconds, or an array.
std::string format_time(const json& j);
std::string str(const json& obj, const char* key, const std::string& fallback = "");
double num(const json& obj, const char* key);
std::string money(double v, int decimals = 4);
std::string with_thousands(long long v);
std::string pad_right(const std::string& s, std::size_t w);
std::string pad_left(const std::string& s, std::size_t w);
std::string truncate(const std::string& s, std::size_t w);
std::vector<std::string> string_list(const json& obj, const char* key);
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::string expiry_text(const json& obj);
void print_named_list(const char* title, const json& items, const char* name_key, const char* empty);
// "never" → nullopt; "30", "30d", "4w", "1y" → days. Throws std::invalid_argument on anything else.
std::optional<int> parse_expiry(const std::string& text);
std::string human_size(std::uint64_t bytes);
// The "Credits" section of gn me and gn credits: balance, then the most recent `show` requests.
void print_credits(const json& credits, std::size_t show);

// ── files ──────────────────────────────────────────────────────────────────

std::string base_name(std::string key);
std::string utf8(const fs::path& p);
// Keys are UTF-8; a plain std::string path would go through the ANSI code page on Windows.
fs::path from_utf8(const std::string& s);
std::string guess_content_type(const std::string& name);
// A key relative to the folder being downloaded, as a local path that can't leave that folder.
std::optional<fs::path> safe_local_path(const std::string& rel);
// Where a download goes: -o (a file, a folder, or - for stdout), else the name in the current folder.
fs::path download_target(const Context& ctx, const std::string& filename);

// ── auth and prompts ───────────────────────────────────────────────────────

std::string resolve_url(const Context& ctx, const config::Credentials* creds);
void not_logged_in();
// "No email given." plus, when that is because nobody can be asked, why not and what to pass instead.
void missing_input(const std::string& msg, const std::string& hint);
// Interactive mode asks for missing positional arguments, named after the synopsis's <placeholders>:
// "upload <provider> <bucket> <bucket_path> <local_path>" with two given asks for the last two.
bool ask_missing_args(const Context& ctx, std::size_t needed, const std::string& synopsis);
// ctx.args has at least `needed` entries (asking for the rest in interactive mode), or prints
// "Usage: gn-cli <synopsis>" and returns false.
bool need_args(const Context& ctx, std::size_t needed, const std::string& synopsis);
bool confirm(const Context& ctx, const std::string& question);

// Whatever this run authenticates with: the CLI token from --token/$GN_TOKEN, or the saved session.
struct Auth {
    std::optional<config::Credentials> creds;
    api::Client client;

    config::Credentials* session() { return creds ? &*creds : nullptr; }
};

std::optional<Auth> authenticate(const Context& ctx);
// Token management needs a real session: a CLI token can't mint or revoke tokens.
std::optional<Auth> authenticate_session(const Context& ctx);

// ── transfers ──────────────────────────────────────────────────────────────

// One line on stderr, redrawn in place: "↑ name  [████░░░░]  45%  12.3 MiB / 27.1 MiB  3.4 MiB/s".
class ProgressBar {
public:
    ProgressBar(const char* arrow, std::string label, std::uint64_t total, bool enabled)
        : arrow_(arrow), label_(truncate(std::move(label), 28)), total_(total), enabled_(enabled),
          start_(std::chrono::steady_clock::now()) {}
    ~ProgressBar() { finish(); }

    void update(std::uint64_t done, bool force = false) {
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
        if (!force && drawn_ && now - last_ < std::chrono::milliseconds(100)) return;
        last_ = now;
        double secs = std::chrono::duration<double>(now - start_).count();
        std::string rate = secs > 0.2 ? "  " + human_size(static_cast<std::uint64_t>(done / secs)) + "/s" : "";
        std::string line = term::cyan(arrow_) + " " + pad_right(label_, 29);
        if (total_ > 0) {
            done = std::min(done, total_);
            constexpr int kWidth = 24;
            int filled = static_cast<int>(static_cast<double>(done) / static_cast<double>(total_) * kWidth);
            bool uni = term::unicode();
            line += "[" + term::green(term::repeat(uni ? "█" : "#", static_cast<std::size_t>(filled))) +
                    term::dim(term::repeat(uni ? "░" : "-", static_cast<std::size_t>(kWidth - filled))) + "] ";
            line += pad_left(std::to_string(done * 100 / total_) + "%", 4) + "  " + human_size(done) + " / " +
                    human_size(total_);
        } else {
            line += human_size(done);
        }
        draw(line + term::dim(rate));
    }

    // Replaces the numbers with a word, for the wait after the last byte.
    void note(const std::string& text) {
        if (enabled_) draw(term::cyan(arrow_) + " " + pad_right(label_, 29) + term::dim(text + term::g().ellipsis));
    }

    void finish() {
        if (!enabled_ || !drawn_) return;
        std::cerr << '\r' << std::string(drawn_, ' ') << '\r' << std::flush;
        drawn_ = 0;
    }

private:
    void draw(const std::string& line) {
        std::size_t w = term::width(line);
        std::cerr << '\r' << line;
        if (w < drawn_) std::cerr << std::string(drawn_ - w, ' ');
        std::cerr << std::flush;
        drawn_ = std::max<std::size_t>(w, 1);
    }

    std::string arrow_;
    std::string label_;
    std::uint64_t total_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_, last_;
    std::size_t drawn_ = 0;
};

// While one lives, Ctrl-C raises a flag instead of ending gn: for waits and refresh loops that
// should stop cleanly. Restores the previous handler when it goes.
class CtrlC {
public:
    CtrlC();
    ~CtrlC();
    CtrlC(const CtrlC&) = delete;
    CtrlC& operator=(const CtrlC&) = delete;
    bool pressed() const;

private:
    void (*previous_)(int);
};

bool show_progress();
// Retries a transfer that died on the way or hit a 5xx: 3 attempts, 2 s and 4 s apart.
void with_retries(const std::string& what, const std::function<void()>& attempt);

// Sends a local file as numbered parts (1-based) of part_size bytes, each a multipart/form-data
// "file" field POSTed to part_path(n), with retries, drawing on bar.
void upload_parts(Auth& a, const fs::path& file, const std::string& name, std::uint64_t size, std::uint64_t part_size,
                  std::uint64_t parts, const std::function<std::string(std::uint64_t)>& part_path, ProgressBar& bar);

// RFC 3986 percent-encoding for one path segment or query value.
std::string pct_encode(const std::string& s);

// /api/v3/storage/{provider}/{bucket}/object, encoded: where a stored file is downloaded from.
std::string storage_object_path(const std::string& provider, const std::string& bucket);

// Sends a local file through /api/v2/multipart (init, parts, complete) and returns its upload
// id, which other endpoints then take in place of the file (S3, secure storage). `after` is what
// the progress line says while the server assembles the parts.
std::string multipart_upload(Auth& a, const fs::path& file, const std::string& label, std::size_t part_size,
                             const std::string& after = "assembling");

// Streams into dest through a .gn-part file, hashing as it goes; a checksum mismatch leaves nothing
// behind. An empty dest means stdout; an empty expected_sha skips the check.
void fetch_into(const fs::path& dest, const std::string& label, std::uint64_t size, const std::string& expected_sha,
                const std::function<void(const http::Sink&, const http::Progress&)>& fetch);

}  // namespace commands

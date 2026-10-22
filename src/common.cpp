#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "sha256.hpp"
#include "term.hpp"

namespace {
volatile std::sig_atomic_t g_ctrl_c = 0;
extern "C" void gn_on_ctrl_c(int) { g_ctrl_c = 1; }
}  // namespace

namespace commands {

CtrlC::CtrlC() {
    g_ctrl_c = 0;
    previous_ = std::signal(SIGINT, gn_on_ctrl_c);
}
CtrlC::~CtrlC() { std::signal(SIGINT, previous_ == SIG_ERR ? SIG_DFL : previous_); }
bool CtrlC::pressed() const { return g_ctrl_c != 0; }

const char* none() { return term::unicode() ? "—" : "-"; }

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && issp(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string format_epoch(double seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    return buf;
}

// Jackson may send an Instant/LocalDateTime as an ISO string, epoch seconds, or an array.
std::string format_time(const json& j) {
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.size() >= 16 && s[10] == 'T') {
            bool utc = s.back() == 'Z';
            return s.substr(0, 10) + " " + s.substr(11, 5) + (utc ? " UTC" : "");
        }
        return s;
    }
    if (j.is_number()) {
        double v = j.get<double>();
        if (v > 1e11) v /= 1000.0;  // milliseconds
        return format_epoch(v);
    }
    if (j.is_array() && j.size() >= 5) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", j[0].get<int>(), j[1].get<int>(), j[2].get<int>(),
                      j[3].get<int>(), j[4].get<int>());
        return buf;
    }
    return none();
}

std::string str(const json& obj, const char* key, const std::string& fallback) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

double num(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return 0;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

std::string money(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "$%.*f", decimals, v);
    return buf;
}

std::string with_thousands(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

std::string pad_right(const std::string& s, std::size_t w) {
    std::size_t n = term::width(s);
    return n >= w ? s : s + std::string(w - n, ' ');
}

std::string pad_left(const std::string& s, std::size_t w) {
    std::size_t n = term::width(s);
    return n >= w ? s : std::string(w - n, ' ') + s;
}

std::string truncate(const std::string& s, std::size_t w) {
    if (term::width(s) <= w) return s;
    // Cut on a code point boundary.
    std::string out;
    std::size_t cols = 0;
    std::size_t ell = term::width(term::g().ellipsis);
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            if (cols + ell >= w) break;
            ++cols;
        }
        out += s[i];
    }
    return out + term::g().ellipsis;
}

std::string resolve_url(const Context& ctx, const config::Credentials* creds) {
    if (!ctx.api_url.empty()) return ctx.api_url;
    if (creds && !creds->api_url.empty()) return creds->api_url;
    return api::kDefaultUrl;
}

void not_logged_in() {
    term::error("You are not logged in.");
    std::cerr << "  Run " << term::bold("gn-cli auth login") << " first.\n";
}

// "No email given." plus, when that is because nobody can be asked, why not and what to pass instead.
void missing_input(const std::string& msg, const std::string& hint) {
    term::error(msg);
    if (!term::interactive())
        std::cerr << "  Not interactive (" << term::noninteractive_reason() << "); " << hint << ".\n";
}

// Interactive mode asks for missing positional arguments, named after the synopsis's <placeholders>:
// "upload <provider> <bucket> <bucket_path> <local_path>" with two given asks for the last two.
bool ask_missing_args(const Context& ctx, std::size_t needed, const std::string& synopsis) {
    if (!term::interactive()) return false;
    std::vector<std::string> names;
    for (std::size_t open = synopsis.find('<'); open != std::string::npos; open = synopsis.find('<', open + 1)) {
        std::size_t close = synopsis.find('>', open);
        if (close == std::string::npos) break;
        // Skip the value of an option, like the <name> in "[--name <name>]".
        std::size_t word_end = synopsis.find_last_not_of(' ', open == 0 ? 0 : open - 1);
        std::size_t word = synopsis.find_last_of(" [", word_end);
        word = word == std::string::npos ? 0 : word + 1;
        if (open > 0 && synopsis[word] == '-') continue;
        names.push_back(synopsis.substr(open + 1, close - open - 1));
    }
    if (names.size() < needed) return false;
    while (ctx.args.size() < needed) {
        std::string value;
        if (!term::prompt(names[ctx.args.size()] + ":", value)) return false;
        value = trim(value);
        if (value.empty()) return false;
        ctx.args.push_back(value);
    }
    return true;
}

std::optional<Auth> authenticate(const Context& ctx) {
    std::optional<config::Credentials> creds;
    if (ctx.token.empty()) {
        creds = config::load();
        if (!creds) {
            not_logged_in();
            return std::nullopt;
        }
    }
    std::string ua = creds && !creds->user_agent.empty() ? creds->user_agent : api::default_user_agent();
    Auth a{creds, api::Client(resolve_url(ctx, creds ? &*creds : nullptr), ua)};
    if (!ctx.token.empty()) a.client.use_cli_token(ctx.token);
    return a;
}

// Token management needs a real session: a CLI token can't mint or revoke tokens.
std::optional<Auth> authenticate_session(const Context& ctx) {
    if (!ctx.token.empty()) {
        term::error("Managing CLI tokens needs a login session, not a CLI token.");
        std::cerr << "  Drop " << term::bold("--token") << " (or unset " << term::bold("GN_TOKEN")
                  << ") and run " << term::bold("gn-cli auth login") << " if needed.\n";
        return std::nullopt;
    }
    return authenticate(ctx);
}

std::vector<std::string> string_list(const json& obj, const char* key) {
    std::vector<std::string> out;
    if (auto it = obj.find(key); it != obj.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : sep) + s;
    return out;
}

std::string expiry_text(const json& obj) {
    auto it = obj.find("expiresAt");
    if (it == obj.end() || it->is_null()) return term::dim("never");
    if (obj.value("expired", false)) return term::red(format_time(*it) + " (expired)");
    return format_time(*it);
}

void print_named_list(const char* title, const json& items, const char* name_key, const char* empty) {
    const auto& G = term::g();
    term::section_begin(std::string(title) + " (" + std::to_string(items.size()) + ")");
    if (items.empty()) term::row("", term::dim(empty), 0);
    for (const auto& item : items) {
        std::string name = str(item, name_key, "unnamed");
        std::string created = item.contains("createdAt") ? format_time(item["createdAt"]) : "";
        std::cout << term::dim(G.vbar) << ' ' << term::cyan(G.bullet) << ' ' << pad_right(truncate(name, 28), 30)
                  << term::dim(created) << '\n';
    }
    term::section_end();
}

// "never" → nullopt; "30", "30d", "4w", "1y" → days. Throws std::invalid_argument on anything else.
std::optional<int> parse_expiry(const std::string& text) {
    std::string s = trim(text);
    if (s == "never" || s == "none" || s == "0") return std::nullopt;
    int mult = 1;
    char unit = s.empty() ? 'x' : s.back();
    if (unit == 'd' || unit == 'w' || unit == 'y') {
        mult = unit == 'w' ? 7 : unit == 'y' ? 365 : 1;
        s.pop_back();
    }
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 6)
        throw std::invalid_argument("--expires takes e.g. 30d, 12w, 1y or never, not '" + text + "'");
    return std::stoi(s) * mult;
}

std::string base_name(std::string key) {
    while (!key.empty() && key.back() == '/') key.pop_back();
    std::size_t slash = key.rfind('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

std::string utf8(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Keys are UTF-8; a plain std::string path would go through the ANSI code page on Windows.
fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string human_size(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string guess_content_type(const std::string& name) {
    std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> types[] = {
        {"jpg", "image/jpeg"},        {"jpeg", "image/jpeg"},     {"png", "image/png"},
        {"gif", "image/gif"},         {"webp", "image/webp"},     {"avif", "image/avif"},
        {"svg", "image/svg+xml"},     {"bmp", "image/bmp"},       {"ico", "image/x-icon"},
        {"mp4", "video/mp4"},         {"webm", "video/webm"},     {"mov", "video/quicktime"},
        {"mkv", "video/x-matroska"},  {"mp3", "audio/mpeg"},      {"wav", "audio/wav"},
        {"ogg", "audio/ogg"},         {"flac", "audio/flac"},     {"m4a", "audio/mp4"},
        {"pdf", "application/pdf"},   {"txt", "text/plain"},      {"md", "text/markdown"},
        {"csv", "text/csv"},          {"html", "text/html"},      {"htm", "text/html"},
        {"css", "text/css"},          {"js", "text/javascript"},  {"json", "application/json"},
        {"xml", "application/xml"},   {"zip", "application/zip"}, {"gz", "application/gzip"},
        {"tar", "application/x-tar"}, {"7z", "application/x-7z-compressed"},
        {"apk", "application/vnd.android.package-archive"},
    };
    for (const auto& [e, t] : types)
        if (ext == e) return t;
    return "application/octet-stream";
}

bool show_progress() { return term::stderr_is_tty(); }

// Retries a transfer that died on the way or hit a 5xx: 3 attempts, 2 s and 4 s apart.
void with_retries(const std::string& what, const std::function<void()>& attempt) {
    for (int n = 1;; ++n) {
        try {
            attempt();
            return;
        } catch (const http::TransportError& e) {
            if (n >= 3) throw;
            term::warn(what + " failed (" + e.what() + "), retrying" + term::g().ellipsis);
        } catch (const api::ApiError& e) {
            if (e.status < 500 || n >= 3) throw;
            term::warn(what + " failed (" + e.what() + "), retrying" + term::g().ellipsis);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 << n));
    }
}

// A key relative to the folder being downloaded, as a local path that can't leave that folder.
std::optional<fs::path> safe_local_path(const std::string& rel) {
    if (rel.empty()) return std::nullopt;
    fs::path out;
    std::size_t start = 0;
    while (start <= rel.size()) {
        std::size_t slash = rel.find('/', start);
        std::string seg = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == ".." || seg.find('\\') != std::string::npos || seg.find(':') != std::string::npos)
            return std::nullopt;
        if (!seg.empty() && seg != ".") out /= from_utf8(seg);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (out.empty() || out.is_absolute() || out.has_root_name() || out.has_root_directory()) return std::nullopt;
    return out;
}

bool confirm(const Context& ctx, const std::string& question) {
    if (ctx.yes) return true;
    if (!term::interactive()) {
        term::error(question + " Pass --yes to confirm when not interactive (" + term::noninteractive_reason() + ").");
        return false;
    }
    std::string answer;
    if (!term::prompt(question + " [y/N]", answer)) return false;
    answer = trim(answer);
    return answer == "y" || answer == "Y" || answer == "yes";
}

// Where a download goes: -o (a file, a folder, or - for stdout), else the name in the current folder.
fs::path download_target(const Context& ctx, const std::string& filename) {
    if (ctx.output == "-") return {};
    if (ctx.output.empty()) return from_utf8(filename);
    fs::path dest = ctx.output;
    std::error_code ec;
    if (ctx.output.back() == '/' || ctx.output.back() == '\\' || fs::is_directory(dest, ec)) dest /= from_utf8(filename);
    return dest;
}

// Streams into dest through a .gn-part file, hashing as it goes; a checksum mismatch leaves nothing behind.
void fetch_into(const fs::path& dest, const std::string& label, std::uint64_t size, const std::string& expected_sha,
                const std::function<void(const http::Sink&, const http::Progress&)>& fetch) {
    ProgressBar bar(term::unicode() ? "↓" : "v", label, size, show_progress());
    auto progress = [&](std::uint64_t done, std::uint64_t) { bar.update(done); };
    sha256::Hasher hasher;
    auto check = [&] {
        bar.finish();
        std::string got = hasher.hex();
        if (!expected_sha.empty() && got != expected_sha)
            throw std::runtime_error("checksum mismatch for " + label + ": expected " + expected_sha + ", got " + got);
    };

    if (dest.empty()) {
        term::binary_stdout();
        fetch([&](const char* d, std::size_t n) {
            hasher.update(d, n);
            return std::fwrite(d, 1, n, stdout) == n;
        }, progress);
        std::fflush(stdout);
        check();
        return;
    }
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path());
    fs::path tmp = dest;
    tmp += ".gn-part";
    try {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("can't write " + utf8(tmp));
            fetch([&](const char* d, std::size_t n) {
                hasher.update(d, n);
                return static_cast<bool>(out.write(d, static_cast<std::streamsize>(n)));
            }, progress);
            out.close();
            if (!out) throw std::runtime_error("can't write " + utf8(tmp));
        }
        check();
        std::error_code ec;
        fs::remove(dest, ec);  // rename won't replace a file on Windows
        fs::rename(tmp, dest);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

bool need_args(const Context& ctx, std::size_t needed, const std::string& synopsis) {
    if (ctx.args.size() >= needed || ask_missing_args(ctx, needed, synopsis)) return true;
    term::error("Usage: gn-cli " + synopsis);
    return false;
}

std::string multipart_upload(Auth& a, const fs::path& file, const std::string& label, std::size_t part_size,
                             const std::string& after) {
    std::uint64_t size = fs::file_size(file);
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + utf8(file));
    std::uint64_t parts = std::max<std::uint64_t>(1, (size + part_size - 1) / part_size);

    json init = a.client.post_form("/api/v2/multipart/init", {}, a.session());
    std::string upload_id = str(init, "uploadId");
    if (upload_id.empty()) throw std::runtime_error("the server didn't start the upload");

    ProgressBar bar(term::unicode() ? "↑" : "^", label, size, show_progress());
    std::vector<char> buf(static_cast<std::size_t>(std::min<std::uint64_t>(part_size, size)));
    std::string file_name = utf8(file.filename());
    std::uint64_t sent = 0;
    for (std::uint64_t part = 1; part <= parts; ++part) {
        std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(part_size, size - sent));
        if (n > 0 && !in.read(buf.data(), static_cast<std::streamsize>(n)))
            throw std::runtime_error("could not read " + utf8(file) + " (did it shrink?)");
        http::FilePart fp{"file", file_name + ".part" + std::to_string(part), buf.data(), n};
        http::Form fields{{"uploadId", upload_id}, {"partNumber", std::to_string(part)}};
        with_retries("Part " + std::to_string(part) + "/" + std::to_string(parts), [&] {
            a.client.post_file("/api/v2/multipart/upload-part", fields, fp, a.session(),
                               [&](std::uint64_t done, std::uint64_t) { bar.update(sent + std::min<std::uint64_t>(done, n)); });
        });
        sent += n;
        bar.update(sent, true);
    }
    bar.note(after);
    a.client.post_form("/api/v2/multipart/complete", {{"uploadId", upload_id}, {"totalParts", std::to_string(parts)}},
                       a.session(), 0);
    return upload_id;
}

std::string pct_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string storage_object_path(const std::string& provider, const std::string& bucket) {
    return "/api/v3/storage/" + pct_encode(provider) + "/" + pct_encode(bucket) + "/object";
}

void upload_parts(Auth& a, const fs::path& file, const std::string& name, std::uint64_t size, std::uint64_t part_size,
                  std::uint64_t parts, const std::function<std::string(std::uint64_t)>& part_path, ProgressBar& bar) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + utf8(file));
    std::vector<char> buf(static_cast<std::size_t>(std::min<std::uint64_t>(part_size, size)));
    std::uint64_t sent = 0;
    for (std::uint64_t part = 1; part <= parts; ++part) {
        std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(part_size, size - sent));
        if (n > 0 && !in.read(buf.data(), static_cast<std::streamsize>(n)))
            throw std::runtime_error("could not read " + utf8(file) + " (did it shrink?)");
        http::FilePart fp{"file", name + ".part" + std::to_string(part), buf.data(), n};
        with_retries("Part " + std::to_string(part) + "/" + std::to_string(parts), [&] {
            a.client.post_file(part_path(part), {}, fp, a.session(),
                               [&](std::uint64_t done, std::uint64_t) { bar.update(sent + std::min<std::uint64_t>(done, n)); });
        });
        sent += n;
        bar.update(sent, true);
    }
}

void print_credits(const json& credits, std::size_t show) {
    const auto& G = term::g();
    term::section_begin("Credits");
    double balance = num(credits, "creditBalance");
    std::string bal = money(balance);
    term::row("Balance", term::bold(balance > 0 ? term::green(bal) : term::red(bal)));

    const json* usage = nullptr;
    if (auto it = credits.find("usage"); it != credits.end() && it->is_array()) usage = &*it;
    if (!usage || usage->empty()) {
        term::row("Recent usage", term::dim("none"));
    } else {
        const std::size_t kShow = show;
        double total = 0;
        for (const auto& u : *usage) total += num(u, "costUsd");
        term::row("Recent usage", std::to_string(usage->size()) + " requests, " + money(total) + " total");
        std::cout << term::dim(G.vbar) << '\n';
        std::cout << term::dim(G.vbar) << "  "
                  << term::dim(pad_right("When", 21) + pad_right("Model", 24) + pad_left("In", 9) +
                               pad_left("Out", 9) + pad_left("Cost", 11))
                  << '\n';
        std::size_t shown = 0;
        for (const auto& u : *usage) {
            if (shown++ == kShow) break;
            std::string when = u.contains("createdAt") ? format_time(u["createdAt"]) : none();
            std::cout << term::dim(G.vbar) << "  " << pad_right(when, 21)
                      << term::cyan(pad_right(truncate(str(u, "modelName", "?"), 22), 24))
                      << pad_left(with_thousands(static_cast<long long>(num(u, "promptTokens"))), 9)
                      << pad_left(with_thousands(static_cast<long long>(num(u, "completionTokens"))), 9)
                      << term::yellow(pad_left(money(num(u, "costUsd")), 11)) << '\n';
        }
        if (usage->size() > kShow)
            std::cout << term::dim(G.vbar) << "  "
                      << term::dim(G.ellipsis + (" and " + std::to_string(usage->size() - kShow) + " more")) << '\n';
    }
    term::section_end();
}

}  // namespace commands

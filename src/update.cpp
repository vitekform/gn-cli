// gn update: replace this binary with the newest gn-cli release from rsys.ganamaga.me.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#define GN_POPEN _popen
#define GN_PCLOSE _pclose
#else
#include <sys/stat.h>
#define GN_POPEN popen
#define GN_PCLOSE pclose
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const char* kProject = "gn-cli";
constexpr long kCheckEverySeconds = 24 * 3600;

// The release file built for this platform (see the build matrix in .github/workflows/gn-cli.yml).
const char* platform_file() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "gn-cli-windows-x64.exe";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "gn-cli-macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "gn-cli-linux-x64";
#else
    return nullptr;
#endif
}

// The running binary, with symlinks resolved: `gn` → gn-cli updates gn-cli.
std::optional<fs::path> self_path() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::nullopt;
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    fs::path p(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    fs::path p(buf.c_str());
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
#endif
    fs::path canonical = fs::canonical(p, ec);
    return ec ? p : canonical;
}

bool installed_by_homebrew(const fs::path& exe) { return utf8(exe).find("/Cellar/gn-cli/") != std::string::npos; }

// Semantic-ish version order: numbers first ("1.10.0" > "1.9.2"), then a release beats its own
// prereleases ("1.2.0" > "1.2.0-rc.1"), and prerelease identifiers compare numerically when they
// are numbers ("1.2.0-dev.57" > "1.2.0-dev.9"). Returns <0, 0, >0 like strcmp.
int compare_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v) {
        std::string s = v;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        if (auto plus = s.find('+'); plus != std::string::npos) s.erase(plus);
        auto dash = s.find('-');
        std::string core = s.substr(0, dash), pre = dash == std::string::npos ? "" : s.substr(dash + 1);
        auto pieces = [](const std::string& text, char sep) {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start <= text.size()) {
                auto at = text.find(sep, start);
                out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
                if (at == std::string::npos) break;
                start = at + 1;
            }
            return out;
        };
        return std::make_pair(pieces(core, '.'), pre.empty() ? std::vector<std::string>{} : pieces(pre, '.'));
    };
    auto is_num = [](const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
    auto cmp_ident = [&](const std::string& x, const std::string& y) {
        if (is_num(x) && is_num(y)) {
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;  // no leading zeros in versions
            return x.compare(y) < 0 ? -1 : x.compare(y) > 0 ? 1 : 0;
        }
        if (is_num(x) != is_num(y)) return is_num(x) ? -1 : 1;  // numbers sort before words
        return x.compare(y) < 0 ? -1 : x.compare(y) > 0 ? 1 : 0;
    };
    auto [ca, pa] = split(a);
    auto [cb, pb] = split(b);
    for (std::size_t i = 0; i < std::max(ca.size(), cb.size()); ++i) {
        int c = cmp_ident(i < ca.size() ? ca[i] : "0", i < cb.size() ? cb[i] : "0");
        if (c != 0) return c;
    }
    if (pa.empty() != pb.empty()) return pa.empty() ? 1 : -1;
    for (std::size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        if (i >= pa.size()) return -1;
        if (i >= pb.size()) return 1;
        int c = cmp_ident(pa[i], pb[i]);
        if (c != 0) return c;
    }
    return 0;
}

std::string rsys_base(const Context& ctx) {
    if (!ctx.rsys_url.empty()) return ctx.rsys_url;
    if (!ctx.api_url.empty()) return ctx.api_url;
    return api::kDefaultRsysUrl;
}

json fetch_latest(const Context& ctx, bool prerelease, long timeout_s = 60) {
    api::Client pub(rsys_base(ctx), api::default_user_agent());
    std::string query = "?since=" + std::string(GN_CLI_VERSION) + (prerelease ? "&prerelease=true" : "");
    return pub.get_public("/" + std::string(kProject) + "/public/latest.json" + query, timeout_s);
}

// Runs `<exe> --version` and checks it names the expected version: a download that doesn't run
// here (wrong architecture, missing library) must never replace a working binary.
bool runs_as(const fs::path& exe, const std::string& version) {
#ifdef _WIN32
    std::string cmd = "\"\"" + utf8(exe) + "\" --version 2>NUL\"";
#else
    std::string cmd = "'" + utf8(exe) + "' --version 2>/dev/null";
#endif
    FILE* p = GN_POPEN(cmd.c_str(), "r");
    if (!p) return false;
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    int rc = GN_PCLOSE(p);
    return rc == 0 && out.find(version) != std::string::npos;
}

void make_executable(const fs::path& p) {
#ifndef _WIN32
    ::chmod(p.c_str(), 0755);
#else
    (void)p;
#endif
}

// Puts `fresh` where `target` is. POSIX renames over the running binary atomically; Windows
// can't replace a running .exe, but it can rename it, so the old one moves to <name>.old
// (deleted by the next run, see update_cleanup).
void swap_in(const fs::path& fresh, const fs::path& target) {
#ifdef _WIN32
    fs::path old = target;
    old += ".old";
    std::error_code ec;
    fs::remove(old, ec);
    fs::rename(target, old);
    try {
        fs::rename(fresh, target);
    } catch (...) {
        fs::rename(old, target, ec);  // put the working one back
        throw;
    }
#else
    fs::rename(fresh, target);
#endif
}

fs::path state_file() { return config::dir() / "update-check.json"; }

}  // namespace

void update_cleanup() {
#ifdef _WIN32
    if (auto self = self_path()) {
        fs::path old = *self;
        old += ".old";
        std::error_code ec;
        fs::remove(old, ec);
    }
#endif
}

int update(const Context& ctx) {
    const char* file = platform_file();
    auto exe = self_path();
    if (!file || !exe) {
        term::error("gn update has no build for this platform; get one from https://rsys.ganamaga.me/gn-cli/public/latest.json");
        return 1;
    }

    json latest = fetch_latest(ctx, ctx.prerelease);
    std::string version = str(latest, "version");
    int order = compare_versions(version, GN_CLI_VERSION);
    if (order <= 0 && !ctx.force) {
        term::success(std::string("gn-cli ") + GN_CLI_VERSION + " is up to date" +
                      (order < 0 ? " " + term::dim("(newer than the latest release, " + version + ")") : "."));
        return 0;
    }

    std::cout << "gn-cli " << GN_CLI_VERSION << ' ' << term::g().arrow << ' ' << term::bold(term::cyan(version))
              << (order > 0 ? "" : " " + term::dim("(--force)")) << '\n';
    for (const auto& r : latest.value("releases", json::array())) {
        std::string title = str(r, "title");
        std::cout << "  " << term::cyan(term::g().bullet) << ' ' << term::bold(str(r, "version"))
                  << (title.empty() || title == "gn-cli " + str(r, "version") ? "" : "  " + title)
                  << (r.value("prerelease", false) ? " " + term::magenta("(prerelease)") : "") << '\n';
    }
    if (auto url = str(latest, "url"); !url.empty()) std::cout << "  " << term::dim("Changelog: " + url) << '\n';

    if (ctx.check) return order > 0 ? 10 : 0;  // scripts: 10 means an update is available

    if (installed_by_homebrew(*exe)) {
        term::info("This gn-cli came from Homebrew; update it with " + term::bold("brew upgrade gn-cli") + ".");
        return 1;
    }

    json asset;
    for (const auto& f : latest.value("files", json::array()))
        if (str(f, "filename") == file) asset = f;
    if (asset.is_null()) {
        term::error("gn-cli " + version + " has no " + file + ".");
        return 1;
    }

    fs::path target = *exe;
    fs::path fresh = target;
    fresh += ".gn-new";
    try {
        fetch_into(fresh, file, static_cast<std::uint64_t>(num(asset, "size")), str(asset, "sha256"),
                   [&](const http::Sink& sink, const http::Progress& progress) {
                       api::Client(rsys_base(ctx), api::default_user_agent()).download_public(str(asset, "url"), sink, progress);
                   });
    } catch (const fs::filesystem_error& e) {
        term::error(std::string("Can't write next to ") + utf8(target) + ": " + e.code().message() + ".");
#ifndef _WIN32
        std::cerr << "  Run it as the owner of that folder, e.g. " << term::bold("sudo gn update") << ".\n";
#endif
        return 1;
    }
    make_executable(fresh);
    if (!runs_as(fresh, version)) {
        std::error_code ec;
        fs::remove(fresh, ec);
        term::error("The downloaded gn-cli " + version + " doesn't run here; keeping " + GN_CLI_VERSION + ".");
        return 1;
    }

    try {
        swap_in(fresh, target);
    } catch (const fs::filesystem_error& e) {
        std::error_code ec;
        fs::remove(fresh, ec);
        term::error("Couldn't replace " + utf8(target) + ": " + e.code().message() + ".");
        return 1;
    }

    // Aliases that are copies rather than symlinks (Windows without developer mode) need the new
    // binary too; symlinks already point at it.
    for (const char* alias : {"gn", "gncli"}) {
        fs::path a = target.parent_path() / (alias + target.extension().string());
        std::error_code ec;
        if (a == target || fs::is_symlink(a, ec) || !fs::is_regular_file(a, ec)) continue;
        fs::path copy = a;
        copy += ".gn-new";
        fs::copy_file(target, copy, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            make_executable(copy);
            try {
                swap_in(copy, a);
            } catch (const fs::filesystem_error&) {
                term::warn("Couldn't update the alias " + utf8(a) + "; it still runs " + GN_CLI_VERSION + ".");
            }
        }
    }

    // A fresh check result, so the shell doesn't announce the version just installed.
    std::ofstream(state_file()) << json{{"checked", std::time(nullptr)}, {"latest", version}}.dump();
    term::success("Updated gn-cli " + std::string(GN_CLI_VERSION) + " " + term::g().arrow + " " + term::bold(version) +
                  " " + term::dim("(" + utf8(target) + ")"));
    return 0;
}

std::string update_notice(const Context& ctx) {
    const char* off = std::getenv("GN_NO_UPDATE_CHECK");
    if (off && *off && std::string_view(off) != "0") return "";
    json state = json::object();
    {
        std::ifstream in(state_file());
        if (in) state = json::parse(in, nullptr, false);
        if (!state.is_object()) state = json::object();
    }
    long long checked = state.value("checked", 0LL);
    long long now = static_cast<long long>(std::time(nullptr));
    if (now - checked > kCheckEverySeconds) {
        // Once a day, quickly, and never in the way: any failure just waits for tomorrow.
        try {
            json latest = fetch_latest(ctx, false, 3);
            state["latest"] = str(latest, "version");
        } catch (const std::exception&) {}
        state["checked"] = now;
        std::error_code ec;
        fs::create_directories(config::dir(), ec);
        std::ofstream(state_file()) << state.dump();
    }
    std::string latest = state.value("latest", "");
    if (latest.empty() || compare_versions(latest, GN_CLI_VERSION) <= 0) return "";
    auto exe = self_path();
    std::string how = exe && installed_by_homebrew(*exe) ? "brew upgrade gn-cli" : "gn update";
    return "gn-cli " + latest + " is out (you have " + GN_CLI_VERSION + "); run " + term::bold(how) + ".";
}

}  // namespace commands

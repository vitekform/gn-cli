// gn s3 share / shares / unshare (FileShareAPI) and gn share get (PublicShareAPI).
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kShares = "/api/v2/s3/shares";
const std::string kPublicShare = "/api/v2/share";
constexpr long long kDefaultValidity = 86400;  // what the web app preselects (share-dialogs.ts)
constexpr long long kMinValidity = 5 * 60, kMaxValidity = 30 * 86400;  // FileShareService

// The page a recipient opens, as the web app links it (features/s3/share-api.ts).
std::string share_link(const std::string& base, const std::string& token) { return base + "/share/" + token; }

// "90m", "12h", "7d", "2w" (or plain seconds) → seconds; nullopt when it doesn't parse.
std::optional<long long> parse_duration(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t digits = text.find_first_not_of("0123456789");
    if (digits == 0) return std::nullopt;
    long long n = 0;
    try {
        n = std::stoll(text.substr(0, digits));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    std::string unit = digits == std::string::npos ? "s" : text.substr(digits);
    if (unit == "s") return n;
    if (unit == "m" || unit == "min") return n * 60;
    if (unit == "h") return n * 3600;
    if (unit == "d") return n * 86400;
    if (unit == "w") return n * 7 * 86400;
    return std::nullopt;
}

// A share link (https://ganamaga.me/share/<token>, …/api/v2/share/<token>/…) or a bare token.
std::pair<std::string, std::string> parse_share_link(const std::string& arg) {
    std::string s = arg;
    if (auto q = s.find_first_of("?#"); q != std::string::npos) s.erase(q);
    while (!s.empty() && s.back() == '/') s.pop_back();
    std::string base;
    if (auto scheme = s.find("://"); scheme != std::string::npos) {
        auto path = s.find('/', scheme + 3);
        base = s.substr(0, path);
        s = path == std::string::npos ? "" : s.substr(path);
    }
    for (const char* marker : {"/api/v2/share/", "/share/"}) {
        if (auto at = s.find(marker); at != std::string::npos) {
            s = s.substr(at + std::string(marker).size());
            break;
        }
    }
    if (auto slash = s.find('/'); slash != std::string::npos) s.erase(slash);
    return {base, s};
}

// What the server calls the zip (FileShareService.zipName): the folder's name when exactly one
// folder was shared, else shared-files.zip.
std::string zip_name(const json& files) {
    std::string common;
    for (const auto& f : files) {
        std::string name = str(f, "name");
        auto slash = name.find('/');
        if (slash == std::string::npos || (!common.empty() && common != name.substr(0, slash))) return "shared-files.zip";
        common = name.substr(0, slash);
    }
    return common.empty() ? "shared-files.zip" : common + ".zip";
}

void print_share(const std::string& base, const json& s) {
    auto files = s.value("files", json::array());
    std::string what = files.size() == 1 ? str(files[0], "name") : std::to_string(files.size()) + " files";
    std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(s, "id"))), 4) << "  "
              << pad_right(truncate(what, 30), 31) << pad_left(human_size(static_cast<std::uint64_t>(num(s, "totalSize"))), 10)
              << "  " << pad_left(std::to_string(static_cast<long long>(num(s, "downloadCount"))), 4) << term::dim(" dl")
              << "  until " << format_time(s["expiresAt"]) << '\n'
              << term::dim(term::g().vbar) << "        " << term::cyan(share_link(base, str(s, "token"))) << '\n';
}

}  // namespace

int s3_share(const Context& ctx) {
    if (!need_args(ctx, 3, "s3 share <provider> <bucket> <path>... [--expires 24h]")) return 2;
    long long validity = kDefaultValidity;
    if (!ctx.expires.empty()) {
        auto parsed = parse_duration(ctx.expires);
        if (!parsed || *parsed < kMinValidity || *parsed > kMaxValidity) {
            term::error("--expires takes 5m to 30d, like 90m, 12h, 7d or 2w.");
            return 2;
        }
        validity = *parsed;
    }
    // Folders end in /, as the server expects; leading slashes go, like everywhere else in s3.
    json keys = json::array();
    for (std::size_t i = 2; i < ctx.args.size(); ++i) {
        std::string k = ctx.args[i];
        k.erase(0, k.find_first_not_of('/'));
        if (k.empty()) {
            term::error("Sharing a whole bucket isn't possible; name a folder or files.");
            return 2;
        }
        if (ctx.recursive && k.back() != '/') k += '/';
        keys.push_back(k);
    }

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json body = {{"provider", ctx.args[0]}, {"bucket", ctx.args[1]}, {"keys", keys}, {"validitySeconds", validity}};
    json s = auth->client.call("POST", kShares, auth->session(), &body);
    std::string url = share_link(auth->client.base_url(), str(s, "token"));
    if (ctx.json) {
        s["url"] = url;
        std::cout << s.dump(2) << '\n';
    } else if (!term::stdout_is_tty()) {
        std::cout << url << '\n';
    } else {
        auto files = s.value("files", json::array());
        term::success("Shared " + std::to_string(files.size()) + (files.size() == 1 ? " file " : " files ") +
                      term::dim("(" + human_size(static_cast<std::uint64_t>(num(s, "totalSize"))) + ")"));
        std::cout << "  " << term::cyan(url) << '\n'
                  << "  " << term::dim("Works until " + format_time(s["expiresAt"]) + "; revoke it with gn s3 unshare " +
                                       std::to_string(static_cast<long long>(num(s, "id")))) << '\n';
    }
    return 0;
}

int s3_shares(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json shares = auth->client.get_authed(kShares, auth->session());
    if (ctx.json) {
        for (auto& s : shares) s["url"] = share_link(auth->client.base_url(), str(s, "token"));
        std::cout << shares.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Share links (" + std::to_string(shares.size()) + ")");
    if (shares.empty()) term::row("", term::dim("none; create one with gn s3 share <provider> <bucket> <path>"), 0);
    for (const auto& s : shares) print_share(auth->client.base_url(), s);
    term::section_end();
    return 0;
}

int s3_unshare(const Context& ctx) {
    if (!need_args(ctx, 1, "s3 unshare <id|link>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    std::optional<json> listing;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        std::string id = arg;
        if (arg.empty() || arg.find_first_not_of("0123456789") != std::string::npos) {
            std::string token = parse_share_link(arg).second;
            if (!listing) listing = auth->client.get_authed(kShares, auth->session());
            id.clear();
            for (const auto& s : *listing)
                if (str(s, "token") == token) id = std::to_string(static_cast<long long>(num(s, "id")));
            if (id.empty()) {
                term::error("None of your share links is " + arg);
                rc = 1;
                continue;
            }
        }
        auth->client.call("DELETE", kShares + "/" + id, auth->session());
        term::success("Revoked share " + id + "; its link no longer works.");
    }
    return rc;
}

int share_get(const Context& ctx) {
    if (!need_args(ctx, 1, "share get <link> [-o <dir>] [--zip]")) return 2;
    auto [base, token] = parse_share_link(ctx.args[0]);
    if (token.empty()) {
        term::error("No token in " + ctx.args[0]);
        return 2;
    }
    if (base.empty()) base = resolve_url(ctx, nullptr);
    api::Client pub(base, api::default_user_agent());
    json info = pub.get_public(kPublicShare + "/" + token);  // 404 unknown, 410 expired
    auto files = info.value("files", json::array());
    if (ctx.json) {
        std::cout << info.dump(2) << '\n';
        return 0;
    }
    std::string prefix = base + kPublicShare + "/" + token;
    std::error_code ec;
    auto refuse_existing = [&](const fs::path& p) {
        if (p.empty() || ctx.force || !fs::exists(p, ec)) return false;
        term::error(utf8(p) + " already exists; pass --force to overwrite it.");
        return true;
    };

    // One file, or everything as a zip: a single download, -o naming it.
    if (ctx.zip || files.size() == 1) {
        std::string name = ctx.zip ? zip_name(files) : base_name(str(files[0], "name"));
        fs::path dest = download_target(ctx, name);
        if (refuse_existing(dest)) return 1;
        std::uint64_t size = ctx.zip ? 0 : static_cast<std::uint64_t>(num(files[0], "size"));
        std::string url = prefix + (ctx.zip ? "/zip" : "/files/0");
        fetch_into(dest, name, size, "", [&](const http::Sink& sink, const http::Progress& progress) {
            pub.download_public(url, sink, progress);
        });
        if (!dest.empty()) term::success("Downloaded " + term::bold(name) + " " + term::dim("→ " + utf8(dest)));
        return 0;
    }

    // Several files: into -o (a folder) or the current one, keeping the shared folder structure.
    if (ctx.output == "-") {
        term::error("This share has " + std::to_string(files.size()) + " files; pass --zip to stream them as one zip.");
        return 2;
    }
    fs::path dir = ctx.output.empty() ? fs::path(".") : from_utf8(ctx.output);
    struct Item {
        int index;
        std::string name;
        std::uint64_t size;
        fs::path dest;
    };
    std::vector<Item> plan;
    for (const auto& f : files) {
        auto rel = safe_local_path(str(f, "name"));
        if (!rel) {
            term::warn("Skipping " + str(f, "name") + ": not a safe local path.");
            continue;
        }
        fs::path dest = dir / *rel;
        if (refuse_existing(dest)) return 1;
        plan.push_back({static_cast<int>(num(f, "index")), str(f, "name"), static_cast<std::uint64_t>(num(f, "size")), dest});
    }
    std::uint64_t total = 0;
    for (const auto& item : plan) {
        fetch_into(item.dest, item.name, item.size, "", [&](const http::Sink& sink, const http::Progress& progress) {
            pub.download_public(prefix + "/files/" + std::to_string(item.index), sink, progress);
        });
        total += item.size;
    }
    term::success("Downloaded " + std::to_string(plan.size()) + " files " +
                  term::dim("(" + human_size(total) + ") → " + utf8(dir)));
    return 0;
}

}  // namespace commands

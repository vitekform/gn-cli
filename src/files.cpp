// gn files: persistent files with a public link that lasts until deleted (PersistentFileAPI,
// PublicPersistentFileAPI). Uploading needs level 2; downloading needs nothing.
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kFiles = "/api/v2/persistent-files";
const std::string kPublicFiles = "/api/v2/pf";

// The page a recipient opens, as the web app links it (features/files/persistent-files-api.ts).
std::string public_link(const std::string& base, const std::string& token) { return base + "/p/" + token; }

// A link (https://ganamaga.me/p/<token>, …/api/v2/pf/<token>…) or a bare token, split into the
// server it lives on (empty for a bare token) and the token.
std::pair<std::string, std::string> parse_link(const std::string& arg) {
    std::string s = arg;
    if (auto q = s.find_first_of("?#"); q != std::string::npos) s.erase(q);
    while (!s.empty() && s.back() == '/') s.pop_back();
    std::string base;
    if (auto scheme = s.find("://"); scheme != std::string::npos) {
        auto path = s.find('/', scheme + 3);
        base = s.substr(0, path);
        s = path == std::string::npos ? "" : s.substr(path);
    }
    for (const char* marker : {"/p/", "/api/v2/pf/"}) {
        if (auto at = s.find(marker); at != std::string::npos) {
            s = s.substr(at + std::string(marker).size());
            break;
        }
    }
    if (auto slash = s.find('/'); slash != std::string::npos) s.erase(slash);  // …/<token>/download
    return {base, s};
}

std::optional<long long> parse_id(const std::string& s) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::stoll(s);
}

}  // namespace

int files_put(const Context& ctx) {
    if (!need_args(ctx, 1, "files <file>...")) return 2;

    std::vector<fs::path> files;
    for (const auto& arg : ctx.args) {
        fs::path p = from_utf8(arg);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) {
            term::error("Not a file: " + arg);
            return 1;
        }
        files.push_back(p);
    }

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;

    // Limits need files:read; a CLI token without it still uploads, just without the early check.
    if (std::optional<json> limits = a.client.try_get_authed(kFiles + "/limits", a.session())) {
        std::uint64_t cap = static_cast<std::uint64_t>(num(*limits, "absoluteMaxSize"));
        if (auto m = limits->find("maxSize"); m != limits->end() && m->is_number())
            cap = std::min(cap, m->get<std::uint64_t>());
        for (const auto& f : files)
            if (cap > 0 && fs::file_size(f) > cap) {
                term::error(utf8(f.filename()) + " is " + human_size(fs::file_size(f)) + "; you may upload up to " +
                            human_size(cap) + ".");
                return 1;
            }
    }

    bool plain = !term::stdout_is_tty() && !ctx.json;  // `url=$(gn files f)` gets just the link
    json results = json::array();
    for (const auto& path : files) {
        std::string name = utf8(path.filename());
        std::uint64_t size = fs::file_size(path);
        json body = {{"filename", name}, {"size", size}, {"contentType", guess_content_type(name)}};
        json created = a.client.call("POST", kFiles, a.session(), &body);
        std::string id = std::to_string(static_cast<long long>(num(created, "id")));
        auto part_size = static_cast<std::uint64_t>(num(created, "partSize"));
        auto parts = static_cast<std::uint64_t>(num(created, "partCount"));
        if (part_size == 0 || parts == 0) throw std::runtime_error("the server didn't start the upload");

        json done;
        try {
            ProgressBar bar(term::unicode() ? "↑" : "^", name, size, show_progress());
            upload_parts(a, path, name, size, part_size, parts,
                         [&](std::uint64_t n) { return kFiles + "/" + id + "/parts/" + std::to_string(n); }, bar);
            bar.note("finishing");
            with_retries("Finishing " + name, [&] { done = a.client.post_form(kFiles + "/" + id + "/complete", {}, a.session(), 0); });
        } catch (...) {
            // Don't leave a half upload behind; the server would only clean it up a day later.
            try {
                a.client.call("DELETE", kFiles + "/" + id, a.session());
            } catch (const std::exception&) {}
            throw;
        }

        std::string url = public_link(a.client.base_url(), str(done, "token"));
        done["url"] = url;
        results.push_back(done);
        if (plain) {
            std::cout << url << '\n';
        } else if (!ctx.json) {
            term::success(term::bold(name) + " " + term::dim("(" + human_size(size) + ")"));
            std::cout << "  " << term::cyan(url) << '\n'
                      << "  " << term::dim("Works until you delete it (gn files rm " + id + ")") << '\n';
        }
    }
    if (ctx.json) std::cout << (results.size() == 1 ? results[0] : results).dump(2) << '\n';
    return 0;
}

int files_list(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json files = auth->client.get_authed(kFiles, auth->session());
    if (ctx.json) {
        for (auto& f : files) f["url"] = public_link(auth->client.base_url(), str(f, "token"));
        std::cout << files.dump(2) << '\n';
        return 0;
    }
    const auto& G = term::g();
    term::section_begin("Persistent files (" + std::to_string(files.size()) + ")");
    if (files.empty()) term::row("", term::dim("none; upload one with gn files <file>"), 0);
    for (const auto& f : files) {
        bool ready = str(f, "status") == "READY";
        std::string state = ready ? "since " + format_time(f["completedAt"]) : term::yellow("uploading");
        std::cout << term::dim(G.vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(f, "id"))), 5) << "  "
                  << pad_right(truncate(str(f, "filename"), 30), 31) << pad_left(human_size(static_cast<std::uint64_t>(num(f, "size"))), 10)
                  << "  " << pad_left(std::to_string(static_cast<long long>(num(f, "downloadCount"))), 5) << term::dim(" dl")
                  << "  " << state << '\n';
        if (ready)
            std::cout << term::dim(G.vbar) << "         " << term::cyan(public_link(auth->client.base_url(), str(f, "token")))
                      << '\n';
    }
    term::section_end();
    return 0;
}

int files_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "files rm <id|link>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;
    std::optional<json> listing;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        std::optional<long long> id = parse_id(arg);
        if (!id) {
            // A link or token: find its id among our files.
            std::string token = parse_link(arg).second;
            if (!listing) listing = a.client.get_authed(kFiles, a.session());
            for (const auto& f : *listing)
                if (str(f, "token") == token) id = static_cast<long long>(num(f, "id"));
            if (!id) {
                term::error("None of your persistent files has the link " + arg);
                rc = 1;
                continue;
            }
        }
        a.client.call("DELETE", kFiles + "/" + std::to_string(*id), a.session());
        term::success("Deleted persistent file " + std::to_string(*id) + "; its link no longer works.");
    }
    return rc;
}

int files_limits(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json l = auth->client.get_authed(kFiles + "/limits", auth->session());  // 403 below level 2
    if (ctx.json) {
        std::cout << l.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Persistent file limits");
    auto max = l.find("maxSize");
    term::row("Largest file", max != l.end() && max->is_number() ? human_size(max->get<std::uint64_t>())
                                                                 : human_size(static_cast<std::uint64_t>(num(l, "absoluteMaxSize"))));
    term::row("Your level", std::to_string(static_cast<int>(num(l, "level"))));
    term::section_end();
    return 0;
}

int files_get(const Context& ctx) {
    if (!need_args(ctx, 1, "files get <link> [-o <path>]")) return 2;
    auto [base, token] = parse_link(ctx.args[0]);
    if (token.empty()) {
        term::error("No token in " + ctx.args[0]);
        return 2;
    }
    if (base.empty()) base = resolve_url(ctx, nullptr);
    api::Client pub(base, api::default_user_agent());
    json info = pub.get_public(kPublicFiles + "/" + token);  // 404 unknown or deleted
    std::string name = str(info, "filename", token);

    if (ctx.json) {
        std::cout << info.dump(2) << '\n';
        return 0;
    }
    fs::path dest = download_target(ctx, name);
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    fetch_into(dest, name, static_cast<std::uint64_t>(num(info, "size")), "",
               [&](const http::Sink& sink, const http::Progress& progress) {
                   pub.download_public(base + kPublicFiles + "/" + token + "/download", sink, progress);
               });
    if (!dest.empty())
        term::success("Downloaded " + term::bold(name) + " " + term::dim("→ " + utf8(dest) + " (" +
                      human_size(static_cast<std::uint64_t>(num(info, "size"))) + ")"));
    return 0;
}

}  // namespace commands

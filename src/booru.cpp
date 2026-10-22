// gn booru (GelbooruAPI: the mirrored image board) and gn comics (R34ComicsAPI: the crawled
// comics), both behind the gelbooru permission. Searching and downloading; uploads and the live
// rule34 search stay in the web app.
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kBooru = "/api/v2/gelbooru";
const std::string kComics = "/api/v2/comics";
// Where the booru's originals are kept (S3ReadAccess lets gelbooru users read them).
const char* kBooruProvider = "Custom MinIO";
const char* kBooruBucket = "gelbooru";

std::string count_of(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

std::string id_of(const json& o) { return std::to_string(static_cast<long long>(num(o, "id"))); }

int limit_of(const Context& ctx, int fallback, int max) {
    if (ctx.limit.empty()) return fallback;
    try {
        return std::max(1, std::min(max, std::stoi(ctx.limit)));
    } catch (const std::exception&) {
        return fallback;
    }
}

int page_of(const Context& ctx) {
    try {
        return ctx.page.empty() ? 1 : std::max(1, std::stoi(ctx.page));
    } catch (const std::exception&) {
        return 1;
    }
}

std::string extension_of(const std::string& key) {
    auto dot = key.rfind('.');
    auto slash = key.rfind('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : key.substr(dot);
}

// Saves one booru post's original as <dir>/<id>.<ext>; false when it has none or it's already there.
bool save_post(Auth& a, const json& post, const fs::path& dir, bool force) {
    std::string key = str(post, "r2Key");
    if (key.empty()) {
        term::warn("Post " + id_of(post) + " has no stored original.");
        return false;
    }
    fs::path dest = dir / (id_of(post) + extension_of(key));
    std::error_code ec;
    if (fs::exists(dest, ec) && !force) return false;
    http::Form q{{"key", key}, {"download", "true"}};
    fetch_into(dest, utf8(dest.filename()), 0, "", [&](const http::Sink& sink, const http::Progress& progress) {
        a.client.download(storage_object_path(kBooruProvider, kBooruBucket), q, a.session(), sink, progress);
    });
    return true;
}

json search_posts(Auth& a, const std::string& tags, bool no_ai, int page, int size) {
    http::Form q{{"tags", tags}, {"page", std::to_string(page - 1)}, {"size", std::to_string(size)}};
    if (no_ai) q.emplace_back("excludeAiGenerated", "true");
    return a.client.get_authed(kBooru + "/posts?" + http::encode_query(q), a.session());
}

fs::path output_dir(const Context& ctx, const std::string& fallback) {
    fs::path dir = ctx.output.empty() ? from_utf8(fallback) : from_utf8(ctx.output);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

// ── booru ───────────────────────────────────────────────────────────────────

int booru_search(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int page = page_of(ctx);
    json r = search_posts(*auth, join(ctx.args, " "), ctx.no_ai, page, limit_of(ctx, 20, 100));
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    auto posts = r.value("content", json::array());
    term::section_begin("Posts " + term::dim("(page " + std::to_string(page) + " of " +
                                             std::to_string(static_cast<long long>(num(r, "totalPages"))) + ", " +
                                             with_thousands(static_cast<long long>(num(r, "totalElements"))) + " in all)"));
    if (posts.empty()) term::row("", term::dim("nothing matches; gn booru tags <prefix> suggests tags"), 0);
    for (const auto& p : posts)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(p), 7) << "  "
                  << pad_right(std::to_string(static_cast<int>(num(p, "width"))) + "x" + std::to_string(static_cast<int>(num(p, "height"))), 11)
                  << pad_right(str(p, "mediaType", "image"), 7) << pad_right(str(p, "rating"), 13) << term::dim(truncate(str(p, "tags"), 46))
                  << '\n';
    term::section_end();
    std::cout << term::dim("  gn booru get <id>... downloads; gn booru dl <tags> --limit N takes a whole search.") << '\n';
    return 0;
}

int booru_tags(const Context& ctx) {
    if (!need_args(ctx, 1, "booru tags <prefix>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json tags = auth->client.get_authed(kBooru + "/tag-suggestions?" + http::encode_query({{"q", ctx.args[0]}, {"limit", "25"}}),
                                        auth->session());
    if (ctx.json) {
        std::cout << tags.dump(2) << '\n';
        return 0;
    }
    for (const auto& t : tags)
        std::cout << pad_right(str(t, "tag"), 40) << term::dim(with_thousands(static_cast<long long>(num(t, "count")))) << '\n';
    return 0;
}

int booru_show(const Context& ctx) {
    if (!need_args(ctx, 1, "booru show <id>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json p = auth->client.get_authed(kBooru + "/posts/" + ctx.args[0], auth->session());
    if (ctx.json) {
        std::cout << p.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Post " + id_of(p));
    term::row("Size", std::to_string(static_cast<int>(num(p, "width"))) + "x" + std::to_string(static_cast<int>(num(p, "height"))) + " " +
                          str(p, "mediaType", "image"));
    if (auto v = str(p, "rating"); !v.empty()) term::row("Rating", v);
    if (auto v = str(p, "creator"); !v.empty()) term::row("Creator", v);
    if (auto v = str(p, "source"); !v.empty()) term::row("Source", term::cyan(v));
    if (p.contains("rule34Id") && p["rule34Id"].is_number())
        term::row("rule34", term::cyan("https://rule34.xxx/index.php?page=post&s=view&id=" + std::to_string(p["rule34Id"].get<long long>())));
    if (p.contains("createdAt")) term::row("Added", format_time(p["createdAt"]));
    term::row("Tags", str(p, "tags"));
    term::section_end();
    return 0;
}

int booru_get(const Context& ctx) {
    if (!need_args(ctx, 1, "booru get <id>... [-o <dir>]")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    fs::path dir = output_dir(ctx, ".");
    int saved = 0;
    for (const auto& id : ctx.args) {
        json p = auth->client.get_authed(kBooru + "/posts/" + id, auth->session());
        if (save_post(*auth, p, dir, ctx.force)) ++saved;
    }
    term::success("Downloaded " + count_of(saved, "post", "posts") + " " + term::dim("→ " + utf8(dir)));
    return 0;
}

// Every post of a search (up to --limit, default 50) into -o (default: a folder named after the tags).
int booru_download(const Context& ctx) {
    if (!need_args(ctx, 1, "booru dl <tags> [--limit N] [-o <dir>]")) return 2;
    std::string tags = join(ctx.args, " ");
    int want = limit_of(ctx, 50, 5000);
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    std::string folder = tags;
    for (char& c : folder)
        if (c == ' ' || c == '/' || c == '\\' || c == ':') c = '_';
    fs::path dir = output_dir(ctx, folder);
    int saved = 0, skipped = 0, seen = 0;
    CtrlC ctrl_c;
    for (int page = 1; seen < want && !ctrl_c.pressed(); ++page) {
        json r = search_posts(*auth, tags, ctx.no_ai, page, std::min(100, want - seen));
        auto posts = r.value("content", json::array());
        if (posts.empty()) break;
        for (const auto& p : posts) {
            if (seen++ >= want || ctrl_c.pressed()) break;
            if (save_post(*auth, p, dir, ctx.force)) ++saved;
            else ++skipped;
        }
        if (page >= static_cast<int>(num(r, "totalPages"))) break;
    }
    term::success("Downloaded " + count_of(saved, "post", "posts") + " " + term::dim("→ " + utf8(dir)) +
                  (skipped ? term::dim(", " + std::to_string(skipped) + " already there or without a file") : "") +
                  (ctrl_c.pressed() ? term::dim(" (stopped)") : ""));
    return 0;
}

// ── comics ──────────────────────────────────────────────────────────────────

int comics_search(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int page = page_of(ctx);
    http::Form q{{"q", join(ctx.args, " ")}, {"page", std::to_string(page)}, {"per", std::to_string(limit_of(ctx, 30, 200))}};
    if (!ctx.tag.empty()) q.emplace_back("tag", ctx.tag);
    if (!ctx.author.empty()) q.emplace_back("author", ctx.author);
    json r = auth->client.get_authed(kComics + "?" + http::encode_query(q), auth->session());
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Comics " + term::dim("(page " + std::to_string(page) + " of " + std::to_string(static_cast<long long>(num(r, "pages"))) +
                                              ", " + with_thousands(static_cast<long long>(num(r, "total"))) + " in all)"));
    for (const auto& c : r.value("comics", json::array()))
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(truncate(str(c, "uid"), 22), 24) << pad_right(truncate(str(c, "title"), 40), 42)
                  << pad_left(std::to_string(static_cast<int>(num(c, "num_images"))), 4) << term::dim(" pages  " + str(c, "category")) << '\n';
    term::section_end();
    return 0;
}

int comics_show(const Context& ctx) {
    if (!need_args(ctx, 1, "comics show <uid>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json c = auth->client.get_authed(kComics + "/" + ctx.args[0], auth->session());
    if (ctx.json) {
        std::cout << c.dump(2) << '\n';
        return 0;
    }
    auto images = c.value("images", json::array());
    int mirrored = 0;
    for (const auto& i : images) mirrored += i.value("mirrored", false) ? 1 : 0;
    auto names = [&](const char* key) {
        std::vector<std::string> out;
        for (const auto& x : c.value(key, json::array())) out.push_back(x.is_string() ? x.get<std::string>() : str(x, "name", str(x, "slug")));
        return join(out, ", ");
    };
    term::section_begin(str(c, "title", ctx.args[0]));
    term::row("Id", str(c, "uid"));
    if (auto v = names("authors"); !v.empty()) term::row("Authors", v);
    if (auto v = names("tags"); !v.empty()) term::row("Tags", v);
    if (auto v = names("characters"); !v.empty()) term::row("Characters", v);
    term::row("Pages", std::to_string(images.size()) + (mirrored < static_cast<int>(images.size())
                                                            ? term::dim(" (" + std::to_string(mirrored) + " mirrored so far)") : ""));
    term::section_end();
    return 0;
}

// Every mirrored page into <dir>/<NNN>.<ext>; pages already there (same size) are skipped, so a
// stopped download carries on.
int comics_get(const Context& ctx) {
    if (!need_args(ctx, 1, "comics get <uid> [-o <dir>]")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json c = auth->client.get_authed(kComics + "/" + ctx.args[0], auth->session());
    std::string folder = str(c, "slug", str(c, "uid", ctx.args[0]));
    fs::path dir = output_dir(ctx, folder);
    auto images = c.value("images", json::array());
    int saved = 0, missing = 0, had = 0;
    CtrlC ctrl_c;
    for (const auto& img : images) {
        if (ctrl_c.pressed()) break;
        if (!img.value("mirrored", false) || str(img, "key").empty()) {
            ++missing;
            continue;
        }
        char name[16];
        std::snprintf(name, sizeof name, "%03d", static_cast<int>(num(img, "idx")));
        fs::path dest = dir / (std::string(name) + extension_of(str(img, "key")));
        std::error_code ec;
        auto size = static_cast<std::uintmax_t>(num(img, "size"));
        if (!ctx.force && fs::exists(dest, ec) && (size == 0 || fs::file_size(dest, ec) == size)) {
            ++had;
            continue;
        }
        http::Form q{{"bucket", str(img, "bucket")}, {"key", str(img, "key")}, {"download", "true"}};
        fetch_into(dest, str(c, "title", folder) + " " + name, size, str(img, "sha256"), [&](const http::Sink& sink, const http::Progress& progress) {
            auth->client.download(kComics + "/image", q, auth->session(), sink, progress);
        });
        ++saved;
    }
    term::success("Downloaded " + count_of(saved, "page", "pages") + " " + term::dim("→ " + utf8(dir)) +
                  (had ? term::dim(", " + std::to_string(had) + " already there") : "") +
                  (missing ? term::dim(", " + std::to_string(missing) + " not mirrored yet") : "") +
                  (ctrl_c.pressed() ? term::dim(" (stopped)") : ""));
    return 0;
}

namespace {
int comics_facet(const Context& ctx, const char* what) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form q{{"q", join(ctx.args, " ")}, {"page", std::to_string(page_of(ctx))}, {"per", std::to_string(limit_of(ctx, 40, 200))}};
    json r = auth->client.get_authed(kComics + "/" + what + "?" + http::encode_query(q), auth->session());
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    for (const auto& x : r.value(what, json::array()))
        std::cout << pad_right(str(x, "name"), 36) << pad_right(term::dim(str(x, "slug")), 30)
                  << term::dim(with_thousands(static_cast<long long>(num(x, "count")))) << '\n';
    std::cout << term::dim(std::string("  Filter with gn comics search --") + (std::string(what) == "tags" ? "tag" : "author") + " <slug>.") << '\n';
    return 0;
}
}  // namespace

int comics_tags(const Context& ctx) { return comics_facet(ctx, "tags"); }
int comics_authors(const Context& ctx) { return comics_facet(ctx, "authors"); }

}  // namespace commands

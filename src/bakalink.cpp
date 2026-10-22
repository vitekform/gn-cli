// gn bakalink: BakaLink's store of what Bakaláři accounts receive — Komens messages, marks and
// timetables (bakalink/controllers) — the way the Bakaláři integration feeds and reads it.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kBase = "/api/v2/bakalink";

std::string base64(const std::string& in) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16 | static_cast<unsigned char>(in[i + 1]) << 8 |
                     static_cast<unsigned char>(in[i + 2]);
        out += abc[n >> 18 & 63];
        out += abc[n >> 12 & 63];
        out += abc[n >> 6 & 63];
        out += abc[n & 63];
    }
    if (i < in.size()) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) n |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += abc[n >> 18 & 63];
        out += abc[n >> 12 & 63];
        out += i + 1 < in.size() ? abc[n >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + utf8(p));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// --file <path>, or - for stdin.
std::string read_input(const std::string& file) {
    if (file == "-") return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    return read_file(from_utf8(file));
}

std::string read_content(const Context& ctx) {
    return ctx.content_file.empty() ? ctx.content : read_input(ctx.content_file);
}

std::string now_iso() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// A boolean the server may leave null.
bool flag(const json& obj, const char* key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() && it->get<bool>();
}

std::string id_of(const json& o) { return std::to_string(static_cast<long long>(num(o, "id"))); }

// "Mgr. Novák" from {"bakaId", "name", "abbrev"}, falling back to whichever is there.
std::string ref_name(const json& obj, const char* key, bool abbrev = false) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return "";
    std::string a = str(*it, "abbrev"), n = str(*it, "name");
    if (abbrev && !a.empty()) return a;
    return !n.empty() ? n : !a.empty() ? a : str(*it, "bakaId");
}

std::string strip_html(const std::string& html) {
    std::string out;
    bool tag = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') {
            tag = true;
            if (html.compare(i, 3, "<br") == 0 || html.compare(i, 3, "</p") == 0 || html.compare(i, 5, "</div") == 0) out += '\n';
        } else if (c == '>') tag = false;
        else if (!tag) out += c;
    }
    return trim(out);
}

// The BakaLink token and, when there is one, the CLI token; no login is involved.
std::optional<api::Client> open(const Context& ctx) {
    std::string bk_token = ctx.bakalink_token;
    if (bk_token.empty())
        if (const char* env = std::getenv("GN_BAKALINK_TOKEN"); env && *env) bk_token = env;
    if (bk_token.empty()) {
        term::error("No BakaLink token given.");
        std::cerr << "  Pass " << term::bold("--bakalink-token <token>") << " or set " << term::bold("GN_BAKALINK_TOKEN")
                  << " (the server's BAKALINK_TOKEN).\n";
        return std::nullopt;
    }
    auto creds = config::load();
    api::Client client(resolve_url(ctx, creds ? &*creds : nullptr), api::default_user_agent());
    // A CLI token still has to carry bakalink:read or bakalink:manage.
    if (!ctx.token.empty()) client.use_cli_token(ctx.token);
    client.add_header("X-BakaLink-Token", bk_token);
    return client;
}

// --account (or $GN_BAKALINK_ACCOUNT) as an account id: given as one, or looked up by name.
std::optional<std::string> account_id(const Context& ctx, api::Client& client, std::string who = "") {
    if (who.empty()) who = ctx.account;
    if (who.empty())
        if (const char* env = std::getenv("GN_BAKALINK_ACCOUNT"); env && *env) who = env;
    if (who.empty()) {
        term::error("No account given.");
        std::cerr << "  Pass " << term::bold("--account <id|name>") << " or set " << term::bold("GN_BAKALINK_ACCOUNT")
                  << " (gn bakalink accounts lists them).\n";
        return std::nullopt;
    }
    if (all_digits(who)) return who;
    for (const auto& a : client.call("GET", kBase + "/accounts", nullptr))
        if (lower(str(a, "name")) == lower(who)) return id_of(a);
    term::error("No BakaLink account " + who + " (gn bakalink accounts lists them).");
    return std::nullopt;
}

std::optional<json> parse_input(const Context& ctx, const std::string& usage) {
    if (ctx.file.empty()) {
        term::error("No --file given.");
        std::cerr << "  Usage: gn-cli " << usage << "\n";
        return std::nullopt;
    }
    json j = json::parse(read_input(ctx.file), nullptr, false);
    if (j.is_discarded()) {
        term::error((ctx.file == "-" ? std::string("Standard input") : ctx.file) + " is not valid JSON.");
        return std::nullopt;
    }
    return j;
}

int print_json(const json& j) {
    std::cout << j.dump(2) << '\n';
    return 0;
}

// key=value arguments from `from` on, checked against the keys a command takes.
std::optional<std::map<std::string, std::string>> fields(const Context& ctx, std::size_t from,
                                                         std::initializer_list<const char*> allowed) {
    std::map<std::string, std::string> out;
    for (std::size_t i = from; i < ctx.args.size(); ++i) {
        const std::string& kv = ctx.args[i];
        auto eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        bool ok = eq != std::string::npos && std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return key == a; });
        if (!ok) {
            std::string names;
            for (const char* a : allowed) names += std::string(names.empty() ? "" : ", ") + a + "=";
            term::error("Expected " + names + " not " + kv + ".");
            return std::nullopt;
        }
        out[key] = kv.substr(eq + 1);
    }
    return out;
}

// name=, school=, user=, full-name= and class= as the API's account fields.
json account_body(const std::map<std::string, std::string>& f) {
    static const std::pair<const char*, const char*> kNames[] = {
        {"name", "name"}, {"school", "schoolUrl"}, {"user", "bakaUserId"}, {"full-name", "fullName"}, {"class", "className"}};
    json body = json::object();
    for (const auto& [key, field] : kNames)
        if (auto it = f.find(key); it != f.end()) body[field] = it->second;
    return body;
}

// {"bakaId": …} for --subject / --teacher, or nothing when the option is absent.
void put_ref(json& body, const char* key, const std::string& baka_id) {
    if (!baka_id.empty()) body[key] = {{"bakaId", baka_id}};
}

// Runs a command body, turning a refused token into the message that fits it.
template <typename F>
int guarded(const Context& ctx, F&& body) {
    try {
        return body();
    } catch (const api::Unauthorized& e) {
        term::error(ctx.token.empty() ? std::string("The server refused the BakaLink token.") : std::string("Refused: ") + e.what());
        return 1;
    }
}

// ── accounts ───────────────────────────────────────────────────────────────

int accounts_list(const Context& ctx, api::Client& client) {
    json accounts = client.call("GET", kBase + "/accounts", nullptr);
    if (ctx.json) return print_json(accounts);
    term::section_begin("BakaLink accounts (" + std::to_string(accounts.size()) + ")");
    if (accounts.empty()) term::row("", term::dim("none; gn bakalink account add name=<name> creates one"), 0);
    for (const auto& a : accounts) {
        std::string who = str(a, "fullName");
        if (!str(a, "className").empty()) who += (who.empty() ? "" : ", ") + str(a, "className");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(a), 4) << "  " << pad_right(term::bold(str(a, "name")), 20)
                  << pad_right(who, 30) << term::dim(str(a, "schoolUrl")) << '\n';
    }
    term::section_end();
    return 0;
}

int account_add(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink account add name=<name> [school=<url>] [user=<Bakaláři id>] [full-name=<name>] [class=<class>]")) return 2;
    auto f = fields(ctx, 0, {"name", "school", "user", "full-name", "class"});
    if (!f) return 2;
    if (!f->count("name")) {
        term::error("An account needs name=.");
        return 2;
    }
    json body = account_body(*f);
    json res = client.call("POST", kBase + "/accounts", nullptr, &body);
    if (ctx.json) return print_json(res);
    term::success("Created account " + term::bold(str(res, "name")) + " " + term::dim("(id " + id_of(res) + ")"));
    return 0;
}

int account_edit(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 2, "bakalink account edit <account> [name=…] [school=…] [user=…] [full-name=…] [class=…]")) return 2;
    auto f = fields(ctx, 1, {"name", "school", "user", "full-name", "class"});
    if (!f) return 2;
    auto id = account_id(ctx, client, ctx.args[0]);
    if (!id) return 1;
    json body = account_body(*f);
    json res = client.call("PATCH", kBase + "/accounts/" + *id, nullptr, &body);
    if (ctx.json) return print_json(res);
    term::success("Updated account " + term::bold(str(res, "name")));
    return 0;
}

int account_remove(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink account rm <account>")) return 2;
    auto id = account_id(ctx, client, ctx.args[0]);
    if (!id) return 1;
    if (!confirm(ctx, "Delete account " + ctx.args[0] + " with all its messages, marks and timetables?")) return 1;
    client.call("DELETE", kBase + "/accounts/" + *id, nullptr);
    if (!ctx.json) term::success("Deleted account " + ctx.args[0]);
    return 0;
}

// ── komens ─────────────────────────────────────────────────────────────────

int komens_list(const Context& ctx, api::Client& client) {
    auto id = account_id(ctx, client);
    if (!id) return 1;
    http::Form query;
    if (!ctx.since.empty()) query.emplace_back("since", ctx.since);
    if (!ctx.limit.empty()) query.emplace_back("limit", ctx.limit);
    json messages = client.call("GET", kBase + "/accounts/" + *id + "/komens" + (query.empty() ? "" : "?" + http::encode_query(query)), nullptr);
    if (ctx.json) return print_json(messages);
    term::section_begin("Komens (" + std::to_string(messages.size()) + ")");
    if (messages.empty()) term::row("", term::dim("no messages"), 0);
    for (const auto& m : messages) {
        std::size_t files = m.value("attachments", json::array()).size();
        bool unread = m.contains("read") && m["read"].is_boolean() && !m["read"].get<bool>();
        std::string title = truncate(str(m, "title", "(no title)"), 44);
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(m), 5) << "  " << pad_right(unread ? term::bold(title) : title, 46)
                  << pad_right(truncate(str(m, "senderName"), 22), 24) << term::dim(format_time(m.value("sentAt", json())))
                  << (str(m, "kind") == "NOTICEBOARD" ? term::dim("  board") : "")
                  << (files ? term::dim("  +" + std::to_string(files)) : "") << '\n';
    }
    term::section_end();
    return 0;
}

int komens_show(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink komens show <message id>")) return 2;
    json m = client.call("GET", kBase + "/komens/" + ctx.args[0], nullptr);
    if (ctx.json) return print_json(m);
    term::section_begin(str(m, "title", "(no title)"));
    term::row("From", str(m, "senderName", none()));
    term::row("Sent", format_time(m.value("sentAt", json())));
    term::row("Kind", lower(str(m, "kind")));
    term::row("Bakaláři id", str(m, "bakaId"));
    for (const auto& a : m.value("attachments", json::array()))
        term::row("Attachment " + id_of(a), str(a, "filename") + " " + term::dim("(" + human_size(static_cast<std::uint64_t>(num(a, "size"))) + ")"));
    term::section_end();
    std::string text = strip_html(str(m, "text"));
    if (!text.empty()) std::cout << '\n' << text << '\n';
    return 0;
}

int komens_get(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink komens get <message id> [<attachment id>] [-o <path>]")) return 2;
    json m = client.call("GET", kBase + "/komens/" + ctx.args[0], nullptr);
    json files = m.value("attachments", json::array());
    if (files.empty()) {
        term::error("Message " + ctx.args[0] + " has no attachments.");
        return 1;
    }
    const json* file = nullptr;
    if (ctx.args.size() > 1) {
        for (const auto& a : files)
            if (id_of(a) == ctx.args[1] || str(a, "filename") == ctx.args[1]) file = &a;
        if (!file) {
            term::error("Message " + ctx.args[0] + " has no attachment " + ctx.args[1] + " (gn bakalink komens show lists them).");
            return 1;
        }
    } else if (files.size() == 1) {
        file = &files[0];
    } else {
        term::error("Message " + ctx.args[0] + " has " + std::to_string(files.size()) + " attachments; name one by its id (gn bakalink komens show lists them).");
        return 2;
    }
    std::string name = base_name(str(*file, "filename", "attachment"));
    fs::path dest = download_target(ctx, name);
    std::error_code ec;
    if (!dest.empty() && !ctx.force && fs::exists(dest, ec)) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    std::uint64_t size = static_cast<std::uint64_t>(num(*file, "size"));
    fetch_into(dest, name, size, "", [&](const http::Sink& sink, const http::Progress& progress) {
        client.download(kBase + "/komens/" + ctx.args[0] + "/attachments/" + id_of(*file), {}, nullptr, sink, progress);
    });
    if (!dest.empty()) term::success("Downloaded " + term::bold(name) + " " + term::dim("→ " + utf8(dest) + " (" + human_size(size) + ")"));
    return 0;
}

int komens_push_body(const Context& ctx, api::Client& client, const std::string& account, json body) {
    json res = client.call("PUT", kBase + "/accounts/" + account + "/komens", nullptr, &body);
    if (ctx.json) return print_json(res);
    int files = static_cast<int>(num(res, "attachmentsStored"));
    term::success("Komens: " + std::to_string(static_cast<int>(num(res, "created"))) + " new, " +
                  std::to_string(static_cast<int>(num(res, "updated"))) + " updated" +
                  (files ? ", " + std::to_string(files) + (files == 1 ? " attachment" : " attachments") + " stored" : ""));
    return 0;
}

int komens_push(const Context& ctx, api::Client& client) {
    auto input = parse_input(ctx, "bakalink komens push --account <account> --file <json|->");
    if (!input) return 2;
    // {"messages": […]}, a bare array of messages, or one message.
    json body = input->is_array() ? json{{"messages", *input}} : input->contains("messages") ? *input : json{{"messages", json::array({*input})}};
    // An attachment may name a local file as "path" in place of base64 "data".
    for (auto& m : body["messages"]) {
        if (!m.is_object() || !m.contains("attachments") || !m["attachments"].is_array()) continue;
        for (auto& a : m["attachments"]) {
            if (!a.is_object() || !a.contains("path") || !a["path"].is_string()) continue;
            fs::path p = from_utf8(a["path"].get<std::string>());
            a["data"] = base64(read_file(p));
            if (!a.contains("filename")) a["filename"] = utf8(p.filename());
            a.erase("path");
        }
    }
    auto id = account_id(ctx, client);
    if (!id) return 1;
    return komens_push_body(ctx, client, *id, std::move(body));
}

int komens_send(const Context& ctx, api::Client& client) {
    const char* usage = "bakalink komens send --account <account> --title <title> [--content <text> | --content-file <path>] [files...]";
    std::string content = read_content(ctx);
    if (ctx.title.empty() && content.empty()) {
        term::error("A message needs a title or content.");
        std::cerr << "  Usage: gn-cli " << usage << "\n";
        return 2;
    }
    std::string kind = ctx.kind.empty() ? "RECEIVED" : ctx.kind;
    for (char& c : kind) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (kind != "RECEIVED" && kind != "NOTICEBOARD") {
        term::error("--kind is received or noticeboard, not " + ctx.kind + ".");
        return 2;
    }
    std::string at = ctx.at.empty() ? now_iso() : ctx.at;
    // Without Bakaláři's own id every send is a message of its own.
    json message = {{"bakaId", ctx.baka_id.empty() ? "gn-" + at + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) : ctx.baka_id},
                    {"kind", kind}, {"title", ctx.title}, {"text", content}, {"sentAt", at}};
    if (!ctx.author_id.empty()) {
        message["sender"] = {{"bakaId", ctx.author_id}};
        if (!ctx.author.empty()) message["sender"]["name"] = ctx.author;
    } else if (!ctx.author.empty()) {
        message["senderName"] = ctx.author;
    }
    json attachments = json::array();
    for (const auto& arg : ctx.args) {
        fs::path p = from_utf8(arg);
        attachments.push_back({{"filename", utf8(p.filename())}, {"data", base64(read_file(p))}});
    }
    message["attachments"] = attachments;
    auto id = account_id(ctx, client);
    if (!id) return 1;
    return komens_push_body(ctx, client, *id, json{{"messages", json::array({message})}});
}

int komens_remove(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink komens rm <message id>")) return 2;
    if (!confirm(ctx, "Delete message " + ctx.args[0] + " and its attachments?")) return 1;
    client.call("DELETE", kBase + "/komens/" + ctx.args[0], nullptr);
    if (!ctx.json) term::success("Deleted message " + ctx.args[0]);
    return 0;
}

// ── marks ──────────────────────────────────────────────────────────────────

int marks_list(const Context& ctx, api::Client& client) {
    auto id = account_id(ctx, client);
    if (!id) return 1;
    http::Form query;
    if (!ctx.subject.empty()) query.emplace_back("subject", ctx.subject);
    json view = client.call("GET", kBase + "/accounts/" + *id + "/marks" + (query.empty() ? "" : "?" + http::encode_query(query)), nullptr);
    if (ctx.json) return print_json(view);
    json subjects = view.value("subjects", json::array());
    if (subjects.empty()) term::info(ctx.subject.empty() ? "No subjects yet." : "No subject " + ctx.subject + ".");
    for (const auto& s : subjects) {
        std::string average = str(s, "averageText");
        term::section_begin(str(s, "name", str(s, "bakaId")) + (average.empty() ? "" : " " + term::dim("⌀ " + average)));
        json marks = s.value("marks", json::array());
        if (marks.empty()) term::row("", term::dim("no marks"), 0);
        for (const auto& m : marks) {
            std::string text = str(m, "markText", "?");
            if (flag(m, "isPoints") && m.contains("maxPoints") && m["maxPoints"].is_number()) text += "/" + std::to_string(static_cast<int>(num(m, "maxPoints")));
            std::string weight = m.contains("weight") && m["weight"].is_number() ? "×" + std::to_string(static_cast<int>(num(m, "weight"))) : "";
            std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(m), 5) << "  " << pad_right(term::bold(text), 8) << pad_right(term::dim(weight), 5)
                      << pad_right(truncate(str(m, "caption"), 40), 42) << term::dim(format_time(m.value("markDate", json())))
                      << (flag(m, "isNew") ? "  " + term::green("new") : "") << '\n';
        }
        term::section_end();
    }
    return 0;
}

int marks_push_body(const Context& ctx, api::Client& client, const std::string& account, const json& body) {
    json res = client.call("PUT", kBase + "/accounts/" + account + "/marks" + (ctx.prune ? "?prune=true" : ""), nullptr, &body);
    if (ctx.json) return print_json(res);
    int deleted = static_cast<int>(num(res, "deleted"));
    term::success("Marks: " + std::to_string(static_cast<int>(num(res, "created"))) + " new, " +
                  std::to_string(static_cast<int>(num(res, "updated"))) + " updated" +
                  (ctx.prune ? ", " + std::to_string(deleted) + " removed" : ""));
    return 0;
}

int marks_push(const Context& ctx, api::Client& client) {
    auto input = parse_input(ctx, "bakalink marks push --account <account> --file <json|-> [--prune]");
    if (!input) return 2;
    // {"subjects": […]} or a bare array of subjects.
    json body = input->is_array() ? json{{"subjects", *input}} : *input;
    auto id = account_id(ctx, client);
    if (!id) return 1;
    return marks_push_body(ctx, client, *id, body);
}

int marks_add(const Context& ctx, api::Client& client) {
    if (ctx.subject.empty() || ctx.mark.empty()) {
        term::error("A mark needs --subject and --mark.");
        std::cerr << "  Usage: gn-cli bakalink marks add --account <account> --subject <Bakaláři id> --mark <mark> [--weight <n>] [--caption <text>] [--at <time>] [--id <Bakaláři id>]\n";
        return 2;
    }
    if (ctx.prune) {
        term::error("--prune goes with marks push: here it would delete every other mark.");
        return 2;
    }
    std::string at = ctx.at.empty() ? now_iso() : ctx.at;
    json mark = {{"bakaId", ctx.baka_id.empty() ? "gn-" + at + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) : ctx.baka_id},
                 {"markText", ctx.mark}, {"markDate", at}};
    if (!ctx.weight.empty()) {
        if (!all_digits(ctx.weight)) {
            term::error("--weight is a whole number, not " + ctx.weight + ".");
            return 2;
        }
        mark["weight"] = std::stoi(ctx.weight);
    }
    if (!ctx.caption.empty()) mark["caption"] = ctx.caption;
    put_ref(mark, "teacher", ctx.teacher);
    auto id = account_id(ctx, client);
    if (!id) return 1;
    return marks_push_body(ctx, client, *id, json{{"subjects", json::array({{{"bakaId", ctx.subject}, {"marks", json::array({mark})}}})}});
}

int marks_remove(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink marks rm <mark id>")) return 2;
    if (!confirm(ctx, "Delete mark " + ctx.args[0] + "?")) return 1;
    client.call("DELETE", kBase + "/marks/" + ctx.args[0], nullptr);
    if (!ctx.json) term::success("Deleted mark " + ctx.args[0]);
    return 0;
}

// ── timetable ──────────────────────────────────────────────────────────────

std::string lesson_line(const json& l) {
    const json hour = l.value("hour", json::object());
    std::string time = str(hour, "beginTime");
    if (!str(hour, "endTime").empty()) time += "–" + str(hour, "endTime");
    std::string change = str(l, "changeType");
    if (!str(l, "changeDescription").empty()) change += (change.empty() ? "" : ": ") + str(l, "changeDescription");
    std::string subject = ref_name(l, "subject");
    return pad_left(str(hour, "caption", str(hour, "bakaId")), 3) + "  " + pad_right(term::dim(time), 13) +
           pad_right(subject.empty() ? term::dim(none()) : term::bold(truncate(subject, 26)), 28) + pad_right(ref_name(l, "teacher", true), 8) +
           pad_right(str(l, "room"), 8) + (str(l, "cycle").empty() ? "" : term::dim(str(l, "cycle")) + "  ") +
           (change.empty() ? "" : term::yellow(change));
}

int timetable_actual(const Context& ctx, api::Client& client) {
    auto id = account_id(ctx, client);
    if (!id) return 1;
    http::Form query;
    if (!ctx.from.empty()) query.emplace_back("from", ctx.from);
    if (!ctx.to.empty()) query.emplace_back("to", ctx.to);
    json view = client.call("GET", kBase + "/accounts/" + *id + "/timetable/actual" + (query.empty() ? "" : "?" + http::encode_query(query)), nullptr);
    if (ctx.json) return print_json(view);
    json days = view.value("days", json::array());
    if (days.empty()) term::info("Nothing stored between " + str(view, "from") + " and " + str(view, "to") + ".");
    for (const auto& d : days) {
        std::string note = str(d, "dayType") == "WorkDay" ? "" : str(d, "dayType");
        if (!str(d, "description").empty()) note += (note.empty() ? "" : ": ") + str(d, "description");
        term::section_begin(str(d, "date") + (note.empty() ? "" : " " + term::dim(note)));
        json lessons = d.value("lessons", json::array());
        if (lessons.empty()) term::row("", term::dim("no lessons"), 0);
        for (const auto& l : lessons) std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(l), 6) << "  " << lesson_line(l) << '\n';
        term::section_end();
    }
    return 0;
}

int timetable_permanent(const Context& ctx, api::Client& client) {
    auto id = account_id(ctx, client);
    if (!id) return 1;
    json view = client.call("GET", kBase + "/accounts/" + *id + "/timetable/permanent", nullptr);
    if (ctx.json) return print_json(view);
    static const char* kDays[] = {"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    json lessons = view.value("lessons", json::array());
    if (lessons.empty()) term::info("No permanent timetable stored.");
    int open_day = 0;
    for (const auto& l : lessons) {
        int day = static_cast<int>(num(l, "dayOfWeek"));
        if (day != open_day) {
            if (open_day) term::section_end();
            term::section_begin(day >= 1 && day <= 7 ? kDays[day] : "Day " + std::to_string(day));
            open_day = day;
        }
        std::cout << term::dim(term::g().vbar) << ' ' << lesson_line(l) << '\n';
    }
    if (open_day) term::section_end();
    return 0;
}

int timetable_push(const Context& ctx, api::Client& client, bool permanent) {
    auto input = parse_input(ctx, std::string("bakalink timetable ") + (permanent ? "permanent " : "") + "push --account <account> --file <json|->");
    if (!input) return 2;
    auto id = account_id(ctx, client);
    if (!id) return 1;
    json res = client.call("PUT", kBase + "/accounts/" + *id + "/timetable/" + (permanent ? "permanent" : "actual"), nullptr, &*input);
    if (ctx.json) return print_json(res);
    term::success(std::string(permanent ? "Permanent timetable: " : "Timetable: ") + std::to_string(static_cast<int>(num(res, "lessons"))) +
                  " lessons on " + std::to_string(static_cast<int>(num(res, "days"))) + " days stored");
    return 0;
}

int timetable_add(const Context& ctx, api::Client& client) {
    if (ctx.date.empty() || ctx.hour.empty()) {
        term::error("A lesson needs --date and --hour.");
        std::cerr << "  Usage: gn-cli bakalink timetable add --account <account> --date <yyyy-mm-dd> --hour <Bakaláři id> [--subject <Bakaláři id>] "
                     "[--teacher <Bakaláři id>] [--room <room>] [--change <type>] [--description <text>]\n";
        return 2;
    }
    json body = {{"date", ctx.date}, {"hour", ctx.hour}};
    put_ref(body, "subject", ctx.subject);
    put_ref(body, "teacher", ctx.teacher);
    if (!ctx.room.empty()) body["room"] = ctx.room;
    if (!ctx.change.empty()) body["changeType"] = ctx.change;
    if (!ctx.description.empty()) body["changeDescription"] = ctx.description;
    auto id = account_id(ctx, client);
    if (!id) return 1;
    json res = client.call("POST", kBase + "/accounts/" + *id + "/timetable/actual/lessons", nullptr, &body);
    if (ctx.json) return print_json(res);
    term::success("Added lesson " + id_of(res) + " on " + ctx.date);
    return 0;
}

int timetable_remove(const Context& ctx, api::Client& client) {
    if (!need_args(ctx, 1, "bakalink timetable rm <lesson id>")) return 2;
    if (!confirm(ctx, "Delete lesson " + ctx.args[0] + "?")) return 1;
    client.call("DELETE", kBase + "/timetable/actual/lessons/" + ctx.args[0], nullptr);
    if (!ctx.json) term::success("Deleted lesson " + ctx.args[0]);
    return 0;
}

// The word after `gn bakalink <area>`, taken off the arguments when it is one of `verbs`.
std::string take_verb(const Context& ctx, std::initializer_list<const char*> verbs) {
    if (ctx.args.empty()) return "";
    std::string word = ctx.args[0];
    if (std::none_of(verbs.begin(), verbs.end(), [&](const char* v) { return word == v; })) return "";
    ctx.args.erase(ctx.args.begin());
    return word;
}

int unknown_verb(const Context& ctx, const std::string& area, const std::string& verbs) {
    term::error("gn bakalink " + area + " has no " + ctx.args[0] + "; there is " + verbs + ".");
    return 2;
}

}  // namespace

int bakalink_accounts(const Context& ctx) {
    auto client = open(ctx);
    if (!client) return 2;
    return guarded(ctx, [&] {
        std::string verb = take_verb(ctx, {"ls", "list", "add", "new", "edit", "rm", "delete"});
        if (verb == "add" || verb == "new") return account_add(ctx, *client);
        if (verb == "edit") return account_edit(ctx, *client);
        if (verb == "rm" || verb == "delete") return account_remove(ctx, *client);
        if (verb.empty() && !ctx.args.empty()) return unknown_verb(ctx, "account", "ls, add, edit and rm");
        return accounts_list(ctx, *client);
    });
}

int bakalink_komens(const Context& ctx) {
    auto client = open(ctx);
    if (!client) return 2;
    return guarded(ctx, [&] {
        std::string verb = take_verb(ctx, {"ls", "list", "show", "get", "download", "push", "send", "rm", "delete"});
        if (verb == "show") return komens_show(ctx, *client);
        if (verb == "get" || verb == "download") return komens_get(ctx, *client);
        if (verb == "push") return komens_push(ctx, *client);
        if (verb == "send") return komens_send(ctx, *client);
        if (verb == "rm" || verb == "delete") return komens_remove(ctx, *client);
        if (verb.empty() && !ctx.args.empty()) return unknown_verb(ctx, "komens", "ls, show, get, push, send and rm");
        return komens_list(ctx, *client);
    });
}

int bakalink_marks(const Context& ctx) {
    auto client = open(ctx);
    if (!client) return 2;
    return guarded(ctx, [&] {
        std::string verb = take_verb(ctx, {"ls", "list", "push", "add", "rm", "delete"});
        if (verb == "push") return marks_push(ctx, *client);
        if (verb == "add") return marks_add(ctx, *client);
        if (verb == "rm" || verb == "delete") return marks_remove(ctx, *client);
        if (verb.empty() && !ctx.args.empty()) return unknown_verb(ctx, "marks", "ls, push, add and rm");
        return marks_list(ctx, *client);
    });
}

int bakalink_timetable(const Context& ctx) {
    auto client = open(ctx);
    if (!client) return 2;
    return guarded(ctx, [&] {
        std::string verb = take_verb(ctx, {"ls", "list", "actual", "permanent", "push", "add", "rm", "delete"});
        if (verb == "permanent") {
            std::string second = take_verb(ctx, {"ls", "list", "push"});
            if (second == "push") return timetable_push(ctx, *client, true);
            if (second.empty() && !ctx.args.empty()) return unknown_verb(ctx, "timetable permanent", "ls and push");
            return timetable_permanent(ctx, *client);
        }
        if (verb == "push") return timetable_push(ctx, *client, false);
        if (verb == "add") return timetable_add(ctx, *client);
        if (verb == "rm" || verb == "delete") return timetable_remove(ctx, *client);
        if (verb.empty() && !ctx.args.empty()) return unknown_verb(ctx, "timetable", "actual, permanent, push, add and rm");
        return timetable_actual(ctx, *client);
    });
}

}  // namespace commands

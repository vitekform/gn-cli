#include "commands.hpp"
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "api.hpp"
#include "config.hpp"
#include "http.hpp"
#include "sha256.hpp"
#include "rsys_compatibility.hpp"
#include <regex>
#include <set>
#include "term.hpp"


namespace commands {

// ── auth login ─────────────────────────────────────────────────────────────

namespace {
int sign_in(const Context& ctx, const std::string& email, const std::string& password);
}  // namespace

int auth_login(const Context& ctx) {
    if (auto existing = config::load())
        term::warn("Already logged in as " + term::bold(existing->email) + "; this replaces that session.");

    std::string email = ctx.email;
    if (email.empty() && !term::prompt("Email:", email)) {
        missing_input("No email given.", "pass -e/--email");
        return 1;
    }
    email = trim(email);
    if (email.empty()) {
        term::error("No email given.");
        return 1;
    }

    std::string password;
    bool got = ctx.password_stdin ? static_cast<bool>(std::getline(std::cin, password))
                                  : term::prompt_secret("Password:", password);
    if (!password.empty() && password.back() == '\r') password.pop_back();
    if (!got || password.empty()) {
        if (ctx.password_stdin) term::error("No password given.");
        else missing_input("No password given.", "pipe it in with --password-stdin");
        return 1;
    }
    return sign_in(ctx, email, password);
}

namespace {

// Logs in with an email and password (and the emailed device code, if the server asks) and
// saves the session. The second half of auth login, shared with auth register.
int sign_in(const Context& ctx, const std::string& email, const std::string& password) {
    auto existing = config::load();
    std::string url = ctx.api_url.empty() ? api::kDefaultUrl : ctx.api_url;

    // Reuse the old master token for the same account: the server then trusts this device outright.
    std::string master;
    if (existing && existing->email == email && existing->api_url == url) master = existing->master_token;

    api::Client client(url, api::default_user_agent());
    api::LoginResponse lr = client.login(email, password, master);

    if (lr.error == "device_verification_required") {
        std::string verification_token = lr.token;
        term::info("New device. A verification code was sent to " + term::bold(email) + ".");
        if (!term::interactive()) {
            term::error("Device verification needs a code typed interactively; log in once from a terminal.");
            std::cerr << "  Not interactive (" << term::noninteractive_reason() << ").\n";
            return 1;
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::string code;
            if (!term::prompt("Code:", code)) {
                term::error("No code given.");
                return 1;
            }
            lr = client.verify_device(verification_token, trim(code));
            if (lr.error != "wrong_code") break;
            term::warn(api::describe_login_error(lr.error));
        }
    }

    if (!lr.ok()) {
        term::error(api::describe_login_error(lr.error.empty() ? "unknown" : lr.error));
        return 1;
    }

    config::Credentials creds;
    creds.api_url = url;
    creds.user_id = lr.user_id;
    creds.email = email;
    creds.token = lr.token;
    creds.master_token = lr.master_token;
    creds.user_agent = client.user_agent();
    config::save(creds);

    term::success("Logged in as " + term::bold(email) + " " + term::dim("(user #" + std::to_string(lr.user_id) + ")"));
    std::cout << "  " << term::dim("Session saved to " + config::credentials_path().string()) << '\n';
    return 0;
}

}  // namespace

// ── auth register / verify-email ───────────────────────────────────────────

int auth_verify_email(const Context& ctx) {
    auto creds = config::load();
    if (!creds) {
        not_logged_in();
        return 1;
    }
    if (!term::interactive()) {
        missing_input("Verifying an email needs the code typed in.", "run it in a terminal");
        return 1;
    }
    api::Client client(resolve_url(ctx, &*creds), creds->user_agent.empty() ? api::default_user_agent() : creds->user_agent);
    // A fresh code, with the token that goes with it.
    // The server takes the user from the session headers; at most one code a minute.
    json sent = client.post_form("/api/v2/auth/send-email-verification", {}, &*creds);
    if (std::string err = str(sent, "error"); !err.empty() && err != "ok")
        throw std::runtime_error(str(sent, "message", "the server sent no code (" + err + ")"));
    std::string token = str(sent, "token");
    if (token.empty()) throw std::runtime_error("the server sent no verification token");
    term::info("A code is on its way to " + term::bold(creds->email) + term::dim(" (a new code at most once a minute)"));
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string code;
        if (!term::prompt("Code:", code)) return 1;
        json r = client.post_form("/api/v2/auth/verify-email", {{"verificationToken", token}, {"code", trim(code)}}, &*creds);
        if (str(r, "error") == "ok") {
            creds->token = str(r, "token", creds->token);
            creds->master_token = str(r, "masterToken", creds->master_token);
            config::save(*creds);
            term::success("Email verified.");
            return 0;
        }
        term::warn(str(r, "message", "That code didn't work."));
    }
    term::error("Not verified; run gn auth verify-email for a new code.");
    return 1;
}

int auth_register(const Context& ctx) {
    std::string email = ctx.email;
    if (email.empty() && !term::prompt("Email:", email)) {
        missing_input("No email given.", "pass -e/--email");
        return 1;
    }
    email = trim(email);
    std::string password, again;
    if (ctx.password_stdin) {
        std::getline(std::cin, password);
        if (!password.empty() && password.back() == '\r') password.pop_back();
    } else if (!term::prompt_secret("Password:", password) || !term::prompt_secret("Again:", again) || again != password) {
        if (term::interactive()) term::error("The two passwords differ.");
        else missing_input("No password given.", "pipe it in with --password-stdin");
        return 1;
    }
    constexpr std::size_t kMinPassword = 8;  // AuthService.register
    if (email.empty() || password.size() < kMinPassword) {
        term::error(email.empty() ? "No email given." : "The password needs at least 8 characters.");
        return 1;
    }

    std::string url = ctx.api_url.empty() ? api::kDefaultUrl : ctx.api_url;
    http::Response res = http::post_form(url + "/api/v2/auth/register", {{"email", email}, {"password", password}}, {},
                                         api::default_user_agent());
    json r = json::parse(res.body, nullptr, false);
    std::string error = r.is_object() ? str(r, "error") : "";
    if (res.status >= 400 || error != "ok") {
        if (error == "email_already_exists") term::error("There is already an account for " + email + "; gn auth login signs in.");
        else if (error == "password_too_short") term::error("The password needs at least 8 characters.");
        else term::error("Registration failed" + (error.empty() ? " (HTTP " + std::to_string(res.status) + ")" : ": " + error));
        return 1;
    }
    term::success("Registered " + term::bold(email) + ".");
    if (int rc = sign_in(ctx, email, password); rc != 0) return rc;
    if (!term::interactive()) {
        std::cout << "  " << term::dim("Verify the email address later with gn auth verify-email.") << '\n';
        return 0;
    }
    std::string answer;
    if (term::prompt("Verify your email address now? [Y/n]", answer) && (trim(answer).empty() || trim(answer)[0] == 'y' || trim(answer)[0] == 'Y'))
        return auth_verify_email(ctx);
    std::cout << "  " << term::dim("Later: gn auth verify-email") << '\n';
    return 0;
}

// ── auth logout ────────────────────────────────────────────────────────────

int auth_logout(const Context&) {
    auto creds = config::load();
    bool removed = config::remove();
    if (!creds && !removed) {
        term::info("You were not logged in.");
        return 0;
    }
    std::string who = creds && !creds->email.empty() ? " " + term::bold(creds->email) : "";
    term::success("Logged out" + who + ".");
    std::cout << "  " << term::dim("The saved session was deleted from this computer.") << '\n';
    return 0;
}

// ── shared ─────────────────────────────────────────────────────────────────


// ── info / me ──────────────────────────────────────────────────────────────

int info(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    api::Client& client = auth->client;
    config::Credentials* session = auth->session();
    const bool with_token = client.using_cli_token();

    // A CLI token only reaches what its scopes list, so ask it what those are and skip the rest.
    std::optional<json> self;
    std::vector<std::string> scopes;
    if (with_token) {
        self = client.get_authed("/api/v2/cli-tokens/self", nullptr);
        scopes = string_list(*self, "scopes");
    }
    auto granted = [&](const char* scope) {
        return !with_token || std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
    };

    std::optional<json> me;
    if (granted("account:read")) me = client.get_authed("/api/v2/auth/me", session);
    auto credits = granted("credits:read") ? client.try_get_authed("/api/v2/auth/me/credits", session) : std::nullopt;
    auto passkeys = granted("passkeys:read") ? client.try_get_authed("/api/v2/auth/passkey/list", session) : std::nullopt;
    // 403 without the openAIAPI permission, whatever the token says.
    auto api_tokens = granted("api-tokens:read") ? client.try_get_authed("/api/v2/api-tokens", session) : std::nullopt;

    if (ctx.json) {
        json out = {{"server", client.base_url()}};
        if (self) out["cliToken"] = *self;
        if (me) out["account"] = *me;
        if (credits) out["credits"] = *credits;
        if (passkeys) out["passkeys"] = *passkeys;
        if (api_tokens) out["apiTokens"] = *api_tokens;
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    const auto& G = term::g();
    std::cout << '\n';

    // CLI token
    if (self) {
        term::section_begin("CLI token");
        term::row("Name", term::bold(str(*self, "name")) + " " + term::dim("(#" + str(*self, "id") + ")"));
        term::row("Token", str(*self, "prefix") + G.ellipsis);
        term::row("Scopes", scopes.empty() ? term::dim("none") : term::green(join(scopes, ", ")));
        term::row("Expires", expiry_text(*self));
        term::section_end();
    }

    // Account
    std::string fallback_email = session ? session->email : (self ? str(*self, "email") : "");
    std::string fallback_id = session ? std::to_string(session->user_id) : (self ? str(*self, "userId") : "");
    term::section_begin("Account");
    if (me) {
        term::row("Email", term::bold(str(*me, "email", fallback_email)));
        term::row("User ID", str(*me, "id", fallback_id));
        std::string role = str(*me, "roleHumanReadable");
        std::string role_id = str(*me, "role");
        std::string level = str(*me, "rolePermissionLevel", "0");
        if (role.empty()) role = role_id.empty() ? "none" : role_id;
        std::string detail = role_id.empty() ? "level " + level : role_id + ", level " + level;
        term::row("Role", term::magenta(term::bold(role)) + " " + term::dim("(" + detail + ")"));
        if (auto it = me->find("masterTokenExpiration"); !with_token && it != me->end() && !it->is_null())
            term::row("Session expires", format_time(*it));
    } else {
        term::row("Email", term::bold(fallback_email));
        term::row("User ID", fallback_id);
    }
    term::row("Server", term::cyan(client.base_url()));
    term::section_end();

    // Permissions
    if (me) {
        term::section_begin("Permissions");
        bool custom = me->value("customPermissionsEnabled", false);
        term::row("Custom permissions", custom ? term::green("enabled") : term::dim("disabled"));
        std::vector<std::string> pages = string_list(*me, "allowedPages");
        std::sort(pages.begin(), pages.end());
        if (pages.empty()) {
            term::row("Allowed pages", term::dim(custom ? "none" : "per role"));
        } else {
            term::row("Allowed pages", std::to_string(pages.size()));
            for (const auto& p : pages)
                std::cout << term::dim(G.vbar) << "   " << term::cyan(G.bullet) << ' ' << p << '\n';
        }
        term::section_end();
    }

    if (credits) print_credits(*credits, 10);

    if (passkeys && passkeys->is_array()) print_named_list("Passkeys", *passkeys, "label", "none registered");
    if (api_tokens && api_tokens->is_array()) print_named_list("API tokens", *api_tokens, "name", "none");

    // Say what the token couldn't see, rather than leaving the reader to wonder.
    if (with_token) {
        std::vector<std::string> hidden;
        const std::pair<const char*, const char*> sections[] = {{"account:read", "role & permissions"},
                                                                {"credits:read", "credits"},
                                                                {"passkeys:read", "passkeys"},
                                                                {"api-tokens:read", "API tokens"}};
        for (const auto& [scope, what] : sections)
            if (!granted(scope)) hidden.push_back(std::string(what) + " " + term::dim(std::string("(") + scope + ")"));
        if (!hidden.empty()) term::info("Not granted to this token: " + join(hidden, ", "));
    }
    return 0;
}

// ── token scopes / list / create / revoke ──────────────────────────────────

int token_scopes(const Context& ctx) {
    auto auth = authenticate_session(ctx);
    if (!auth) return 1;
    json scopes = auth->client.get_authed("/api/v2/cli-tokens/scopes", auth->session());
    if (ctx.json) {
        std::cout << scopes.dump(2) << '\n';
        return 0;
    }
    const auto& G = term::g();
    std::cout << '\n';
    term::section_begin("CLI token scopes");
    bool first = true;
    for (const auto& s : scopes) {
        if (!first) std::cout << term::dim(G.vbar) << '\n';
        first = false;
        std::cout << term::dim(G.vbar) << ' ' << term::green(term::bold(pad_right(str(s, "id"), 18)))
                  << str(s, "description") << '\n';
        for (const auto& call : string_list(s, "calls"))
            std::cout << term::dim(G.vbar) << "   " << term::dim(std::string(G.arrow) + " " + call) << '\n';
    }
    term::section_end();
    return 0;
}

int token_list(const Context& ctx) {
    auto auth = authenticate_session(ctx);
    if (!auth) return 1;
    json tokens = auth->client.get_authed("/api/v2/cli-tokens", auth->session());
    if (ctx.json) {
        std::cout << tokens.dump(2) << '\n';
        return 0;
    }
    const auto& G = term::g();
    std::cout << '\n';
    term::section_begin("CLI tokens (" + std::to_string(tokens.size()) + ")");
    if (tokens.empty()) {
        term::row("", term::dim("none yet. Create one with: gn-cli token create <name> --scope <scope>"), 0);
    }
    bool first = true;
    for (const auto& t : tokens) {
        if (!first) std::cout << term::dim(G.vbar) << '\n';
        first = false;
        std::string id = "#" + str(t, "id");
        std::cout << term::dim(G.vbar) << ' ' << term::yellow(pad_right(id, 6)) << term::bold(str(t, "name")) << "  "
                  << term::dim(str(t, "prefix") + G.ellipsis) << '\n';
        std::string indent = std::string(term::dim(G.vbar)) + "       ";
        std::cout << indent << term::dim("scopes     ") << term::green(join(string_list(t, "scopes"), ", ")) << '\n';
        std::cout << indent << term::dim("created    ") << format_time(t.value("createdAt", json())) << '\n';
        std::cout << indent << term::dim("expires    ") << expiry_text(t) << '\n';
        auto used = t.find("lastUsedAt");
        std::cout << indent << term::dim("last used  ")
                  << (used == t.end() || used->is_null() ? term::dim("never") : format_time(*used)) << '\n';
    }
    term::section_end();
    return 0;
}


int token_create(const Context& ctx) {
    if (ctx.args.empty()) {
        term::error("Give the token a name: gn-cli token create <name> --scope <scope> ...");
        return 2;
    }
    std::vector<std::string> scopes;
    for (const auto& arg : ctx.scopes) {  // --scope a --scope b, or --scope a,b
        std::size_t start = 0;
        while (start <= arg.size()) {
            std::size_t comma = arg.find(',', start);
            std::string one = trim(arg.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!one.empty()) scopes.push_back(one);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (scopes.empty()) {
        term::error("A token needs at least one --scope. See them all with: gn-cli token scopes");
        return 2;
    }
    std::optional<int> days;
    try {
        days = parse_expiry(ctx.expires.empty() ? "90d" : ctx.expires);
    } catch (const std::invalid_argument& e) {
        term::error(e.what());
        return 2;
    }

    auto auth = authenticate_session(ctx);
    if (!auth) return 1;
    json body = {{"name", join(ctx.args, " ")}, {"scopes", scopes}};
    body["expiresInDays"] = days ? json(*days) : json(nullptr);
    json created = auth->client.call("POST", "/api/v2/cli-tokens", auth->session(), &body);

    if (ctx.json) {
        std::cout << created.dump(2) << '\n';
        return 0;
    }
    std::string token = str(created, "token");
    term::success("Created CLI token " + term::bold(str(created, "name")) + " " + term::dim("(#" + str(created, "id") + ")"));
    std::cout << '\n';
    term::section_begin("Your new token");
    term::row("Token", term::bold(term::yellow(token)), 10);
    term::row("Scopes", term::green(join(string_list(created, "scopes"), ", ")), 10);
    term::row("Expires", expiry_text(created), 10);
    term::section_end();
    term::warn("Copy it now: it is shown only this once.");
    std::cout << "  Use it with " << term::bold("gn-cli --token <token> me") << " or " << term::bold("export GN_TOKEN=<token>")
              << ".\n";
    return 0;
}

int token_revoke(const Context& ctx) {
    std::string id = ctx.args.empty() ? "" : ctx.args[0];
    if (!id.empty() && id[0] == '#') id.erase(0, 1);
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
        term::error("Give the token's ID, as shown by gn-cli token list: gn-cli token revoke <id>");
        return 2;
    }
    auto auth = authenticate_session(ctx);
    if (!auth) return 1;
    auth->client.call("DELETE", "/api/v2/cli-tokens/" + id, auth->session());
    term::success("Revoked CLI token #" + id + ".");
    return 0;
}

// ── s3 ─────────────────────────────────────────────────────────────────────

namespace {

const std::string kStorage = "/api/v3/storage";
constexpr int kListCap = 20000;  // the most one listing collects before giving up

// Provider names have spaces and keys have slashes, so both are encoded wherever they go.
std::string pct(const std::string& s) { return pct_encode(s); }

std::string at(const std::string& provider, const std::string& bucket) {
    return kStorage + "/" + pct(provider) + "/" + pct(bucket);
}

std::string with_query(std::string path, const http::Form& query) {
    char sep = '?';
    for (const auto& [k, v] : query) {
        path += sep + pct(k) + "=" + pct(v);
        sep = '&';
    }
    return path;
}

bool usage(const Context& ctx, std::size_t needed, const char* synopsis) {
    if (ctx.args.size() >= needed || ask_missing_args(ctx, needed, synopsis)) return true;
    term::error(std::string("Usage: gn-cli s3 ") + synopsis);
    return false;
}

// Keys never start with '/'; "" (or "/") is the bucket root.
std::string clean_key(std::string k) {
    std::size_t i = 0;
    while (i < k.size() && k[i] == '/') ++i;
    return k.substr(i);
}

// A key prefix for a folder: "a/b" → "a/b/", root stays "".
std::string as_dir(const std::string& k) {
    std::string d = clean_key(k);
    if (!d.empty() && d.back() != '/') d += '/';
    return d;
}

bool is_dir_key(const std::string& k) { return k.empty() || k.back() == '/'; }

std::string where(const std::string& provider, const std::string& bucket, const std::string& key) {
    return provider + ":" + bucket + "/" + key;
}

// The listing, following the server's pages until it ends or max_keys entries are in.
json list_objects(Auth& a, const std::string& provider, const std::string& bucket, const std::string& prefix,
                  bool recursive, int max_keys = kListCap) {
    json all = json::array();
    std::string cursor;
    do {
        http::Form q{{"limit", std::to_string(std::min(max_keys, 1000))}};
        if (!prefix.empty()) q.emplace_back("prefix", prefix);
        if (recursive) q.emplace_back("recursive", "true");
        if (!cursor.empty()) q.emplace_back("cursor", cursor);
        json page = a.client.get_authed(with_query(at(provider, bucket) + "/objects", q), a.session());
        if (page.contains("items") && page["items"].is_array())
            for (auto& item : page["items"]) {
                if (all.size() >= static_cast<std::size_t>(max_keys)) return all;
                all.push_back(item);
            }
        cursor = page.contains("nextCursor") && page["nextCursor"].is_string() ? page["nextCursor"].get<std::string>() : "";
    } while (!cursor.empty());
    return all;
}

// Every object under a folder prefix; refuses listings the server had to cut short.
json list_tree(Auth& a, const std::string& provider, const std::string& bucket, const std::string& prefix) {
    json items = list_objects(a, provider, bucket, prefix, true);
    if (items.size() >= static_cast<std::size_t>(kListCap))
        throw std::runtime_error("more than " + with_thousands(kListCap) + " objects under '" + prefix +
                                 "'; pick a smaller folder");
    return items;
}

bool is_dir_item(const json& item) { return item.value("isDirectory", item.value("directory", false)); }

std::uint64_t item_size(const json& item) {
    auto it = item.find("size");
    return it != item.end() && it->is_number() ? it->get<std::uint64_t>() : 0;
}

// The object's listing entry: a value when it exists, nullopt when it doesn't, and an empty
// json when listing isn't allowed (gallery users may read files they can't list).
std::optional<json> stat_object(Auth& a, const std::string& provider, const std::string& bucket,
                                const std::string& key) {
    try {
        // Nothing sorts before a prefix itself, so the first key under it is the object if it exists.
        json items = list_objects(a, provider, bucket, key, true, 1);
        if (!items.empty() && str(items[0], "key") == key) return items[0];
        return std::nullopt;
    } catch (const api::ApiError&) {
        return json();
    }
}

void report_missing(const std::string& provider, const std::string& bucket, const std::string& key) {
    term::error("No such object: " + where(provider, bucket, key));
    std::cerr << "  " << term::dim("For a folder, end the path with / or pass -r.") << '\n';
}

bool no_such_object(Auth& a, const std::string& provider, const std::string& bucket, const std::string& key) {
    if (stat_object(a, provider, bucket, key)) return false;
    report_missing(provider, bucket, key);
    return true;
}

// Opens an upload, streams the parts (the server sizes them; each goes straight into storage),
// and completes it. A failure abandons the upload so its parts don't linger.
void upload_file(Auth& a, const std::string& provider, const std::string& bucket, const std::string& key,
                 const fs::path& file, const std::string& content_type) {
    std::uint64_t size = fs::file_size(file);
    json body{{"key", key}, {"size", size}, {"contentType", content_type}};
    json opened = a.client.call("POST", at(provider, bucket) + "/uploads", a.session(), &body);
    std::string token = str(opened, "token");
    auto part_size = static_cast<std::uint64_t>(num(opened, "partSize"));
    auto parts = static_cast<std::uint64_t>(num(opened, "partCount"));
    if (token.empty() || part_size == 0 || parts == 0) throw std::runtime_error("the server didn't open the upload");
    std::string base = kStorage + "/uploads/" + pct(token);
    try {
        ProgressBar bar(term::unicode() ? "↑" : "^", base_name(key), size, show_progress());
        upload_parts(a, file, base_name(key), size, part_size, parts,
                     [&](std::uint64_t n) { return base + "/parts/" + std::to_string(n); }, bar);
        a.client.call("POST", base + "/complete", a.session());
    } catch (...) {
        try {
            a.client.call("DELETE", base, a.session());
        } catch (...) {
            // the server's reaper gets it
        }
        throw;
    }
}

// Downloads into dest (through a .gn-part file, so an interrupted run leaves no half file
// under the real name), or to stdout when dest is empty.
void download_file(Auth& a, const std::string& provider, const std::string& bucket, const std::string& key,
                   const fs::path& dest, std::uint64_t size_hint) {
    http::Form query{{"key", key}, {"download", "true"}};
    std::string path = at(provider, bucket) + "/object";
    ProgressBar bar(term::unicode() ? "↓" : "v", base_name(key), size_hint, show_progress());
    auto progress = [&](std::uint64_t done, std::uint64_t) { bar.update(done); };

    if (dest.empty()) {
        term::binary_stdout();
        a.client.download(path, query, a.session(), [&](const char* d, std::size_t n) {
            return std::fwrite(d, 1, n, stdout) == n;
        }, progress);
        std::fflush(stdout);
        return;
    }

    if (dest.has_parent_path()) fs::create_directories(dest.parent_path());
    fs::path tmp = dest;
    tmp += ".gn-part";
    try {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("can't write " + utf8(tmp));
            a.client.download(path, query, a.session(), [&](const char* d, std::size_t n) {
                return static_cast<bool>(out.write(d, static_cast<std::streamsize>(n)));
            }, progress);
            out.close();
            if (!out) throw std::runtime_error("can't write " + utf8(tmp));
        }
        std::error_code ec;
        fs::remove(dest, ec);  // rename won't replace a file on Windows
        fs::rename(tmp, dest);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

}  // namespace

int s3_providers(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json providers = auth->client.get_authed(kStorage + "/providers", auth->session());
    if (ctx.json) {
        std::cout << providers.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    term::section_begin("S3 providers (" + std::to_string(providers.size()) + ")");
    if (providers.empty()) term::row("", term::dim("none you can see"), 0);
    for (const auto& p : providers)
        std::cout << term::dim(term::g().vbar) << ' ' << term::cyan(term::g().bullet) << ' '
                  << term::bold(pad_right(str(p, "name"), 24)) << pad_right(str(p, "id"), 6) << term::dim(str(p, "type")) << '\n';
    term::section_end();
    return 0;
}

int s3_buckets(const Context& ctx) {
    if (!usage(ctx, 1, "buckets <provider>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    const std::string& provider = ctx.args[0];
    json buckets = auth->client.get_authed(kStorage + "/" + pct(provider) + "/buckets", auth->session());
    if (ctx.json) {
        std::cout << buckets.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    term::section_begin("Buckets on " + provider + " (" + std::to_string(buckets.size()) + ")");
    if (buckets.empty()) term::row("", term::dim("none"), 0);
    for (const auto& b : buckets)
        std::cout << term::dim(term::g().vbar) << ' ' << term::cyan(term::g().bullet) << ' '
                  << (b.is_string() ? b.get<std::string>() : b.dump()) << '\n';
    term::section_end();
    return 0;
}

int s3_make_bucket(const Context& ctx) {
    if (!usage(ctx, 2, "mb <provider> <bucket>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json body{{"name", ctx.args[1]}};
    auth->client.call("POST", kStorage + "/" + pct(ctx.args[0]) + "/buckets", auth->session(), &body);
    term::success("Created bucket " + term::bold(ctx.args[1]) + " on " + ctx.args[0] + ".");
    return 0;
}

int s3_list(const Context& ctx) {
    // `s3 ls` alone lists providers, `s3 ls <provider>` its buckets.
    if (ctx.args.empty()) return s3_providers(ctx);
    if (ctx.args.size() == 1) return s3_buckets(ctx);
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    const std::string& provider = ctx.args[0];
    const std::string& bucket = ctx.args[1];
    std::string prefix = ctx.args.size() > 2 ? as_dir(ctx.args[2]) : "";
    json items = list_objects(*auth, provider, bucket, prefix, ctx.recursive);
    if (ctx.json) {
        std::cout << items.dump(2) << '\n';
        return 0;
    }

    const auto& G = term::g();
    std::size_t dirs = 0, files = 0;
    std::uint64_t total = 0;
    std::cout << '\n';
    term::section_begin(where(provider, bucket, prefix));
    if (items.empty()) term::row("", term::dim("empty"), 0);
    for (const auto& item : items) {
        std::string name = str(item, "name", str(item, "key"));
        if (is_dir_item(item)) {
            ++dirs;
            std::cout << term::dim(G.vbar) << ' ' << pad_left(term::dim("dir"), 10) << "  " << pad_right("", 21)
                      << term::bold(term::blue(name + "/")) << '\n';
        } else {
            ++files;
            std::uint64_t size = item_size(item);
            total += size;
            std::string when = item.contains("lastModified") && !item["lastModified"].is_null()
                                   ? format_time(item["lastModified"])
                                   : "";
            std::cout << term::dim(G.vbar) << ' ' << term::yellow(pad_left(human_size(size), 10)) << "  "
                      << term::dim(pad_right(when, 21)) << name << '\n';
        }
    }
    term::section_end();
    std::string summary = std::to_string(files) + (files == 1 ? " file" : " files") + ", " + human_size(total);
    if (dirs) summary = std::to_string(dirs) + (dirs == 1 ? " folder, " : " folders, ") + summary;
    std::cout << "  " << term::dim(summary) << '\n';
    if (ctx.recursive && items.size() >= static_cast<std::size_t>(kListCap))
        term::warn("Stopped at " + with_thousands(kListCap) + " objects; list a narrower prefix to see the rest.");
    else if (!ctx.recursive && items.size() >= static_cast<std::size_t>(kListCap))
        term::warn("Stopped at " + with_thousands(kListCap) + " entries.");
    return 0;
}

int s3_mkdir(const Context& ctx) {
    if (!usage(ctx, 3, "mkdir <provider> <bucket> <path>")) return 2;
    std::string key = as_dir(ctx.args[2]);
    if (key.empty()) {
        term::error("Give the folder a name.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json body{{"prefix", key}};
    auth->client.call("POST", at(ctx.args[0], ctx.args[1]) + "/folder", auth->session(), &body);
    term::success("Created " + term::bold(where(ctx.args[0], ctx.args[1], key)));
    return 0;
}

int s3_upload(const Context& ctx) {
    if (!usage(ctx, 4, "upload <provider> <bucket> <bucket_path> <local_path>")) return 2;
    const std::string& provider = ctx.args[0];
    const std::string& bucket = ctx.args[1];
    const std::string& target = ctx.args[2];
    fs::path local(ctx.args[3]);

    if (!ctx.part_size.empty()) term::warn("--part-size is ignored: the server sizes the parts now.");

    std::error_code ec;
    if (!fs::exists(local, ec)) {
        term::error("No such file: " + utf8(local));
        return 1;
    }

    // What to send, and under which key.
    std::vector<std::pair<fs::path, std::string>> jobs;
    if (fs::is_directory(local, ec)) {
        if (!ctx.recursive) {
            term::error(utf8(local) + " is a folder; pass -r to upload everything in it.");
            return 2;
        }
        std::string prefix = as_dir(target);
        for (auto it = fs::recursive_directory_iterator(local, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it)
            if (it->is_regular_file()) jobs.emplace_back(it->path(), prefix + utf8(fs::relative(it->path(), local)));
        std::sort(jobs.begin(), jobs.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        if (jobs.empty()) {
            term::info(utf8(local) + " has no files to upload.");
            return 0;
        }
    } else {
        std::string key = clean_key(target);
        if (is_dir_key(target) || key.empty()) key = as_dir(target) + utf8(local.filename());
        jobs.emplace_back(local, key);
    }

    auto auth = authenticate(ctx);
    if (!auth) return 1;

    std::uint64_t total = 0;
    for (const auto& [file, key] : jobs) {
        std::uint64_t size = fs::file_size(file);
        std::string type = ctx.content_type.empty() ? guess_content_type(key) : ctx.content_type;
        upload_file(*auth, provider, bucket, key, file, type);
        total += size;
        term::success("Uploaded " + utf8(file) + " " + term::dim(term::g().arrow) + " " +
                      term::bold(where(provider, bucket, key)) + " " + term::dim("(" + human_size(size) + ")"));
    }
    if (jobs.size() > 1) std::cout << "  " << term::dim(std::to_string(jobs.size()) + " files, " + human_size(total)) << '\n';
    return 0;
}

int s3_download(const Context& ctx) {
    if (!usage(ctx, 3, "download <provider> <bucket> <path> [local_path]")) return 2;
    const std::string& provider = ctx.args[0];
    const std::string& bucket = ctx.args[1];
    const std::string& path = ctx.args[2];
    std::string output = !ctx.output.empty() ? ctx.output : ctx.args.size() > 3 ? ctx.args[3] : "";
    std::error_code ec;

    if (ctx.recursive || is_dir_key(path)) {
        if (output == "-") {
            term::error("A folder can't go to standard output; give a local folder instead.");
            return 2;
        }
        std::string prefix = as_dir(path);
        fs::path dir = !output.empty() ? fs::path(output) : from_utf8(prefix.empty() ? bucket : base_name(prefix));
        auto auth = authenticate(ctx);
        if (!auth) return 1;
        json items = list_tree(*auth, provider, bucket, prefix);

        std::vector<std::tuple<std::string, fs::path, std::uint64_t>> jobs;
        std::vector<std::string> existing;
        for (const auto& item : items) {
            std::string key = str(item, "key");
            auto rel = safe_local_path(key.substr(std::min(prefix.size(), key.size())));
            if (!rel) {
                term::warn("Skipping " + key + ": not a safe local file name.");
                continue;
            }
            fs::path dest = dir / *rel;
            if (fs::exists(dest, ec)) existing.push_back(utf8(dest));
            jobs.emplace_back(key, dest, item_size(item));
        }
        if (jobs.empty()) {
            term::info("Nothing under " + where(provider, bucket, prefix) + ".");
            return 0;
        }
        if (!existing.empty() && !ctx.force) {
            term::error(std::to_string(existing.size()) + " of these files already exist locally, e.g. " + existing[0]);
            std::cerr << "  Pass " << term::bold("--force") << " to overwrite them.\n";
            return 1;
        }
        std::uint64_t total = 0;
        for (const auto& [key, dest, size] : jobs) {
            download_file(*auth, provider, bucket, key, dest, size);
            total += size;
            term::success("Downloaded " + term::bold(key) + " " + term::dim(term::g().arrow) + " " + utf8(dest) + " " +
                          term::dim("(" + human_size(size) + ")"));
        }
        std::cout << "  " << term::dim(std::to_string(jobs.size()) + (jobs.size() == 1 ? " file, " : " files, ") + human_size(total) + " into " + utf8(dir))
                  << '\n';
        return 0;
    }

    std::string key = clean_key(path);
    fs::path dest;
    if (output.empty()) dest = from_utf8(base_name(key));
    else if (output != "-") {
        dest = output;
        if (output.back() == '/' || output.back() == '\\' || fs::is_directory(dest, ec)) dest /= from_utf8(base_name(key));
    }
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto st = stat_object(*auth, provider, bucket, key);
    if (!st) {
        report_missing(provider, bucket, key);
        return 1;
    }
    std::uint64_t size = st->is_null() ? 0 : item_size(*st);
    download_file(*auth, provider, bucket, key, dest, size);
    if (!dest.empty()) {
        std::uint64_t got = fs::file_size(dest, ec);
        term::success("Downloaded " + term::bold(where(provider, bucket, key)) + " " + term::dim(term::g().arrow) + " " +
                      utf8(dest) + " " + term::dim("(" + human_size(ec ? size : got) + ")"));
    }
    return 0;
}

int s3_cat(const Context& ctx) {
    if (!usage(ctx, 3, "cat <provider> <bucket> <path>")) return 2;
    Context c = ctx;
    c.args.resize(3);
    c.output = "-";
    c.recursive = false;
    return s3_download(c);
}

int s3_delete(const Context& ctx) {
    if (!usage(ctx, 3, "delete <provider> <bucket> <path>")) return 2;
    const std::string& provider = ctx.args[0];
    const std::string& bucket = ctx.args[1];
    const std::string& path = ctx.args[2];

    if (ctx.recursive || is_dir_key(path)) {
        std::string prefix = as_dir(path);
        if (prefix.empty()) {
            term::error("Refusing to empty a whole bucket; name a folder.");
            return 2;
        }
        auto auth = authenticate(ctx);
        if (!auth) return 1;
        json items = list_objects(*auth, provider, bucket, prefix, true);
        std::string count = items.size() >= static_cast<std::size_t>(kListCap) ? "over " + with_thousands(kListCap)
                                                                                 : with_thousands(static_cast<long long>(items.size()));
        if (!confirm(ctx, "Delete " + where(provider, bucket, prefix) + " and the " + count + " objects in it?")) {
            term::info("Nothing deleted.");
            return 1;
        }
        json res = auth->client.call("DELETE", with_query(at(provider, bucket) + "/folder", {{"prefix", prefix}}), auth->session());
        long long deleted = static_cast<long long>(num(res, "deleted"));
        term::success("Deleted " + term::bold(where(provider, bucket, prefix)) + " " +
                      term::dim("(" + with_thousands(deleted) + " objects)"));
        return 0;
    }

    std::string key = clean_key(path);
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    // S3 happily "deletes" keys that don't exist; say so instead of reporting success.
    if (no_such_object(*auth, provider, bucket, key)) return 1;
    auth->client.call("DELETE", with_query(at(provider, bucket) + "/object", {{"key", key}}), auth->session());
    term::success("Deleted " + term::bold(where(provider, bucket, key)));
    return 0;
}

int s3_rename(const Context& ctx) {
    if (!usage(ctx, 4, "rename <provider> <bucket> <old_path> <new_path>")) return 2;
    const std::string& provider = ctx.args[0];
    const std::string& bucket = ctx.args[1];
    const std::string& from = ctx.args[2];
    const std::string& to = ctx.args[3];
    // One call either way: the server moves a folder (keys ending in "/") object by object,
    // placeholders included, and copies files past 5 GiB in parts.
    auto move = [&](Auth& a, const std::string& old_key, const std::string& new_key) {
        json body{{"from", old_key}, {"to", new_key}};
        json res = a.client.call("POST", at(provider, bucket) + "/move", a.session(), &body);
        return static_cast<long long>(num(res, "moved"));
    };

    if (ctx.recursive || is_dir_key(from)) {
        std::string src = as_dir(from), dst = as_dir(to);
        if (src.empty() || dst.empty()) {
            term::error("Folder renames need a folder on both sides, not the bucket root.");
            return 2;
        }
        if (dst.rfind(src, 0) == 0 || src.rfind(dst, 0) == 0) {
            term::error("Can't move a folder into itself or its own parent path.");
            return 2;
        }
        auto auth = authenticate(ctx);
        if (!auth) return 1;
        long long moved = move(*auth, src, dst);
        term::success("Moved " + term::bold(where(provider, bucket, src)) + " " + term::dim(term::g().arrow) + " " +
                      term::bold(dst) + " " + term::dim("(" + with_thousands(moved) + " objects)"));
        return 0;
    }

    std::string old_key = clean_key(from);
    std::string new_key = is_dir_key(to) ? as_dir(to) + base_name(old_key) : clean_key(to);
    if (old_key == new_key) {
        term::info("Old and new names are the same; nothing to do.");
        return 0;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    if (no_such_object(*auth, provider, bucket, old_key)) return 1;
    move(*auth, old_key, new_key);
    term::success("Renamed " + term::bold(where(provider, bucket, old_key)) + " " + term::dim(term::g().arrow) + " " +
                  term::bold(new_key));
    return 0;
}

// ── rsys ───────────────────────────────────────────────────────────────────

namespace {

const std::string kRsys = "/api/v2/rsys";

bool rsys_usage(const Context& ctx, std::size_t needed, const char* synopsis) {
    if (ctx.args.size() >= needed || ask_missing_args(ctx, needed, synopsis)) return true;
    term::error(std::string("Usage: gn-cli rsys ") + synopsis);
    return false;
}

// The release system lives on its own host. An explicit --api-url (a dev server) serves it too,
// and so does a session saved against some other server than the default one.
std::string rsys_url(const Context& ctx, const config::Credentials* creds) {
    if (!ctx.rsys_url.empty()) return ctx.rsys_url;
    if (!ctx.api_url.empty()) return ctx.api_url;
    if (creds && !creds->api_url.empty() && creds->api_url != api::kDefaultUrl) return creds->api_url;
    return api::kDefaultRsysUrl;
}

// The saved session or --token, aimed at the release system. With quiet, a missing login is
// not reported: the caller has the public endpoints to fall back on.
std::optional<Auth> rsys_authenticate(const Context& ctx, bool quiet = false) {
    std::optional<config::Credentials> creds;
    if (ctx.token.empty()) {
        creds = config::load();
        if (!creds) {
            if (!quiet) not_logged_in();
            return std::nullopt;
        }
    }
    std::string ua = creds && !creds->user_agent.empty() ? creds->user_agent : api::default_user_agent();
    Auth a{creds, api::Client(rsys_url(ctx, creds ? &*creds : nullptr), ua)};
    if (!ctx.token.empty()) a.client.use_cli_token(ctx.token);
    return a;
}

// Nobody logged in: a client for the public endpoints only.
api::Client rsys_public_client(const Context& ctx) {
    auto creds = config::load();
    return api::Client(rsys_url(ctx, creds ? &*creds : nullptr), api::default_user_agent());
}

// One URL path segment. The server only accepts URL-safe names, but whatever the user typed
// must not escape its segment.
std::string seg(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string project_path(const std::string& project) { return kRsys + "/projects/" + seg(project); }

std::string release_path(const std::string& project, const std::string& version) {
    return project_path(project) + "/releases/" + seg(version);
}

bool is_latest(const std::string& version) { return version.empty() || version == "latest"; }

std::string status_text(const std::string& status) {
    if (status == "PUBLISHED") return term::green("published");
    if (status == "DRAFT") return term::yellow("draft");
    if (status == "YANKED") return term::red("yanked");
    if (status == "UPLOADING") return term::yellow("uploading");
    if (status == "READY") return term::green("ready");
    return status;
}

std::string read_changelog(const Context& ctx) {
    if (ctx.changelog_file.empty()) return ctx.changelog;
    std::string text;
    if (ctx.changelog_file == "-") {
        std::string line;
        while (std::getline(std::cin, line)) text += line + "\n";
        return text;
    }
    std::ifstream in(fs::path(ctx.changelog_file), std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + ctx.changelog_file);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return text;
}

void print_changelog(const std::string& text) {
    const auto& G = term::g();
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::cout << term::dim(G.vbar) << "   " << line << '\n';
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void print_project(const json& p) {
    term::section_begin(str(p, "name", str(p, "slug")));
    term::row("Slug", term::bold(str(p, "slug")));
    term::row("Visibility", p.value("isPublic", false) ? term::green("public") : term::dim("private"));
    if (auto d = str(p, "description"); !d.empty()) term::row("Description", d);
    term::row("Latest", str(p, "latestVersion", term::dim("nothing published")));
    term::row("Releases", str(p, "releaseCount", "0"));
    if (auto u = str(p, "publicUrl"); !u.empty()) term::row("Public", term::cyan(u));
    term::row("Created", format_time(p.value("createdAt", json())) + term::dim(" by " + str(p, "createdBy", none())));
    term::section_end();
}

void print_release_list(const std::string& project, const json& releases) {
    const auto& G = term::g();
    term::section_begin("Releases of " + project + " (" + std::to_string(releases.size()) + ")");
    if (releases.empty()) term::row("", term::dim("none yet. Publish one with: gn-cli rsys publish " + project + " <version> <files>"), 0);
    for (const auto& r : releases) {
        std::string when = r.contains("publishedAt") && !r["publishedAt"].is_null() ? format_time(r["publishedAt"])
                                                                                     : term::dim("not published");
        std::string tag = r.value("prerelease", false) ? " " + term::magenta("prerelease") : "";
        std::cout << term::dim(G.vbar) << ' ' << term::bold(pad_right(truncate(str(r, "version"), 22), 24))
                  << pad_right(status_text(str(r, "status")), 11) << pad_right(when, 22)
                  << truncate(str(r, "title"), 30) << tag << '\n';
    }
    term::section_end();
}

// A release from the management API or a public release/latest.json: same field names where it
// matters, except that public files carry a url instead of an id.
void print_release(const std::string& project, const json& r) {
    const auto& G = term::g();
    std::string version = str(r, "version");
    term::section_begin(project + " " + version);
    if (auto t = str(r, "title"); !t.empty()) term::row("Title", term::bold(t));
    if (r.contains("status")) term::row("Status", status_text(str(r, "status")));
    if (r.value("prerelease", false)) term::row("Prerelease", term::magenta("yes"));
    if (r.contains("publishedAt") && !r["publishedAt"].is_null()) term::row("Published", format_time(r["publishedAt"]));
    if (auto by = str(r, "createdBy"); !by.empty()) term::row("By", by);
    if (auto u = str(r, "publicUrl", str(r, "url")); !u.empty()) term::row("URL", term::cyan(u));
    const json* files = nullptr;
    if (auto it = r.find("files"); it != r.end() && it->is_array()) files = &*it;
    term::row("Files", files ? std::to_string(files->size()) : "0");
    if (files) {
        for (const auto& f : *files) {
            bool primary = f.value("primary", false) || (r.contains("primary") && r["primary"].is_object() && str(r["primary"], "filename") == str(f, "filename"));
            std::string mark = primary ? term::yellow(term::unicode() ? "★" : "*") : " ";
            std::string st = f.contains("status") && str(f, "status") != "READY" ? " " + status_text(str(f, "status")) : "";
            std::string dl = f.contains("downloadCount") ? term::dim("  " + str(f, "downloadCount") + " dl") : "";
            std::cout << term::dim(G.vbar) << "   " << mark << ' ' << pad_right(truncate(str(f, "filename"), 36), 38)
                      << pad_left(human_size(static_cast<std::uint64_t>(num(f, "size"))), 10) << dl << st << '\n';
            if (auto h = str(f, "sha256"); !h.empty())
                std::cout << term::dim(G.vbar) << "     " << term::dim("sha256 " + h) << '\n';
        }
    }
    if (auto c = str(r, "changelog"); !c.empty()) {
        std::cout << term::dim(G.vbar) << '\n' << term::dim(G.vbar) << ' ' << term::bold("Changelog") << '\n';
        print_changelog(c);
    }
    term::section_end();
}

// A file as the release's JSON lists it: --file, or the primary one, or the first finished one.
// The returns build the optional in place: MSVC won't convert json to optional<json> implicitly.
std::optional<json> pick_file(const json& release, const std::string& wanted) {
    auto it = release.find("files");
    if (it == release.end() || !it->is_array()) return std::nullopt;
    const json* first = nullptr;
    for (const auto& f : *it) {
        if (f.contains("status") && str(f, "status") != "READY") continue;
        if (!wanted.empty()) {
            if (str(f, "filename") == wanted) return std::optional<json>(std::in_place, f);
            continue;
        }
        if (f.value("primary", false)) return std::optional<json>(std::in_place, f);
        if (!first) first = &f;
    }
    if (wanted.empty() && first) return std::optional<json>(std::in_place, *first);
    return std::nullopt;
}

struct LocalFile {
    fs::path path;
    std::string name;  // as it will be called in the release
    std::uint64_t size = 0;
    std::string sha;
};

// "path" or "path#name". '#' is rare in paths, unlike ':' (C:\) and '=' (a=b.txt).
LocalFile parse_local(const std::string& arg) {
    LocalFile lf;
    std::size_t hash = arg.rfind('#');
    std::string path = hash == std::string::npos ? arg : arg.substr(0, hash);
    lf.path = fs::path(path);
    lf.name = hash == std::string::npos ? utf8(lf.path.filename()) : arg.substr(hash + 1);
    return lf;
}

void upload_release_file(Auth& a, const std::string& project, const std::string& version, const LocalFile& lf,
                         const std::string& content_type, json& uploaded) {
    json body = {{"filename", lf.name}, {"size", lf.size}, {"sha256", lf.sha}};
    if (!content_type.empty()) body["contentType"] = content_type;
    json created = a.client.call("POST", release_path(project, version) + "/files", a.session(), &body);
    std::string id = str(created, "id");
    auto part_size = static_cast<std::uint64_t>(num(created, "partSize"));
    auto parts = static_cast<std::uint64_t>(num(created, "partCount"));
    if (id.empty() || part_size == 0 || parts == 0) throw std::runtime_error("the server didn't start the upload");

    ProgressBar bar(term::unicode() ? "↑" : "^", lf.name, lf.size, show_progress());
    upload_parts(a, lf.path, lf.name, lf.size, part_size, parts,
                 [&](std::uint64_t part) { return kRsys + "/files/" + id + "/parts/" + std::to_string(part); }, bar);
    bar.note("sealing");
    with_retries("Finishing " + lf.name, [&] {
        uploaded = a.client.post_form(kRsys + "/files/" + id + "/complete", {}, a.session(), 0);
    });
}

std::string compatibility_query(const Context& ctx) {
    auto selected = rsys::selection(ctx.compatibility);
    http::Form query;
    for (auto it = selected.begin(); it != selected.end(); ++it) query.emplace_back("f." + it.key(), it.value().get<std::string>());
    if (ctx.prerelease) query.emplace_back("prerelease", "true");
    return query.empty() ? "" : "?" + http::encode_query(query);
}

struct CompatibleResult {
    json release;
    std::optional<Auth> auth;
};

CompatibleResult resolve_compatible(const Context& ctx, const std::string& project) {
    const auto query = compatibility_query(ctx);
    try {
        return {rsys_public_client(ctx).get_public("/" + seg(project) + "/public/resolve.json" + query), std::nullopt};
    } catch (const api::ApiError& e) {
        if (e.status != 404) throw;
        auto auth = rsys_authenticate(ctx, true);
        if (!auth) throw;
        json release = auth->client.get_authed(project_path(project) + "/resolve" + query, auth->session());
        return {std::move(release), std::move(auth)};
    }
}

std::vector<rsys::Variant> stored_variants(const json& metadata) {
    std::vector<rsys::Variant> result;
    for (const auto& variant : metadata.at("variants")) {
        rsys::Variant item{str(variant, "key"), variant.at("selection"), {}, ""};
        for (const auto& file : variant.at("files")) {
            item.files.push_back(str(file, "filename"));
            if (file.at("id") == variant.at("defaultFileId")) item.default_file = str(file, "filename");
        }
        result.push_back(std::move(item));
    }
    return result;
}

}  // namespace

int rsys_projects(const Context& ctx) {
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json list = auth->client.get_authed(kRsys + "/projects", auth->session());
    if (ctx.json) {
        std::cout << list.dump(2) << '\n';
        return 0;
    }
    const auto& G = term::g();
    std::cout << '\n';
    term::section_begin("Release projects (" + std::to_string(list.size()) + ")");
    if (list.empty()) term::row("", term::dim("none yet. Create one with: gn-cli rsys new-project <slug>"), 0);
    for (const auto& p : list) {
        std::string vis = p.value("isPublic", false) ? term::green("public ") : term::dim("private");
        std::string latest = str(p, "latestVersion", none());
        std::cout << term::dim(G.vbar) << ' ' << term::bold(pad_right(truncate(str(p, "slug"), 24), 26)) << vis << "  "
                  << pad_right(truncate(latest, 16), 18) << term::dim(truncate(str(p, "name"), 30)) << '\n';
    }
    term::section_end();
    return 0;
}

int rsys_project(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "project <project>")) return 2;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json p = auth->client.get_authed(project_path(ctx.args[0]), auth->session());
    json releases = auth->client.get_authed(project_path(ctx.args[0]) + "/releases", auth->session());
    if (ctx.json) {
        std::cout << json{{"project", p}, {"releases", releases}}.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    print_project(p);
    print_release_list(str(p, "slug"), releases);
    return 0;
}

int rsys_new_project(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "new-project <slug> [--name <name>] [--description <text>] [--public]")) return 2;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json body = {{"slug", ctx.args[0]}, {"isPublic", ctx.set_public}};
    if (!ctx.name.empty()) body["name"] = ctx.name;
    if (!ctx.description.empty()) body["description"] = ctx.description;
    json p = auth->client.call("POST", kRsys + "/projects", auth->session(), &body);
    if (ctx.json) {
        std::cout << p.dump(2) << '\n';
        return 0;
    }
    term::success("Created project " + term::bold(str(p, "slug")) + (ctx.set_public ? " (public)" : " (private)"));
    if (auto u = str(p, "publicUrl"); !u.empty()) std::cout << "  " << term::dim("Latest will be at ") << term::cyan(u) << '\n';
    return 0;
}

int rsys_edit_project(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "edit-project <slug> [--name <name>] [--description <text>] [--public|--private]")) return 2;
    if (ctx.set_public && ctx.set_private) {
        term::error("--public or --private, not both.");
        return 2;
    }
    json body = json::object();
    if (!ctx.name.empty()) body["name"] = ctx.name;
    if (!ctx.description.empty()) body["description"] = ctx.description;
    if (ctx.set_public) body["isPublic"] = true;
    if (ctx.set_private) body["isPublic"] = false;
    if (body.empty()) {
        term::error("Nothing to change: pass --name, --description, --public or --private.");
        return 2;
    }
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json p = auth->client.call("PATCH", project_path(ctx.args[0]), auth->session(), &body);
    if (ctx.json) {
        std::cout << p.dump(2) << '\n';
        return 0;
    }
    term::success("Updated project " + term::bold(str(p, "slug")) + ".");
    std::cout << '\n';
    print_project(p);
    return 0;
}

int rsys_delete_project(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "delete-project <slug>")) return 2;
    const std::string& slug = ctx.args[0];
    if (!confirm(ctx, "Delete project " + slug + " with every release and file? This can't be undone.")) return 1;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    auth->client.call("DELETE", project_path(slug), auth->session());
    term::success("Deleted project " + term::bold(slug) + ".");
    return 0;
}

int rsys_releases(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "releases <project>")) return 2;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json releases = auth->client.get_authed(project_path(ctx.args[0]) + "/releases", auth->session());
    if (ctx.json) {
        std::cout << releases.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    print_release_list(ctx.args[0], releases);
    return 0;
}

int rsys_show(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "show <project> [version]")) return 2;
    const std::string& project = ctx.args[0];
    std::string version = ctx.args.size() > 1 ? ctx.args[1] : "";
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json r = is_latest(version)
                 ? auth->client.get_authed(project_path(project) + "/latest" + (ctx.prerelease ? "?prerelease=true" : ""),
                                           auth->session())
                 : auth->client.get_authed(release_path(project, version), auth->session());
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    print_release(project, r);
    return 0;
}

int rsys_unyank(const Context& ctx);

int rsys_publish(const Context& ctx) {
    if (!rsys_usage(ctx, 2, "publish <project> <version> [file[#name]...] [--compat field=value | --variants-file manifest.json]")) return 2;
    const std::string& project = ctx.args[0];
    const std::string& version = ctx.args[1];
    const bool explicit_variants = !ctx.compatibility.empty() || !ctx.variants_file.empty();
    if (!ctx.compatibility.empty() && !ctx.variants_file.empty()) throw std::invalid_argument("Use either --compat or --variants-file");
    if (ctx.variants_file == "-" && ctx.changelog_file == "-") throw std::invalid_argument("The variants manifest and changelog cannot both read stdin");
    // Parse local metadata before contacting the server, and validate its schema before uploads.
    json selected = rsys::selection(ctx.compatibility);
    std::vector<rsys::Variant> plan = ctx.variants_file.empty() ? std::vector<rsys::Variant>{} : rsys::read_manifest(ctx.variants_file);
    if (ctx.args.size() == 2 && !explicit_variants && !ctx.draft) return rsys_unyank(ctx);
    std::vector<LocalFile> locals;
    static const std::regex filename("[A-Za-z0-9][A-Za-z0-9._-]{0,199}");
    for (std::size_t i = 2; i < ctx.args.size(); ++i) {
        LocalFile lf = parse_local(ctx.args[i]);
        std::error_code ec;
        if (!fs::is_regular_file(lf.path, ec)) throw std::invalid_argument("Not a file: " + utf8(lf.path));
        if (!std::regex_match(lf.name, filename)) throw std::invalid_argument("Invalid release file name: " + lf.name);
        for (const auto& other : locals)
            if (other.name == lf.name) throw std::invalid_argument("Two files would both be called " + lf.name + "; rename one with path#name");
        lf.size = fs::file_size(lf.path);
        if (lf.size == 0) throw std::invalid_argument(utf8(lf.path) + " is empty");
        locals.push_back(std::move(lf));
    }
    std::string changelog = read_changelog(ctx);
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;
    json project_info = a.client.get_authed(project_path(project), a.session());
    json fields = json::array(), metadata;
    const bool templates = project_info.contains("template") && project_info["template"].is_object();
    if (templates) fields = project_info["template"].at("fields");
    else if (explicit_variants) throw std::runtime_error("This server does not support release templates; update the RSYS backend first");

    json release;
    bool existed = false;
    try {
        release = a.client.get_authed(release_path(project, version), a.session());
        existed = true;
    } catch (const api::ApiError& e) { if (e.status != 404) throw; }
    if (existed) {
        const auto status = str(release, "status");
        if (status != "DRAFT" && !ctx.append) throw std::runtime_error(project + " " + str(release, "version") + " is already " + status + "; use --append or a new version");
        if (templates) {
            metadata = a.client.get_authed(release_path(project, str(release, "version")) + "/variants", a.session());
            fields = metadata.at("fields"); // Resume against the release snapshot, even after project template edits.
        }
    } else if (locals.empty()) throw std::invalid_argument("A new release needs at least one local file");

    std::map<std::string, json> have, ids;
    std::set<std::string> available;
    if (existed) for (const auto& file : release.at("files")) {
        have[str(file, "filename")] = file;
        if (str(file, "status") == "READY") {
            available.insert(str(file, "filename"));
            ids[str(file, "filename")] = file.at("id");
        }
    }
    for (const auto& lf : locals) available.insert(lf.name);
    if (!ctx.primary.empty() && !available.count(ctx.primary)) throw std::invalid_argument("--primary is not an existing ready file or a local upload: " + ctx.primary);
    if (!ctx.compatibility.empty()) {
        std::vector<std::string> attachments;
        // Keep the established first-file default, and include existing ready files on resume.
        for (const auto& lf : locals) attachments.push_back(lf.name);
        for (const auto& name : available) if (std::find(attachments.begin(), attachments.end(), name) == attachments.end()) attachments.push_back(name);
        std::string primary = ctx.primary;
        if (primary.empty()) for (const auto& [name, file] : have)
            if (file.value("primary", false) && available.count(name)) { primary = name; break; }
        if (primary.empty() && !attachments.empty()) primary = attachments.front();
        plan.push_back({"default", selected, std::move(attachments), primary});
    }
    auto preflight = [&] {
        if (explicit_variants) rsys::validate(plan, fields, available, !ctx.draft || (existed && str(release, "status") == "PUBLISHED"));
        else if (!ctx.draft && !fields.empty()) {
            if (metadata.is_null() || metadata.at("variants").empty())
                throw std::invalid_argument("This template requires compatible builds. Supply --compat field=value or --variants-file before uploading");
            rsys::validate(stored_variants(metadata), fields, available, true, true);
        }
    };
    preflight();
    // Hashing large files only starts after metadata and schema validation succeed.
    for (auto& lf : locals) lf.sha = sha256::of_file(lf.path);
    for (const auto& lf : locals) if (auto it = have.find(lf.name); it != have.end()) {
        const auto& file = it->second;
        bool same = str(file, "status") == "READY" && str(file, "sha256") == lf.sha && static_cast<std::uint64_t>(num(file, "size")) == lf.size;
        if (!same && str(file, "status") == "READY" && !ctx.force)
            throw std::runtime_error("The release already has a different " + lf.name + "; pass --force to replace it");
    }
    if (!existed) {
        json body = {{"version", version}, {"prerelease", ctx.prerelease}};
        if (!ctx.title.empty()) body["title"] = ctx.title;
        if (!changelog.empty()) body["changelog"] = changelog;
        release = a.client.call("POST", project_path(project) + "/releases", a.session(), &body);
        term::success("Created draft " + term::bold(project + " " + str(release, "version")));
        if (templates) {
            metadata = a.client.get_authed(release_path(project, str(release, "version")) + "/variants", a.session());
            fields = metadata.at("fields");
            preflight(); // Reject a concurrent project schema change before sending any file bytes.
        }
    } else {
        json patch = json::object();
        if (!ctx.title.empty()) patch["title"] = ctx.title;
        if (!changelog.empty()) patch["changelog"] = changelog;
        if (ctx.prerelease) patch["prerelease"] = true;
        if (!patch.empty()) release = a.client.call("PATCH", release_path(project, str(release, "version")), a.session(), &patch);
        term::info("Continuing " + term::bold(project + " " + str(release, "version")));
    }
    const std::string canonical = str(release, "version");
    std::set<json> removed;
    bool automatic_changed = false;
    const bool automatic = !metadata.is_null() && std::any_of(metadata.at("variants").begin(), metadata.at("variants").end(),
        [](const json& variant) { return variant.value("automatic", false); });
    std::uint64_t total = 0;
    for (const auto& lf : locals) {
        if (auto it = have.find(lf.name); it != have.end()) {
            const json& file = it->second;
            if (str(file, "status") == "READY" && str(file, "sha256") == lf.sha && static_cast<std::uint64_t>(num(file, "size")) == lf.size) {
                term::info(lf.name + " is already there " + term::dim("(same sha256)"));
                continue;
            }
            a.client.call("DELETE", kRsys + "/files/" + str(file, "id"), a.session());
            removed.insert(file.at("id"));
        }
        json uploaded;
        upload_release_file(a, project, canonical, lf, guess_content_type(lf.name), uploaded);
        ids[lf.name] = uploaded.at("id");
        automatic_changed = automatic;
        total += lf.size;
        term::success("Uploaded " + term::bold(lf.name));
    }
    if (!ctx.primary.empty()) {
        a.client.post_form(kRsys + "/files/" + ids.at(ctx.primary).dump() + "/primary", {}, a.session());
        automatic_changed = automatic;
    }
    if (explicit_variants) {
        const auto fresh = a.client.get_authed(release_path(project, canonical) + "/variants", a.session());
        if (!rsys::unchanged(metadata, fresh, removed, automatic_changed))
            throw std::runtime_error("Compatibility changed during upload. Uploaded files are kept; review the release and rerun before replacing metadata");
        auto body = rsys::replacement(plan, ids, fresh.at("version"));
        a.client.call("PUT", release_path(project, canonical) + "/variants", a.session(), &body);
    }
    if (ctx.draft) {
        release = a.client.get_authed(release_path(project, canonical), a.session());
        term::success(str(release, "status") == "DRAFT"
            ? "Left " + term::bold(project + " " + canonical) + " as a draft"
            : "Updated " + term::bold(project + " " + canonical) + " (" + str(release, "status") + ")");
    } else {
        bool was_live = str(release, "status") == "PUBLISHED";
        release = a.client.post_form(release_path(project, canonical) + "/publish", {}, a.session());
        term::success((was_live ? "Updated " : "Published ") + term::bold(project + " " + canonical));
    }
    if (ctx.json) std::cout << release.dump(2) << '\n';
    else if (auto url = str(release, "publicUrl"); !url.empty()) std::cout << "  " << term::cyan(url) << '\n';
    if (!ctx.json && total > 0) std::cout << "  " << term::dim(human_size(total) + " uploaded") << '\n';
    return 0;
}

int rsys_edit_release(const Context& ctx) {
    if (!rsys_usage(ctx, 2, "edit <project> <version> [--title ...] [--changelog ...|--changelog-file ...] [--prerelease|--stable]")) return 2;
    if (ctx.prerelease && ctx.stable) {
        term::error("--prerelease or --stable, not both.");
        return 2;
    }
    json body = json::object();
    if (!ctx.title.empty()) body["title"] = ctx.title;
    std::string changelog = read_changelog(ctx);
    if (!changelog.empty()) body["changelog"] = changelog;
    if (ctx.prerelease) body["prerelease"] = true;
    if (ctx.stable) body["prerelease"] = false;
    if (body.empty()) {
        term::error("Nothing to change: pass --title, --changelog, --changelog-file, --prerelease or --stable.");
        return 2;
    }
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json r = auth->client.call("PATCH", release_path(ctx.args[0], ctx.args[1]), auth->session(), &body);
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    term::success("Updated " + term::bold(ctx.args[0] + " " + str(r, "version")) + ".");
    return 0;
}

namespace {

int rsys_transition(const Context& ctx, const char* verb, const char* action, const char* done) {
    if (!rsys_usage(ctx, 2, (std::string(verb) + " <project> <version>").c_str())) return 2;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    json r = auth->client.post_form(release_path(ctx.args[0], ctx.args[1]) + "/" + action, {}, auth->session());
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    term::success(std::string(done) + " " + term::bold(ctx.args[0] + " " + str(r, "version")) + ".");
    return 0;
}

}  // namespace

int rsys_yank(const Context& ctx) { return rsys_transition(ctx, "yank", "yank", "Yanked"); }

int rsys_unyank(const Context& ctx) { return rsys_transition(ctx, "unyank", "publish", "Published"); }

int rsys_delete_release(const Context& ctx) {
    if (!rsys_usage(ctx, 2, "delete <project> <version>")) return 2;
    if (!confirm(ctx, "Delete " + ctx.args[0] + " " + ctx.args[1] + " and its files? This can't be undone.")) return 1;
    auto auth = rsys_authenticate(ctx);
    if (!auth) return 1;
    auth->client.call("DELETE", release_path(ctx.args[0], ctx.args[1]), auth->session());
    term::success("Deleted " + term::bold(ctx.args[0] + " " + ctx.args[1]) + ".");
    return 0;
}

int rsys_latest(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "latest <project> [--compat field=value] [--prerelease] [--since <version>]")) return 2;
    const std::string& project = ctx.args[0];
    if (!ctx.variants_file.empty()) throw std::invalid_argument("--variants-file is only supported by rsys publish");
    const bool compatible = !ctx.compatibility.empty();
    json r;
    bool from_public = true;
    if (compatible) {
        auto resolved = resolve_compatible(ctx, project);
        r = std::move(resolved.release);
        if (!ctx.since.empty()) {
            auto query = compatibility_query(ctx) + "&limit=200";
            auto history = resolved.auth
                ? resolved.auth->client.get_authed(project_path(project) + "/compatible-releases" + query, resolved.auth->session())
                : rsys_public_client(ctx).get_public("/" + seg(project) + "/public/compatible-releases.json" + query);
            std::string since = ctx.since;
            if (since.size() > 1 && since[0] == 'v' && std::isdigit(static_cast<unsigned char>(since[1]))) since.erase(0, 1);
            auto anchor = std::find_if(history.begin(), history.end(), [&](const json& release) { return str(release, "version") == since; });
            if (anchor == history.end()) throw std::runtime_error("--since version is not in the last 200 compatible releases; omit --since to resolve latest");
            r["releases"] = json::array();
            for (auto it = history.begin(); it != anchor; ++it) r["releases"].push_back(*it);
        }
    } else {
        http::Form query;
        if (ctx.prerelease) query.emplace_back("prerelease", "true");
        if (!ctx.since.empty()) query.emplace_back("since", ctx.since);
        const std::string qs = query.empty() ? "" : "?" + http::encode_query(query);
        try {
            r = rsys_public_client(ctx).get_public("/" + seg(project) + "/public/latest.json" + qs);
        } catch (const api::ApiError& e) {
            if (e.status != 404) throw;
            auto auth = rsys_authenticate(ctx, true);
            if (!auth) throw;
            r = auth->client.get_authed(project_path(project) + "/latest" + (ctx.prerelease ? "?prerelease=true" : ""), auth->session());
            from_public = false;
        }
    }
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    std::cout << '\n';
    print_release(project, r);
    // latest.json also carries the history: everything since --since, or the last few.
    if (from_public) {
        auto it = r.find("releases");
        if (it != r.end() && it->is_array() && !it->empty() && (compatible || it->size() > 1)) {
            term::section_begin(ctx.since.empty() ? "Recent releases" : "Changes since " + ctx.since);
            bool first = true;
            for (const auto& h : *it) {
                if (!first) std::cout << term::dim(term::g().vbar) << '\n';
                first = false;
                std::string head = term::bold(str(h, "version"));
                if (auto t = str(h, "title"); !t.empty()) head += " " + term::dim(term::g().dash) + " " + t;
                if (h.contains("publishedAt")) head += "  " + term::dim(format_time(h["publishedAt"]));
                std::cout << term::dim(term::g().vbar) << ' ' << head << '\n';
                if (auto c = str(h, "changelog"); !c.empty()) print_changelog(c);
            }
            term::section_end();
        }
    }
    return 0;
}

int rsys_download(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "download <project> [version] [--compat field=value] [--file <name>] [-o <path>]")) return 2;
    const std::string& project = ctx.args[0];
    std::string version = ctx.args.size() > 1 ? ctx.args[1] : "";

    if (!ctx.variants_file.empty()) throw std::invalid_argument("--variants-file is only supported by rsys publish");
    const bool compatible = !ctx.compatibility.empty();
    if (compatible && !is_latest(version)) throw std::invalid_argument("--compat resolves latest; omit the explicit release version");
    std::optional<json> release;
    std::optional<Auth> auth;
    api::Client pub = rsys_public_client(ctx);
    if (compatible) {
        auto resolved = resolve_compatible(ctx, project);
        release = std::move(resolved.release);
        auth = std::move(resolved.auth);
    } else {
        auth = rsys_authenticate(ctx, true);
        if (auth) {
            try {
                release = is_latest(version)
                    ? auth->client.get_authed(project_path(project) + "/latest" + (ctx.prerelease ? "?prerelease=true" : ""), auth->session())
                    : auth->client.get_authed(release_path(project, version), auth->session());
            } catch (const api::ApiError& e) {
                if (e.status != 403) throw;
                auth.reset();
            } catch (const api::Unauthorized&) { auth.reset(); }
        }
        if (!release) {
            std::string path = "/" + seg(project) + "/public/" +
                (is_latest(version) ? std::string("latest.json") + (ctx.prerelease ? "?prerelease=true" : "") : "v/" + seg(version) + ".json");
            release = pub.get_public(path);
        }
    }
    auto file = compatible && ctx.file.empty() ? std::optional<json>(std::in_place, release->at("primary")) : pick_file(*release, ctx.file);
    if (!file) {
        if (!ctx.file.empty()) term::error(project + " " + str(*release, "version") + " has no file called " + ctx.file + " in the selected build.");
        else term::error(project + " " + str(*release, "version") + " has no downloadable file.");
        return 1;
    }
    std::string name = str(*file, "filename");
    fs::path dest = download_target(ctx, name);
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    auto size = static_cast<std::uint64_t>(num(*file, "size"));
    std::string sha = str(*file, "sha256");

    if (compatible) {
        const std::string url = str(*file, "url");
        // The response has no private file IDs outside its URL. Restrict authenticated
        // transfers to RSYS download paths on the configured server, never another origin.
        std::smatch match;
        static const std::regex private_url("(?:https?://[^/?#]+)?(/api/v2/rsys/files/[0-9]+/download)");
        if (auth && std::regex_match(url, match, private_url)) {
            const auto path = match[1].str();
            fetch_into(dest, name, size, sha, [&](const http::Sink& sink, const http::Progress& progress) {
                auth->client.download(path, {}, auth->session(), sink, progress);
            });
        } else {
            fetch_into(dest, name, size, sha, [&](const http::Sink& sink, const http::Progress& progress) { pub.download_public(url, sink, progress); });
        }
    } else if (auth) {
        std::string id = str(*file, "id");
        fetch_into(dest, name, size, sha, [&](const http::Sink& sink, const http::Progress& progress) {
            auth->client.download(kRsys + "/files/" + id + "/download", {}, auth->session(), sink, progress);
        });
    } else {
        std::string url = str(*file, "url");
        fetch_into(dest, name, size, sha, [&](const http::Sink& sink, const http::Progress& progress) { pub.download_public(url, sink, progress); });
    }
    if (!dest.empty()) {
        term::success("Downloaded " + term::bold(project + " " + str(*release, "version")) + " " + term::dim(term::g().arrow) +
                      " " + utf8(dest) + " " + term::dim("(" + human_size(size) + (sha.empty() ? "" : ", sha256 verified") + ")"));
    }
    return 0;
}

int rsys_hash(const Context& ctx) {
    if (!rsys_usage(ctx, 1, "hash <file>...")) return 2;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        try {
            std::cout << sha256::of_file(fs::path(arg)) << "  " << arg << '\n';
        } catch (const std::exception& e) {
            term::error(e.what());
            rc = 1;
        }
    }
    return rc;
}

}  // namespace commands

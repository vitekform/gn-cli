// gn admin: the admin panel (AIAdminAPI, OAuthClientAdminAPI). Login session only: no CLI scope
// reaches /api/v2/admin, and every change asks first unless --yes.
#include <iostream>
#include <sstream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kAdmin = "/api/v2/admin";

// The ids AIAdminAPI.KNOWN_PERMISSIONS accepts, without the legacy page names.
const char* kPermissions[] = {"aiChatBasic", "aiRoleplayBasic", "aiRoleplayFull", "aiImagen", "aiTTS", "s3", "smartHome",
                              "gelbooru", "gelbooruAdmin", "aiAdmin", "openAIAPI", "khkAdmin", "katharsAdmin", "gallery",
                              "release_system", "game_player"};

std::optional<Auth> admin_session(const Context& ctx) {
    if (!ctx.token.empty()) {
        term::error("The admin commands need a login session, not a CLI token.");
        return std::nullopt;
    }
    return authenticate(ctx);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A user by id, email, or a piece of an email that fits only one (confirmations name the email).
std::optional<json> find_user(Auth& a, const std::string& who) {
    json users = a.client.get_authed(kAdmin + "/users", a.session());
    std::vector<json> partial;
    for (const auto& u : users) {
        if (std::to_string(static_cast<long long>(num(u, "id"))) == who || lower(str(u, "email")) == lower(who)) return json(u);
        if (lower(str(u, "email")).find(lower(who)) != std::string::npos) partial.push_back(u);
    }
    if (partial.size() == 1) return json(partial[0]);
    if (partial.empty()) {
        term::error("No user " + who + " (an id or an email).");
    } else {
        term::error("'" + who + "' fits " + std::to_string(partial.size()) + " users:");
        for (const auto& u : partial) std::cerr << "  " << pad_left(std::to_string(static_cast<long long>(num(u, "id"))), 5) << "  " << str(u, "email") << '\n';
    }
    return std::nullopt;
}

std::string uid(const json& u) { return std::to_string(static_cast<long long>(num(u, "id"))); }

std::string who(const json& u) { return term::bold(str(u, "email")) + term::dim(" (#" + uid(u) + ")"); }

std::vector<std::string> split_list(const std::vector<std::string>& args, std::size_t from) {
    std::vector<std::string> out;
    for (std::size_t i = from; i < args.size(); ++i) {
        std::stringstream in(args[i]);
        std::string item;
        while (std::getline(in, item, ','))
            if (!trim(item).empty()) out.push_back(trim(item));
    }
    return out;
}

bool known_permission(const std::string& p) {
    for (const char* k : kPermissions)
        if (p == k) return true;
    return false;
}

std::string access_text(const json& u) {
    if (!u.value("customPermissionsEnabled", false))
        return "role " + str(u, "role", "?") + " (level " + std::to_string(static_cast<int>(num(u, "level"))) + ")";
    auto pages = string_list(u, "allowedPages");
    return "only " + (pages.empty() ? std::string("nothing") : join(pages, ", "));
}

// A new password, typed twice, or one line of stdin with --password-stdin.
std::optional<std::string> read_new_password(const Context& ctx) {
    std::string pw, again;
    if (ctx.password_stdin) {
        if (!std::getline(std::cin, pw) || pw.empty()) {
            term::error("No password on stdin.");
            return std::nullopt;
        }
        if (pw.back() == '\r') pw.pop_back();
        return pw;
    }
    if (!term::prompt_secret("New password:", pw) || pw.empty()) {
        missing_input("No password given.", "pipe it in with --password-stdin");
        return std::nullopt;
    }
    if (!term::prompt_secret("Again:", again) || again != pw) {
        term::error("The two passwords differ.");
        return std::nullopt;
    }
    return pw;
}

void print_usage_rows(const json& usage, bool with_user) {
    const auto& G = term::g();
    for (const auto& u : usage) {
        std::cout << term::dim(G.vbar) << ' ' << pad_right(u.contains("createdAt") ? format_time(u["createdAt"]) : none(), 21);
        if (with_user) std::cout << pad_right(truncate(str(u, "userEmail"), 24), 26);
        std::cout << term::cyan(pad_right(truncate(str(u, "modelName"), 22), 24))
                  << pad_left(with_thousands(static_cast<long long>(num(u, "promptTokens"))), 9)
                  << pad_left(with_thousands(static_cast<long long>(num(u, "completionTokens"))), 9)
                  << term::yellow(pad_left(money(num(u, "costUsd")), 11)) << '\n';
    }
}

}  // namespace

int admin_users(const Context& ctx) {
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    json users = auth->client.get_authed(kAdmin + "/users", auth->session());
    std::string filter = ctx.args.empty() ? "" : lower(ctx.args[0]);
    json shown = json::array();
    for (const auto& u : users)
        if (filter.empty() || lower(str(u, "email")).find(filter) != std::string::npos) shown.push_back(u);
    if (ctx.json) {
        std::cout << shown.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Users (" + std::to_string(shown.size()) + (filter.empty() ? "" : " matching " + filter) + ")");
    for (const auto& u : shown)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(uid(u), 5) << "  " << pad_right(truncate(str(u, "email"), 34), 36)
                  << pad_left(money(num(u, "creditBalance"), 2), 10) << "  " << term::dim(truncate(access_text(u), 40)) << '\n';
    term::section_end();
    return 0;
}

int admin_user(const Context& ctx) {
    if (!need_args(ctx, 1, "admin user <id|email>")) return 2;
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    auto u = find_user(*auth, ctx.args[0]);
    if (!u) return 1;
    json credits = auth->client.get_authed(kAdmin + "/users/" + uid(*u) + "/credits", auth->session());
    if (ctx.json) {
        (*u)["credits"] = credits;
        std::cout << u->dump(2) << '\n';
        return 0;
    }
    term::section_begin(str(*u, "email"));
    term::row("Id", uid(*u));
    term::row("Access", access_text(*u));
    term::section_end();
    print_credits(credits, 20);
    return 0;
}

int admin_create_user(const Context& ctx) {
    if (!need_args(ctx, 2, "admin create-user <email> <permission>[,<permission>...]")) return 2;
    std::vector<std::string> perms = split_list(ctx.args, 1);
    for (const auto& p : perms)
        if (!known_permission(p)) {
            term::error("Unknown permission " + p + ". Known: " + join(std::vector<std::string>(std::begin(kPermissions), std::end(kPermissions)), ", "));
            return 2;
        }
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    auto pw = read_new_password(ctx);
    if (!pw) return 1;
    if (!confirm(ctx, "Create " + ctx.args[0] + " with access to only " + join(perms, ", ") + "?")) return 1;
    json body = {{"email", ctx.args[0]}, {"password", *pw}, {"allowedPages", perms}};
    json u = auth->client.call("POST", kAdmin + "/users", auth->session(), &body);
    term::success("Created " + who(u) + " with access to " + join(string_list(u, "allowedPages"), ", ") + ".");
    return 0;
}

// admin perms <user>                      show
// admin perms <user> set <perm>[,…]       exactly these (turns custom permissions on)
// admin perms <user> role                 back to what the role grants
int admin_perms(const Context& ctx) {
    if (!need_args(ctx, 1, "admin perms <id|email> [set <permission>,... | role]")) return 2;
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    auto u = find_user(*auth, ctx.args[0]);
    if (!u) return 1;
    std::string action = ctx.args.size() > 1 ? ctx.args[1] : "";
    if (action.empty()) {
        std::cout << who(*u) << ": " << access_text(*u) << '\n';
        return 0;
    }
    json body;
    std::string after;
    if (action == "set") {
        std::vector<std::string> perms = split_list(ctx.args, 2);
        if (perms.empty()) {
            term::error("set needs at least one permission.");
            return 2;
        }
        for (const auto& p : perms)
            if (!known_permission(p)) {
                term::error("Unknown permission " + p + ".");
                return 2;
            }
        body = {{"customPermissionsEnabled", true}, {"allowedPages", perms}};
        after = "only " + join(perms, ", ");
    } else if (action == "role") {
        body = {{"customPermissionsEnabled", false}};
        after = "what the role " + str(*u, "role", "?") + " grants";
    } else {
        term::error("admin perms takes set <permissions> or role, not " + action + ".");
        return 2;
    }
    std::cout << "  " << term::dim("now:   ") << access_text(*u) << "\n  " << term::dim("after: ") << after << '\n';
    if (!confirm(ctx, "Change the access of " + str(*u, "email") + "?")) return 1;
    auth->client.call("PUT", kAdmin + "/users/" + uid(*u) + "/permissions", auth->session(), &body);
    term::success("Updated " + who(*u) + ".");
    return 0;
}

int admin_password(const Context& ctx) {
    if (!need_args(ctx, 1, "admin password <id|email>")) return 2;
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    auto u = find_user(*auth, ctx.args[0]);
    if (!u) return 1;
    auto pw = read_new_password(ctx);
    if (!pw) return 1;
    if (!confirm(ctx, "Reset the password of " + str(*u, "email") + "? Their sessions are signed out.")) return 1;
    json body = {{"password", *pw}};
    auth->client.call("PUT", kAdmin + "/users/" + uid(*u) + "/password", auth->session(), &body);
    term::success("New password set for " + who(*u) + ".");
    return 0;
}

// admin credits <user>                  show
// admin credits <user> set <amount>     exact balance
// admin credits <user> add|sub <amount> adjust
int admin_credits(const Context& ctx) {
    if (!need_args(ctx, 1, "admin credits <id|email> [set|add|sub <amount>]")) return 2;
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    auto u = find_user(*auth, ctx.args[0]);
    if (!u) return 1;
    std::string path = kAdmin + "/users/" + uid(*u) + "/credits";
    if (ctx.args.size() < 2) {
        json c = auth->client.get_authed(path, auth->session());
        if (ctx.json) std::cout << c.dump(2) << '\n';
        else print_credits(c, 20);
        return 0;
    }
    std::string action = ctx.args[1];
    if (ctx.args.size() < 3) {
        term::error(action + " needs an amount.");
        return 2;
    }
    double amount = 0;
    try {
        std::size_t used = 0;
        amount = std::stod(ctx.args[2], &used);
        if (used != ctx.args[2].size() || amount < 0) throw std::invalid_argument("amount");
    } catch (const std::exception&) {
        term::error("Not an amount: " + ctx.args[2]);
        return 2;
    }
    double before = num(*u, "creditBalance");
    json body;
    std::string verb;
    double after = before;
    if (action == "set") {
        body = {{"balance", amount}};
        after = amount;
        verb = "PUT";
    } else if (action == "add" || action == "sub") {
        double delta = action == "add" ? amount : -amount;
        body = {{"amount", delta}};
        path += "/adjust";
        after = before + delta;
        verb = "POST";
    } else {
        term::error("admin credits takes set, add or sub, not " + action + ".");
        return 2;
    }
    if (!confirm(ctx, "Change the balance of " + str(*u, "email") + " from " + money(before, 2) + " to " + money(after, 2) + "?"))
        return 1;
    json r = auth->client.call(verb, path, auth->session(), &body);
    term::success(who(*u) + " now has " + term::bold(money(num(r, "creditBalance"), 2)) + ".");
    return 0;
}

int admin_usage(const Context& ctx) {
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    json usage = auth->client.get_authed(kAdmin + "/usage", auth->session());
    if (ctx.json) {
        std::cout << usage.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Recent usage, everyone (" + std::to_string(usage.size()) + ")");
    print_usage_rows(usage, true);
    term::section_end();
    return 0;
}

int admin_providers(const Context& ctx) {
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    json providers = auth->client.get_authed(kAdmin + "/providers", auth->session());
    if (ctx.json) {
        std::cout << providers.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Model providers (" + std::to_string(providers.size()) + ")");
    for (const auto& p : providers) {
        std::string key = p.value("hasApiKey", false) ? term::green("key …" + str(p, "apiKeyHint")) : term::red("no key");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(uid(p), 4) << "  " << pad_right(truncate(str(p, "name"), 22), 24)
                  << pad_left(std::to_string(static_cast<int>(num(p, "modelCount"))), 4) << term::dim(" models") << "  "
                  << pad_right(key, 14) << ' ' << term::dim(truncate(str(p, "endpoint"), 40)) << '\n';
    }
    term::section_end();
    return 0;
}

int admin_models(const Context& ctx) {
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    json models = auth->client.get_authed(kAdmin + "/models", auth->session());
    if (ctx.json) {
        std::cout << models.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Models, usable or not (" + std::to_string(models.size()) + ")");
    for (const auto& m : models) {
        std::string flags = std::string(m.value("visionCapable", false) ? " vision" : "") + (m.value("toolingSupported", false) ? " tools" : "") +
                            (m.value("roleplayCompatible", false) ? " roleplay" : "");
        double in = num(m, "pricePerPromptToken") * 1e6, out = num(m, "pricePerCompletionToken") * 1e6;
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(uid(m), 4) << "  "
                  << (m.value("usable", false) ? term::green(term::g().ok) : term::red(term::g().fail)) << ' '
                  << pad_right(truncate(str(m, "name"), 34), 36) << pad_right(truncate(str(m, "providerName"), 14), 16)
                  << pad_left(in == 0 && out == 0 ? "free" : money(in, 2) + "/" + money(out, 2), 16) << term::dim(flags) << '\n';
    }
    term::section_end();
    return 0;
}

int admin_oauth_clients(const Context& ctx) {
    auto auth = admin_session(ctx);
    if (!auth) return 1;
    json clients = auth->client.get_authed(kAdmin + "/oauth-clients", auth->session());
    if (ctx.json) {
        std::cout << clients.dump(2) << '\n';
        return 0;
    }
    term::section_begin("OAuth clients (" + std::to_string(clients.size()) + ")");
    for (const auto& c : clients) {
        std::string state = !str(c, "revokedAt").empty() ? term::red("revoked") : c.value("enabled", false) ? term::green("enabled") : term::yellow("disabled");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(truncate(str(c, "name"), 24), 26) << pad_right(state, 10)
                  << term::dim(str(c, "clientId")) << (c.value("trusted", false) ? term::cyan(" trusted") : "")
                  << (c.value("publicClient", false) ? term::dim(" public") : "") << '\n'
                  << term::dim(term::g().vbar) << "   " << term::dim(join(string_list(c, "redirectUris"), "  ")) << '\n';
    }
    term::section_end();
    return 0;
}

}  // namespace commands

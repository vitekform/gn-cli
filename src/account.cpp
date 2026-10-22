// gn api-tokens (UserApiTokenAPI), gn credits (AuthAPI /me/credits), gn passkeys (PasskeyAPI).
#include <chrono>
#include <iostream>
#include <thread>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kApiTokens = "/api/v2/api-tokens";
// Where the OpenAI-compatible gateway answers (nextgen_frontend_ng api-tokens-api.ts).
const char* kGateway = "https://oai.ganamaga.me/v1";

std::optional<long long> parse_id(const std::string& s) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::stoll(s);
}

}  // namespace

// ── api-tokens ──────────────────────────────────────────────────────────────

int api_tokens_list(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json tokens = auth->client.get_authed(kApiTokens, auth->session());
    if (ctx.json) {
        std::cout << tokens.dump(2) << '\n';
        return 0;
    }
    term::section_begin("OpenAI-compatible API tokens (" + std::to_string(tokens.size()) + ")");
    if (tokens.empty()) term::row("", term::dim("none; create one with gn api-tokens create <name>"), 0);
    for (const auto& t : tokens)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(t, "id"))), 5) << "  "
                  << pad_right(truncate(str(t, "name"), 34), 36) << term::dim(t.contains("createdAt") ? format_time(t["createdAt"]) : "")
                  << '\n';
    term::section_end();
    std::cout << term::dim("  Gateway: " + std::string(kGateway)) << '\n';
    return 0;
}

int api_tokens_create(const Context& ctx) {
    if (!need_args(ctx, 1, "api-tokens create <name>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json body = {{"name", join(ctx.args, " ")}};
    json t = auth->client.call("POST", kApiTokens, auth->session(), &body);
    std::string key = str(t, "token");
    if (ctx.json) {
        t["baseUrl"] = kGateway;
        std::cout << t.dump(2) << '\n';
        return 0;
    }
    if (!term::stdout_is_tty()) {  // key=$(gn api-tokens create ci)
        std::cout << key << '\n';
        return 0;
    }
    term::success("Created API token " + term::bold(str(t, "name")) + " " +
                  term::dim("(id " + std::to_string(static_cast<long long>(num(t, "id"))) + ")"));
    std::cout << "\n  " << term::bold(term::yellow(key)) << "\n\n"
              << term::dim("  Shown only now; the server keeps just its hash. For OpenAI clients:") << "\n"
              << "  export OPENAI_BASE_URL=" << kGateway << "\n"
              << "  export OPENAI_API_KEY=" << key << "\n";
    return 0;
}

int api_tokens_revoke(const Context& ctx) {
    if (!need_args(ctx, 1, "api-tokens revoke <id>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        if (!parse_id(arg)) {
            term::error("Not a token id: " + arg + " (gn api-tokens ls lists them)");
            rc = 2;
            continue;
        }
        auth->client.call("DELETE", kApiTokens + "/" + arg, auth->session());
        term::success("Revoked API token " + arg + "; programs using it stop working now.");
    }
    return rc;
}

int api_tokens_models(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json models = auth->client.get_authed(kApiTokens + "/models", auth->session());
    if (ctx.json) {
        std::cout << models.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Models on the gateway (" + std::to_string(models.size()) + ")");
    for (const auto& m : models) {
        double in = num(m, "pricePerPromptToken") * 1e6, out = num(m, "pricePerCompletionToken") * 1e6;
        std::string price = in == 0 && out == 0 ? term::green("free") : money(in, 2) + " / " + money(out, 2);
        std::string flags = std::string(m.value("visionCapable", false) ? " vision" : "") + (m.value("toolingSupported", false) ? " tools" : "");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(truncate(str(m, "name"), 40), 42) << pad_left(price, 16)
                  << term::dim(flags) << '\n';
    }
    term::section_end();
    std::cout << term::dim("  Pass the name as \"model\" to " + std::string(kGateway) + "/chat/completions; prices per million tokens, in / out.")
              << '\n';
    return 0;
}

// ── credits ─────────────────────────────────────────────────────────────────

int credits(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto fetch = [&] { return auth->client.get_authed("/api/v2/auth/me/credits", auth->session()); };
    if (ctx.json) {
        std::cout << fetch().dump(2) << '\n';
        return 0;
    }
    if (!ctx.watch) {
        print_credits(fetch(), 30);
        return 0;
    }
    // --watch: redraw every 10 s until Ctrl-C.
    CtrlC ctrl_c;
    while (!ctrl_c.pressed()) {
        json c = fetch();
        term::clear_screen();
        print_credits(c, 20);
        std::cout << term::dim("  Refreshing every 10 s; Ctrl-C to stop.") << std::flush;
        for (int i = 0; i < 100 && !ctrl_c.pressed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << '\n';
    return 0;
}

// ── passkeys ────────────────────────────────────────────────────────────────

int passkeys_list(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json keys = auth->client.get_authed("/api/v2/auth/passkey/list", auth->session());
    if (ctx.json) {
        std::cout << keys.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Passkeys (" + std::to_string(keys.size()) + ")");
    if (keys.empty()) term::row("", term::dim("none; register one in the web app's settings"), 0);
    for (const auto& k : keys)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(k, "id"))), 5) << "  "
                  << pad_right(truncate(str(k, "label", "unnamed"), 30), 32) << pad_right(str(k, "algorithm"), 8)
                  << term::dim(k.contains("createdAt") ? format_time(k["createdAt"]) : "") << '\n';
    term::section_end();
    return 0;
}

// PasskeyAPI takes the session as form fields rather than headers, so this needs a login session.
int passkeys_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "passkeys rm <id>...")) return 2;
    if (!ctx.token.empty()) {
        term::error("Removing passkeys needs a login session, not a CLI token.");
        return 1;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    // A first authenticated call renews the session if it went stale, before its token goes into the form.
    json keys = auth->client.get_authed("/api/v2/auth/passkey/list", auth->session());
    int rc = 0;
    for (const auto& arg : ctx.args) {
        auto id = parse_id(arg);
        std::string label;
        for (const auto& k : keys)
            if (static_cast<long long>(num(k, "id")) == id.value_or(-1)) label = str(k, "label", "unnamed");
        if (!id || label.empty()) {
            term::error("No passkey with id " + arg + " (gn passkeys ls lists them)");
            rc = 1;
            continue;
        }
        if (!confirm(ctx, "Remove the passkey " + label + "? You won't be able to sign in with it any more.")) {
            rc = 1;
            continue;
        }
        const config::Credentials& c = *auth->session();
        auth->client.post_form("/api/v2/auth/passkey/delete",
                               {{"userId", std::to_string(c.user_id)}, {"token", c.token}, {"passkeyId", arg}}, auth->session());
        term::success("Removed the passkey " + label + ".");
    }
    return rc;
}

}  // namespace commands

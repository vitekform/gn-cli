// The AI's read-only tools from the command line: the central log, memories, web search.
//
// The backend runs the very tool a chat would call (POST /api/v2/ai/tools/{name}/run), with the
// same checks, and lists the ones this user may run with their parameters (GET /api/v2/ai/tools).
// So this file stays generic — `gn tool <name> key=value…` runs any of them, `gn tools <name>`
// shows a tool's parameters from the server — and `gn logs`, `gn memory` and `gn web` are short
// forms of it. A tool the backend adds later works here without a new release.
//
// In the AI sandbox, the VM's gn runs these with the token sandboxd injects; the chat a command
// belongs to comes from $GN_SANDBOX_CHAT (the memory tools need it), unless --chat names one.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {

namespace {

const std::string kTools = "/api/v2/ai/tools";

// key=value arguments as a JSON object. A value is JSON when it parses as such — 20, true,
// ["a","b"], {"x":1}, "quoted" — and a plain string otherwise, so where="status >= 500" and
// request_id=abc just work.
bool parse_key_values(const std::vector<std::string>& words, json& out, std::string& error) {
    out = json::object();
    for (const auto& w : words) {
        auto eq = w.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "expected key=value, got '" + w + "'";
            return false;
        }
        std::string key = w.substr(0, eq);
        std::string raw = w.substr(eq + 1);
        json value = raw;
        if (!raw.empty()) {
            char c = raw[0];
            bool looks_json = c == '[' || c == '{' || c == '"' || c == '-' || (c >= '0' && c <= '9') ||
                              raw == "true" || raw == "false" || raw == "null";
            if (looks_json) {
                try {
                    value = json::parse(raw);
                } catch (const json::exception&) {
                    value = raw;  // 2026-11-05 and friends: not JSON, a string
                }
            }
        }
        out[key] = value;
    }
    return true;
}

// The chat to run in: --chat, else the sandbox's own ($GN_SANDBOX_CHAT), else none.
std::string chat_for(const Context& ctx) {
    if (!ctx.chat.empty()) return ctx.chat;
    if (const char* env = std::getenv("GN_SANDBOX_CHAT")) return env;
    return "";
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

int run_tool(const Context& ctx, const std::string& name, json args) {
    // --limit is a gn option elsewhere; here it is the tool's own limit/count, if it takes one.
    if (!ctx.limit.empty() && !args.contains("limit") && !args.contains("count")) {
        try {
            args["limit"] = std::stoi(ctx.limit);
        } catch (...) {
            std::cerr << "--limit must be a number\n";
            return 2;
        }
    }
    std::string path = kTools + "/" + name + "/run";
    std::string chat = chat_for(ctx);
    if (!chat.empty()) {
        if (!all_digits(chat)) {
            std::cerr << "--chat (or $GN_SANDBOX_CHAT) must be a chat id, not '" << chat << "'\n";
            return 2;
        }
        path += "?chatId=" + chat;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json r = auth->client.call("POST", path, auth->session(), &args);
    if (ctx.json) {
        std::cout << r.dump(2) << '\n';
        return 0;
    }
    std::string result = str(r, "result");
    std::cout << result;
    if (result.empty() || result.back() != '\n') std::cout << '\n';
    return 0;
}

int run_with_key_values(const Context& ctx, const std::string& name, const std::vector<std::string>& words) {
    json args;
    std::string error;
    if (!parse_key_values(words, args, error)) {
        std::cerr << error << " (gn tools " << name << " lists the parameters)\n";
        return 2;
    }
    return run_tool(ctx, name, args);
}

std::string first_sentence(const std::string& text) {
    auto end = text.find(". ");
    std::string s = end == std::string::npos ? text : text.substr(0, end + 1);
    if (s.size() > 110) s = s.substr(0, 107) + "...";
    return s;
}

void print_parameters(const json& tool) {
    const auto& G = term::g();
    json params = tool.contains("parameters") ? tool["parameters"] : json::object();
    json props = params.contains("properties") ? params["properties"] : json::object();
    std::vector<std::string> required;
    if (params.contains("required") && params["required"].is_array())
        for (const auto& r : params["required"]) required.push_back(r.get<std::string>());
    if (props.empty()) {
        std::cout << term::dim(G.vbar) << " (no parameters)\n";
        return;
    }
    for (auto it = props.begin(); it != props.end(); ++it) {
        const json& p = it.value();
        bool req = std::find(required.begin(), required.end(), it.key()) != required.end();
        std::string type = p.contains("type") && p["type"].is_string() ? p["type"].get<std::string>() : "";
        std::cout << term::dim(G.vbar) << ' ' << term::green(term::bold(it.key())) << ' ' << term::dim(type)
                  << (req ? " (required)" : "") << '\n';
        if (p.contains("enum") && p["enum"].is_array()) {
            std::vector<std::string> values;
            for (const auto& v : p["enum"]) values.push_back(v.is_string() ? v.get<std::string>() : v.dump());
            std::cout << term::dim(G.vbar) << "     one of: " << join(values, ", ") << '\n';
        }
        std::string d = str(p, "description");
        if (!d.empty()) std::cout << term::dim(G.vbar) << "     " << d << '\n';
    }
}

}  // namespace

// gn tools [name]: what this user may run, or one tool's description and parameters.
int ai_tools(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json tools = auth->client.get_authed(kTools, auth->session());
    if (ctx.args.empty()) {
        if (ctx.json) {
            std::cout << tools.dump(2) << '\n';
            return 0;
        }
        if (tools.empty()) {
            std::cout << "No AI tools you can run from here (they need ai.tools.* nodes, and a CLI token the "
                         "logs:read, memory:read or web:search scope).\n";
            return 0;
        }
        const auto& G = term::g();
        std::cout << '\n';
        term::section_begin("AI tools");
        for (const auto& t : tools)
            std::cout << term::dim(G.vbar) << ' ' << term::green(term::bold(pad_right(str(t, "name"), 22)))
                      << first_sentence(str(t, "description")) << '\n';
        term::section_end();
        std::cout << "\n" << term::dim("gn tools <name> for its parameters; gn tool <name> key=value... to run it") << '\n';
        return 0;
    }
    const std::string& wanted = ctx.args[0];
    for (const auto& t : tools) {
        if (str(t, "name") != wanted) continue;
        if (ctx.json) {
            std::cout << t.dump(2) << '\n';
            return 0;
        }
        const auto& G = term::g();
        std::cout << '\n';
        term::section_begin(wanted);
        std::cout << term::dim(G.vbar) << ' ' << str(t, "description") << '\n' << term::dim(G.vbar) << '\n';
        print_parameters(t);
        term::section_end();
        std::cout << "\n" << term::dim("gn tool " + wanted + " key=value...  (values are JSON when they parse)") << '\n';
        return 0;
    }
    std::cerr << "No tool '" << wanted << "' you can run (gn tools lists them)\n";
    return 1;
}

// gn tool <name> [key=value…]
int ai_tool(const Context& ctx) {
    if (!need_args(ctx, 1, "gn tool <name> [key=value...]")) return 2;
    std::vector<std::string> rest(ctx.args.begin() + 1, ctx.args.end());
    return run_with_key_values(ctx, ctx.args[0], rest);
}

// gn logs <search|count|trace|sources|booru> [key=value…]: the logs_* tools.
namespace {
int logs(const Context& ctx, const std::string& tool) { return run_with_key_values(ctx, tool, ctx.args); }
}  // namespace
int logs_search(const Context& ctx) { return logs(ctx, "logs_search"); }
int logs_count(const Context& ctx) { return logs(ctx, "logs_count"); }
int logs_trace(const Context& ctx) { return logs(ctx, "logs_trace"); }
int logs_sources(const Context& ctx) { return logs(ctx, "logs_sources"); }
int logs_booru(const Context& ctx) { return logs(ctx, "logs_booru_activity"); }

// gn memory list | read <id> | read-chat <id>
int memory_list(const Context& ctx) { return run_tool(ctx, "list_memories", json::object()); }

namespace {
int memory_read_scope(const Context& ctx, const std::string& tool, const std::string& synopsis) {
    if (!need_args(ctx, 1, synopsis)) return 2;
    std::string id = ctx.args[0];
    if (!id.empty() && id[0] == '#') id = id.substr(1);
    if (!all_digits(id)) {
        std::cerr << "a memory id is a number, not '" << ctx.args[0] << "'\n";
        return 2;
    }
    return run_tool(ctx, tool, json{{"id", std::stoll(id)}});
}
}  // namespace
int memory_read(const Context& ctx) { return memory_read_scope(ctx, "read_memory_global", "gn memory read <id>"); }
int memory_read_chat(const Context& ctx) {
    return memory_read_scope(ctx, "read_memory_chat", "gn memory read-chat <id>");
}

// gn web search <query…> | fetch <url…> | research <query…>
int web_search(const Context& ctx) {
    if (!need_args(ctx, 1, "gn web search <query...>")) return 2;
    json args{{"query", join(ctx.args, " ")}};
    if (!ctx.limit.empty()) {
        try {
            args["count"] = std::stoi(ctx.limit);
        } catch (...) {
            std::cerr << "--limit must be a number\n";
            return 2;
        }
    }
    Context plain = ctx;
    plain.limit.clear();
    return run_tool(plain, "search_web", args);
}

int web_fetch(const Context& ctx) {
    if (!need_args(ctx, 1, "gn web fetch <url> [<url>...]")) return 2;
    return run_tool(ctx, "fetch_web_page", json{{"urls", ctx.args}});
}

int web_research(const Context& ctx) {
    if (!need_args(ctx, 1, "gn web research <query...>")) return 2;
    return run_tool(ctx, "research_web", json{{"query", join(ctx.args, " ")}});
}

}  // namespace commands

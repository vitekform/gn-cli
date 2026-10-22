// gn rp: roleplay chats (AIRoleplayAPI) in the terminal. Characters, personas and background
// cards are made in the web app (they are mostly pictures); here you pick them and talk.
//
// Every roleplay message is a {"messages":[{"character_name","content"}]} envelope, streamed raw
// while the model writes it and repaired by the server in the final event — so a reply shows a
// "writing…" line while it streams and is printed, speaker by speaker, once it is complete.
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "lineedit.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kRp = "/api/v2/ai/roleplay";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string id_of(const json& o) { return std::to_string(static_cast<long long>(num(o, "id"))); }

// "Finn, Mira" → {"Finn", "Mira"}
std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c != ',') {
            item += c;
            continue;
        }
        if (!trim(item).empty()) out.push_back(trim(item));
        item.clear();
    }
    return out;
}

// The lines of one stored or generated message: [(speaker, text)].
std::vector<std::pair<std::string, std::string>> lines_of(const std::string& content, const std::string& fallback) {
    std::vector<std::pair<std::string, std::string>> out;
    json env = json::parse(content, nullptr, false);
    if (env.is_object() && env.contains("messages") && env["messages"].is_array()) {
        for (const auto& m : env["messages"]) out.emplace_back(str(m, "character_name", fallback), str(m, "content"));
    } else if (!content.empty()) {
        out.emplace_back(fallback, content);
    }
    return out;
}

void print_lines(const std::vector<std::pair<std::string, std::string>>& lines, bool user) {
    for (const auto& [who, text] : lines)
        std::cout << (user ? term::bold(term::green(who)) : term::bold(term::cyan(who))) << term::dim(": ") << text << "\n\n";
}

// Prints a stored message the way the chat shows it; tool proposals as a note.
void print_message(const json& m, const std::string& persona) {
    std::string author = str(m, "author");
    if (author == "TOOL_PROPOSAL") {
        term::info("The model proposed " + term::bold(str(m, "toolName")) + "; act on it in the web app.");
        return;
    }
    bool user = author == "USER";
    std::string fallback = str(m, "senderName", user ? persona : "?");
    print_lines(lines_of(str(m, "content"), fallback), user);
}

// Streams one generation (a sent message or a regenerate), showing progress, then prints the
// finished turn. Returns the final event, or null after an error or Ctrl-C.
json stream_turn(Auth& a, const std::string& path, const http::Form& form, const std::string& fallback_speaker) {
    json done;
    bool live = term::stderr_is_tty();
    std::size_t chars = 0;
    std::string status;
    CtrlC ctrl_c;
    auto show = [&](const std::string& s) {
        if (!live || s == status) return;
        std::cerr << '\r' << term::dim(s) << "\x1b[K" << std::flush;
        status = s;
    };
    show("thinking" + std::string(term::g().ellipsis));
    a.client.stream_events(path, form, a.session(), [&](const std::string& data) {
        if (ctrl_c.pressed()) return false;
        json e = json::parse(data, nullptr, false);
        if (!e.is_object()) return true;
        if (e.contains("done")) {
            done = e;
        } else if (auto it = e.find("error"); it != e.end()) {
            if (live) std::cerr << "\r\x1b[K";
            term::error(it->is_string() ? it->get<std::string>() : it->dump());
            status.clear();
        } else if (auto c = e.find("content"); c != e.end() && c->is_string()) {
            chars += c->get<std::string>().size();
            show("writing" + std::string(term::g().ellipsis) + " " + std::to_string(chars) + " characters");
        } else if (auto t = e.find("toolCall"); t != e.end() && str(*t, "status") == "running") {
            show("running " + str(*t, "name") + term::g().ellipsis);
        } else if (auto p = e.find("toolProposal"); p != e.end()) {
            if (live) std::cerr << "\r\x1b[K";
            term::info("The model proposes " + term::bold(str(*p, "name")) + "; act on it in the web app.");
            status.clear();
        }
        return true;
    });
    if (live) std::cerr << "\r\x1b[K" << std::flush;
    if (ctrl_c.pressed()) {
        std::cerr << term::dim("(stopped; nothing was saved for this reply)") << '\n';
        return nullptr;
    }
    if (done.is_null()) return nullptr;
    print_lines(lines_of(str(done, "content"), fallback_speaker), false);
    return done;
}

json chat_detail(Auth& a, const std::string& id) { return a.client.get_authed(kRp + "/chats/" + id, a.session()); }

std::string cast_of(const json& chat) {
    std::vector<std::string> names;
    for (const auto& c : chat.value("characters", json::array())) names.push_back(str(c, "name"));
    return join(names, ", ");
}

std::string persona_of(const json& chat) {
    return chat.contains("userCharacter") && chat["userCharacter"].is_object() ? str(chat["userCharacter"], "name", "You") : "You";
}

// Finds characters (or personas) by id or name in a listing.
std::optional<json> find_named(const json& list, const std::string& who, const char* what) {
    std::vector<json> partial;
    for (const auto& x : list) {
        if (id_of(x) == who || lower(str(x, "name")) == lower(who)) return json(x);
        if (lower(str(x, "name")).find(lower(who)) != std::string::npos) partial.push_back(x);
    }
    if (partial.size() == 1) return json(partial[0]);
    term::error(partial.empty() ? std::string("No ") + what + " " + who + "."
                                : "'" + who + "' fits " + std::to_string(partial.size()) + " " + what + "s; say which.");
    return std::nullopt;
}

int session(Auth& a, json chat) {
    using namespace term;
    std::string id = id_of(chat);
    std::string persona = persona_of(chat);
    std::string first = chat.value("characters", json::array()).empty() ? "?" : str(chat["characters"][0], "name");
    std::cout << bold(cyan(str(chat, "name"))) << ' ' << dim("(chat " + id + ", " + str(chat, "modelName") + ")") << ' ' << dim(g().dash)
              << ' ' << dim("/help for commands, /exit to leave") << "\n\n";
    auto messages = chat.value("messages", json::array());
    std::size_t from = messages.size() > 6 ? messages.size() - 6 : 0;
    if (from) std::cout << dim("… " + std::to_string(from) + " earlier messages (/history shows more)") << "\n\n";
    for (std::size_t i = from; i < messages.size(); ++i) print_message(messages[i], persona);

    const fs::path history = config::dir() / "rp-history";
    lineedit::history_load(history);
    const std::string prompt = bold(green(persona)) + "> ";
    for (;;) {
        std::string line;
        if (!lineedit::read_line(prompt, line)) {
            std::cout << '\n';
            break;
        }
        std::string text = trim(line);
        if (text.empty()) continue;
        lineedit::history_add(line);
        lineedit::history_save(history);
        try {
            if (text == "/exit" || text == "/quit" || text == "/q") break;
            if (text == "/help") {
                std::cout << "  /regen     write the last reply again\n  /history   the whole conversation so far\n"
                          << "  /clear     clear the screen\n  /exit      leave (also Ctrl-D)\n"
                          << dim("  Ctrl-C stops a reply. Narration goes in *asterisks*, like in the web app.") << '\n';
                continue;
            }
            if (text == "/clear") {
                clear_screen();
                continue;
            }
            if (text == "/history") {
                for (const auto& m : chat_detail(a, id).value("messages", json::array())) print_message(m, persona);
                continue;
            }
            if (text == "/regen") {
                auto all = chat_detail(a, id).value("messages", json::array());
                std::string last;
                for (const auto& m : all)
                    if (str(m, "author") != "USER") last = id_of(m);
                if (last.empty()) {
                    error("Nothing to write again yet.");
                    continue;
                }
                stream_turn(a, kRp + "/messages/" + last + "/regenerate", {}, first);
                continue;
            }
            if (text[0] == '/') {
                error("Unknown command " + text + "; /help lists them.");
                continue;
            }
            std::cout << '\n';
            json done = stream_turn(a, kRp + "/chats/" + id + "/messages", {{"content", text}}, first);
            if (!done.is_null() && stderr_is_tty()) {
                long long in = static_cast<long long>(num(done, "promptTokens")), window = static_cast<long long>(num(done, "contextLength"));
                if (in > 0 && window > 0) std::cerr << dim(with_thousands(in) + " tokens, " + std::to_string(in * 100 / window) + "% of the context") << '\n';
            }
        } catch (const api::ApiError& e) {
            error(std::string("The server said no: ") + e.what());
        } catch (const http::TransportError& e) {
            error(std::string("Could not reach the server: ") + e.what());
        }
    }
    return 0;
}

}  // namespace

int rp_characters(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    // Copies of a character living inside one chat (chatId set) are that chat's, not the library's.
    json list = json::array();
    for (const auto& c : auth->client.get_authed(kRp + "/characters", auth->session()))
        if (!c.contains("chatId") || !c["chatId"].is_number()) list.push_back(c);
    if (ctx.json) {
        std::cout << list.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Characters (" + std::to_string(list.size()) + ")");
    for (const auto& c : list) {
        std::vector<std::string> about;
        for (const char* key : {"age", "gender"})
            if (!str(c, key).empty()) about.push_back(str(c, key));
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(c), 5) << "  " << pad_right(term::bold(str(c, "name")), 22)
                  << pad_right(truncate(str(c, "title"), 34), 36) << term::dim(join(about, ", ")) << '\n';
    }
    term::section_end();
    return 0;
}

int rp_personas(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json list = json::array();
    for (const auto& p : auth->client.get_authed(kRp + "/user-characters", auth->session()))
        if (!p.contains("chatId") || !p["chatId"].is_number()) list.push_back(p);
    if (ctx.json) {
        std::cout << list.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Your personas (" + std::to_string(list.size()) + ")");
    if (list.empty()) term::row("", term::dim("none; you talk as yourself"), 0);
    for (const auto& p : list) {
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(p), 5) << "  " << pad_right(term::bold(str(p, "name")), 22)
                  << term::dim(truncate(str(p, "title"), 40)) << '\n';
    }
    term::section_end();
    return 0;
}

int rp_chats(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json chats = auth->client.get_authed(kRp + "/chats", auth->session());
    if (ctx.json) {
        std::cout << chats.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Roleplay chats (" + std::to_string(chats.size()) + ")");
    if (chats.empty()) term::row("", term::dim("none; start one with gn rp new <character>"), 0);
    for (const auto& c : chats)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(c), 5) << "  " << pad_right(truncate(str(c, "name"), 30), 32)
                  << pad_right(truncate(cast_of(c), 26), 28) << term::dim(c.value("noModelSelected", false) ? "no model" : str(c, "modelName"))
                  << '\n';
    term::section_end();
    return 0;
}

int rp_show(const Context& ctx) {
    if (!need_args(ctx, 1, "rp show <chat>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json chat = chat_detail(*auth, ctx.args[0]);
    if (ctx.json) {
        std::cout << chat.dump(2) << '\n';
        return 0;
    }
    std::cout << term::bold(str(chat, "name")) << ' ' << term::dim("(" + cast_of(chat) + "; " + str(chat, "modelName") + ")") << "\n\n";
    for (const auto& m : chat.value("messages", json::array())) print_message(m, persona_of(chat));
    return 0;
}

// gn rp <chat>: carry on in a chat.
int rp_continue(const Context& ctx) {
    if (ctx.args.empty()) return rp_chats(ctx);
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json chat = chat_detail(*auth, ctx.args[0]);
    if (!term::interactive()) {
        term::error("A roleplay session needs a terminal; gn rp show " + ctx.args[0] + " prints the chat.");
        return 1;
    }
    return session(*auth, chat);
}

// gn rp new <character>[,<character>…] [--as <persona>] [-m <model>] [--name <name>]
int rp_new(const Context& ctx) {
    if (!need_args(ctx, 1, "rp new <character>[,<character>...] [--as <persona>] [-m <model>]")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;
    json characters = a.client.get_authed(kRp + "/characters", a.session());
    json ids = json::array();
    std::string names;
    for (const auto& who : split_commas(join(ctx.args, " "))) {
        auto c = find_named(characters, who, "character");
        if (!c) return 1;
        ids.push_back(num(*c, "id"));
        names += (names.empty() ? "" : ", ") + str(*c, "name");
    }
    json body = {{"characterIds", ids}};
    if (!ctx.persona.empty()) {
        auto p = find_named(a.client.get_authed(kRp + "/user-characters", a.session()), ctx.persona, "persona");
        if (!p) return 1;
        body["userCharacterId"] = num(*p, "id");
    }
    // A model that can roleplay: --model, else the first one that says it can.
    json models = a.client.get_authed("/api/v2/ai/models", a.session());
    std::optional<json> model;
    for (const auto& m : models) {
        if (!m.value("roleplayCompatible", false)) continue;
        if (ctx.model.empty() ? !model : (std::to_string(static_cast<long long>(num(m, "id"))) == ctx.model ||
                                          lower(str(m, "name")).find(lower(ctx.model)) != std::string::npos)) {
            model = m;
            if (!ctx.model.empty()) break;
        }
    }
    if (!model) {
        term::error(ctx.model.empty() ? "No model is marked as fit for roleplay." : "No roleplay model matches " + ctx.model + ".");
        return 1;
    }
    body["modelId"] = num(*model, "id");
    body["name"] = ctx.name.empty() ? names : ctx.name;
    json chat = a.client.call("POST", kRp + "/chats", a.session(), &body);
    term::success("Started " + term::bold(str(chat, "name")) + " with " + str(*model, "name") + term::dim(" (chat " + id_of(chat) + ")"));
    if (!term::interactive()) return 0;
    std::cout << '\n';
    return session(a, chat);
}

int rp_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "rp rm <chat>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int rc = 0;
    for (const auto& id : ctx.args) {
        json chat = chat_detail(*auth, id);
        if (!confirm(ctx, "Delete the roleplay chat " + str(chat, "name") + " (" + id + ") and its messages?")) {
            rc = 1;
            continue;
        }
        auth->client.call("DELETE", kRp + "/chats/" + id, auth->session());
        term::success("Deleted " + str(chat, "name") + ".");
    }
    return rc;
}

}  // namespace commands

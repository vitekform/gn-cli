// gn chat: talk to the AI chat models (AIChatAPI) from the terminal, one question at a time or
// in a REPL, with replies streamed as the model writes them.
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "commands.hpp"
#include "common.hpp"
#include "lineedit.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kAi = "/api/v2/ai";

// ── Ctrl-C during a reply stops the reply, not gn ──────────────────────────

volatile std::sig_atomic_t g_interrupted = 0;
extern "C" void on_sigint(int) { g_interrupted = 1; }

class InterruptGuard {
public:
    InterruptGuard() {
        g_interrupted = 0;
        previous_ = std::signal(SIGINT, on_sigint);
    }
    ~InterruptGuard() { std::signal(SIGINT, previous_ == SIG_ERR ? SIG_DFL : previous_); }

private:
    void (*previous_)(int) = SIG_DFL;
};

// ── the model ──────────────────────────────────────────────────────────────

fs::path state_file() { return config::dir() / "chat.json"; }

std::optional<long long> remembered_model() {
    std::ifstream in(state_file());
    if (!in) return std::nullopt;
    json s = json::parse(in, nullptr, false);
    if (!s.is_object() || !s.contains("model") || !s["model"].is_number()) return std::nullopt;
    return s["model"].get<long long>();
}

void remember_model(long long id) {
    std::error_code ec;
    fs::create_directories(config::dir(), ec);
    std::ofstream(state_file()) << json{{"model", id}}.dump();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string label(const std::string& parent, const std::string& name) {
    return parent.empty() || lower(name).rfind(lower(parent), 0) == 0 ? name : parent + " / " + name;
}

// A model from /models: "OpenAI / gpt-5".
std::string model_label(const json& m) { return label(str(m, "parent"), str(m, "name")); }

// The model a chat uses (chats carry it as modelName next to their own name).
std::string chat_model_label(const json& c) {
    return c.value("noModelSelected", false) ? "no model" : label(str(c, "parent"), str(c, "modelName"));
}

// An id, an exact name, or a piece of one that fits exactly one model.
std::optional<json> find_model(const json& models, const std::string& query) {
    std::vector<json> hits;
    for (const auto& m : models) {
        if (std::to_string(static_cast<long long>(num(m, "id"))) == query || lower(str(m, "name")) == lower(query)) return json(m);
        if (lower(model_label(m)).find(lower(query)) != std::string::npos) hits.push_back(m);
    }
    if (hits.size() == 1) return json(hits[0]);
    if (hits.empty()) {
        term::error("No model matches '" + query + "'; gn chat models lists them.");
    } else {
        term::error("'" + query + "' fits " + std::to_string(hits.size()) + " models:");
        for (const auto& m : hits)
            std::cerr << "  " << pad_left(std::to_string(static_cast<long long>(num(m, "id"))), 4) << "  " << model_label(m) << '\n';
    }
    return std::nullopt;
}

void print_models(const json& models) {
    term::section_begin("Models (" + std::to_string(models.size()) + ")");
    for (const auto& m : models) {
        std::string flags;
        if (m.value("visionCapable", false)) flags += " vision";
        if (m.value("toolingSupported", false)) flags += " tools";
        double in = num(m, "pricePerPromptToken") * 1e6, out = num(m, "pricePerCompletionToken") * 1e6;
        std::string price = in == 0 && out == 0 ? term::green("free") : money(in, 2) + " / " + money(out, 2);
        long long ctx_len = static_cast<long long>(num(m, "contextLength"));
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(m, "id"))), 4) << "  "
                  << pad_right(truncate(model_label(m), 38), 39) << pad_left(price, 16) << "  "
                  << pad_left(ctx_len > 0 ? with_thousands(ctx_len / 1000) + "k" : "", 6) << term::dim(flags) << '\n';
    }
    term::section_end();
    std::cout << term::dim("  Prices per million tokens, in / out.") << '\n';
}

// The model to start with: --model, else the last one used, else (interactively) a pick from the list.
std::optional<json> choose_model(Auth& a, const Context& ctx) {
    json models = a.client.get_authed(kAi + "/models", a.session());
    if (models.empty()) {
        term::error("No models are available to you.");
        return std::nullopt;
    }
    if (!ctx.model.empty()) return find_model(models, ctx.model);
    if (auto last = remembered_model())
        for (const auto& m : models)
            if (static_cast<long long>(num(m, "id")) == *last) return json(m);
    if (!term::interactive()) {
        missing_input("No model chosen.", "pass --model <name or id> (gn chat models lists them)");
        return std::nullopt;
    }
    print_models(models);
    for (;;) {
        std::string answer;
        if (!term::prompt("Model (id or name):", answer)) return std::nullopt;
        answer = trim(answer);
        if (answer.empty()) continue;
        if (auto m = find_model(models, answer)) return m;
    }
}

// ── a turn ─────────────────────────────────────────────────────────────────

struct Turn {
    json done;            // the final event: messageId, content, tokens, toolCalls
    bool failed = false;
    bool interrupted = false;
};

// Draws one reply as its events arrive: text to stdout; thinking and tool activity as a single
// transient line on stderr (only on a terminal, so `gn chat … > answer.md` stays clean).
class Renderer {
public:
    explicit Renderer(bool show_reasoning) : reasoning_(show_reasoning), status_(term::stderr_is_tty()) {}

    bool on_event(const std::string& data, Turn& turn) {
        if (g_interrupted) {
            turn.interrupted = true;
            return false;
        }
        json e = json::parse(data, nullptr, false);
        if (!e.is_object()) return true;
        if (e.contains("done")) {
            turn.done = e;
            return true;
        }
        if (auto it = e.find("error"); it != e.end()) {
            end_text();
            term::error(it->is_string() ? it->get<std::string>() : it->dump());
            turn.failed = true;
            return true;
        }
        if (auto it = e.find("reasoning"); it != e.end() && it->is_string()) {
            if (reasoning_) {
                clear_status();
                std::cerr << term::dim(it->get<std::string>()) << std::flush;
                reasoned_ = true;
            } else {
                show_status("thinking");
            }
            return true;
        }
        if (auto it = e.find("content"); it != e.end() && it->is_string()) {
            clear_status();
            if (reasoned_ && !wrote_) std::cerr << "\n\n";
            const std::string& text = it->get<std::string>();
            std::cout << text << std::flush;
            if (!text.empty()) last_ = text.back();
            wrote_ = wrote_ || !text.empty();
            return true;
        }
        if (auto it = e.find("toolCall"); it != e.end() && it->is_object()) {
            std::string name = str(*it, "name"), state = str(*it, "status");
            if (state == "running") {
                show_status("running " + name);
            } else {
                end_text();
                std::string summary = str(*it, "resultSummary");
                std::cerr << (state == "error" ? term::red(term::g().fail) : term::dim(term::g().ok)) << ' '
                          << term::dim(name + (summary.empty() ? "" : ": " + truncate(summary, 100))) << '\n';
            }
            return true;
        }
        if (auto it = e.find("toolProposal"); it != e.end() && it->is_object()) {
            end_text();
            term::info("The model proposes " + term::bold(str(*it, "name")) + "; act on it in the web app's chat.");
            return true;
        }
        return true;
    }

    void finish() {
        clear_status();
        end_text();
    }

private:
    void end_text() {
        clear_status();
        if (wrote_ && last_ != '\n') std::cout << '\n';
        last_ = '\n';
        std::cout << std::flush;
    }
    void show_status(const std::string& what) {
        if (!status_ || wrote_ || what == shown_) return;
        std::cerr << '\r' << term::dim(what + term::g().ellipsis) << "\x1b[K" << std::flush;
        shown_ = what;
    }
    void clear_status() {
        if (shown_.empty()) return;
        std::cerr << "\r\x1b[K" << std::flush;
        shown_.clear();
    }

    bool reasoning_;
    bool status_;
    bool wrote_ = false, reasoned_ = false;
    char last_ = '\n';
    std::string shown_;
};

// The ask_user questions waiting in a finished turn, if any: {call id, questions}.
std::optional<std::pair<long long, json>> pending_questions(const json& done) {
    for (const auto& c : done.value("toolCalls", json::array())) {
        if (str(c, "name") != "ask_user" || str(c, "decision") != "PENDING") continue;
        json result = json::parse(str(c, "result"), nullptr, false);
        if (result.is_object() && result.contains("questions"))
            return std::make_pair(static_cast<long long>(num(c, "id")), result["questions"]);
    }
    return std::nullopt;
}

// Asks the model's questions on the terminal; one entry per question as AskUserTool expects.
std::optional<json> answer_questions(const json& questions) {
    json answers = json::array();
    for (const auto& q : questions) {
        std::string text = q.is_string() ? q.get<std::string>() : str(q, "question");
        json options = q.is_object() ? q.value("options", json::array()) : json::array();
        bool multiple = q.is_object() && q.value("multiple", false);
        std::cout << '\n' << term::bold(term::cyan("? ")) << term::bold(text) << '\n';
        for (std::size_t i = 0; i < options.size(); ++i) {
            std::string desc = str(options[i], "description");
            std::cout << "  " << term::cyan(std::to_string(i + 1)) << ". " << str(options[i], "label")
                      << (desc.empty() ? "" : term::dim(" — " + desc)) << '\n';
        }
        std::string hint = options.empty() ? "your answer" : multiple ? "numbers like 1,3, or your own words" : "a number, or your own words";
        for (;;) {
            std::string reply;
            if (!term::prompt("(" + hint + "; empty skips)", reply)) return std::nullopt;
            reply = trim(reply);
            if (reply.empty()) {
                answers.push_back({{"skipped", true}});
                break;
            }
            // All numbers in range: picks. Anything else: a free-text answer.
            std::vector<std::string> picks;
            bool all_numbers = !options.empty();
            std::string token;
            std::istringstream in(reply);
            while (all_numbers && std::getline(in, token, ',')) {
                token = trim(token);
                if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) {
                    all_numbers = false;
                    break;
                }
                std::size_t n = std::stoul(token);
                if (n < 1 || n > options.size()) {
                    all_numbers = false;
                    break;
                }
                picks.push_back(str(options[n - 1], "label"));
            }
            if (all_numbers && !picks.empty()) {
                if (picks.size() > 1 && !multiple) {
                    term::warn("Pick one.");
                    continue;
                }
                answers.push_back({{"selected", picks}});
            } else {
                answers.push_back({{"custom", reply}});
            }
            break;
        }
    }
    return answers;
}

// Sends one message and streams the reply, then answers any questions the model asked (which
// resumes the turn) until it is done. Returns the last turn.
Turn converse(Auth& a, long long chat_id, const std::string& content, const Context& ctx, bool quiet) {
    Turn turn;
    InterruptGuard guard;
    {
        Renderer r(ctx.reasoning);
        // Quiet (--json): nothing drawn, only the final event (or an error) kept.
        auto handle = [&](const std::string& data) {
            if (!quiet) return r.on_event(data, turn);
            json e = json::parse(data, nullptr, false);
            if (e.is_object() && (e.contains("done") || e.contains("error"))) {
                turn.done = e;
                turn.failed = e.contains("error");
            }
            return g_interrupted == 0;
        };
        a.client.stream_events(kAi + "/chats/" + std::to_string(chat_id) + "/messages", {{"content", content}}, a.session(), handle);
        r.finish();
    }
    while (!turn.interrupted && !turn.failed) {
        auto pending = pending_questions(turn.done);
        if (!pending) break;
        if (!term::interactive()) {
            term::warn("The model asked something; answer it in the web app's chat, or continue with gn chat --chat " +
                       std::to_string(chat_id) + ".");
            break;
        }
        auto answers = answer_questions(pending->second);
        if (!answers) break;
        json body = {{"answers", *answers}};
        Turn next;
        Renderer r(ctx.reasoning);
        std::cout << '\n';
        a.client.stream_events(kAi + "/tool-calls/" + std::to_string(pending->first) + "/answer", {}, a.session(),
                               [&](const std::string& data) { return r.on_event(data, next); }, &body);
        r.finish();
        turn = next;
    }
    if (g_interrupted) turn.interrupted = true;
    if (turn.interrupted) std::cerr << term::dim("(stopped)") << '\n';
    return turn;
}

std::string usage_line(const json& done) {
    long long in = static_cast<long long>(num(done, "promptTokens")), out = static_cast<long long>(num(done, "completionTokens"));
    long long window = static_cast<long long>(num(done, "contextLength"));
    if (in == 0 && out == 0) return "";
    std::string s = with_thousands(in) + " " + term::g().arrow + " " + with_thousands(out) + " tokens";
    if (window > 0) s += ", " + std::to_string(in * 100 / window) + "% of the context";
    return s;
}

long long create_chat(Auth& a, const json& model, const std::string& first_message) {
    std::string name = first_message.substr(0, first_message.find('\n'));
    name = trim(name);
    if (term::width(name) > 48) name = truncate(name, 48);
    http::Form form{{"modelId", std::to_string(static_cast<long long>(num(model, "id")))}};
    if (!name.empty()) form.emplace_back("name", name);
    json chat = a.client.post_form(kAi + "/chats", form, a.session());
    return static_cast<long long>(num(chat, "id"));
}

std::string read_all_stdin() {
    std::string data((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) data.pop_back();
    return data;
}

std::optional<long long> parse_chat_id(const std::string& s) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::stoll(s);
}

// ── the REPL ───────────────────────────────────────────────────────────────

int repl(Auth& a, const Context& ctx, json model, std::optional<long long> chat_id) {
    using namespace term;
    auto banner = [&] {
        std::cout << bold(cyan(model_label(model))) << ' '
                  << dim(chat_id ? "(chat " + std::to_string(*chat_id) + ")" : "(new chat)") << ' ' << dim(g().dash) << ' '
                  << dim("/help for commands, /exit to leave") << '\n';
    };
    banner();
    const fs::path history = config::dir() / "chat-history";
    lineedit::history_load(history);
    static const std::vector<std::string> kSlash = {"/help", "/exit", "/quit", "/new", "/model", "/models", "/clear", "/reasoning"};
    lineedit::Completer complete = [](const std::string& before, std::size_t& start) {
        std::vector<std::string> out;
        if (before.empty() || before[0] != '/' || before.find(' ') != std::string::npos) return out;
        start = 0;
        for (const auto& c : kSlash)
            if (c.rfind(before, 0) == 0) out.push_back(c);
        return out;
    };
    Context local = ctx;
    const std::string prompt = bold(green("you")) + "> ";
    for (;;) {
        std::string line;
        if (!lineedit::read_line(prompt, line, complete)) {
            std::cout << '\n';
            break;
        }
        // A trailing backslash continues the message on the next line.
        while (!line.empty() && line.back() == '\\') {
            line.pop_back();
            std::string more;
            if (!lineedit::read_line(dim("... "), more)) break;
            line += "\n" + more;
        }
        std::string text = trim(line);
        if (text.empty()) continue;
        lineedit::history_add(line);
        lineedit::history_save(history);

        try {
            if (text[0] == '/') {
                std::string cmd = text.substr(0, text.find(' '));
                std::string arg = text.find(' ') == std::string::npos ? "" : trim(text.substr(text.find(' ')));
                if (cmd == "/exit" || cmd == "/quit" || cmd == "/q") break;
                if (cmd == "/help") {
                    std::cout << "  /new              start a new chat with the same model\n"
                              << "  /model [name|id]  switch model (no argument: list them)\n"
                              << "  /models           list the models\n"
                              << "  /reasoning        show or hide the model's reasoning\n"
                              << "  /clear            clear the screen\n"
                              << "  /exit             leave (also Ctrl-D)\n"
                              << dim("  End a line with \\ to continue on the next one. Ctrl-C stops a reply.") << '\n';
                } else if (cmd == "/new") {
                    chat_id.reset();
                    banner();
                } else if (cmd == "/models" || (cmd == "/model" && arg.empty())) {
                    print_models(a.client.get_authed(kAi + "/models", a.session()));
                } else if (cmd == "/model") {
                    auto m = find_model(a.client.get_authed(kAi + "/models", a.session()), arg);
                    if (!m) continue;
                    model = *m;
                    remember_model(static_cast<long long>(num(model, "id")));
                    if (chat_id)
                        a.client.send_form("PUT", kAi + "/chats/" + std::to_string(*chat_id),
                                           {{"modelId", std::to_string(static_cast<long long>(num(model, "id")))}}, a.session());
                    banner();
                } else if (cmd == "/reasoning") {
                    local.reasoning = !local.reasoning;
                    term::info(std::string("Reasoning is ") + (local.reasoning ? "shown." : "hidden."));
                } else if (cmd == "/clear") {
                    clear_screen();
                    banner();
                } else {
                    error("Unknown command " + cmd + "; /help lists them.");
                }
                continue;
            }
            if (!chat_id) chat_id = create_chat(a, model, text);
            std::cout << '\n';
            Turn t = converse(a, *chat_id, text, local, false);
            if (std::string u = usage_line(t.done); !u.empty() && !t.interrupted) std::cerr << dim(u) << '\n';
            std::cout << '\n';
        } catch (const api::ApiError& e) {
            error(std::string("The server said no: ") + e.what());
        } catch (const http::TransportError& e) {
            error(std::string("Could not reach the server: ") + e.what());
        }
    }
    return 0;
}

}  // namespace

// gn chat [question...]: a REPL, or one question when there is one (or something on stdin).
int chat(const Context& ctx) {
    std::string question = join(ctx.args, " ");
    std::string piped = term::stdin_is_tty() ? "" : read_all_stdin();
    if (question.empty() && piped.empty() && !term::interactive()) {
        term::error("Nothing to ask: give a question, pipe something in, or run gn chat in a terminal.");
        return 2;
    }

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;

    std::optional<long long> chat_id;
    std::optional<json> model;
    if (!ctx.chat.empty()) {
        chat_id = parse_chat_id(ctx.chat);
        if (!chat_id) {
            term::error("--chat takes a chat id; gn chat ls lists them.");
            return 2;
        }
        json existing = a.client.get_authed(kAi + "/chats/" + std::to_string(*chat_id), a.session());
        if (!ctx.model.empty()) {
            model = choose_model(a, ctx);
            if (!model) return 1;
            a.client.send_form("PUT", kAi + "/chats/" + std::to_string(*chat_id),
                               {{"modelId", std::to_string(static_cast<long long>(num(*model, "id")))}}, a.session());
        } else if (existing.value("noModelSelected", false)) {
            model = choose_model(a, ctx);
            if (!model) return 1;
            a.client.send_form("PUT", kAi + "/chats/" + std::to_string(*chat_id),
                               {{"modelId", std::to_string(static_cast<long long>(num(*model, "id")))}}, a.session());
        } else {
            model = json{{"id", existing["modelId"]}, {"name", existing.value("modelName", "")}, {"parent", existing.value("parent", "")}};
        }
    } else {
        model = choose_model(a, ctx);
        if (!model) return 1;
    }
    remember_model(static_cast<long long>(num(*model, "id")));

    if (question.empty() && piped.empty()) return repl(a, ctx, *model, chat_id);

    std::string content = question;
    if (!piped.empty()) content = question.empty() ? piped : question + "\n\n```\n" + piped + "\n```";
    if (!chat_id) chat_id = create_chat(a, *model, question.empty() ? piped : question);
    Turn t = converse(a, *chat_id, content, ctx, ctx.json);
    if (ctx.json) {
        json out = t.done;
        out["chatId"] = *chat_id;
        std::cout << out.dump(2) << '\n';
    } else if (term::stderr_is_tty() && !t.interrupted && !t.failed) {
        std::cerr << term::dim("Continue with: gn chat --chat " + std::to_string(*chat_id)) << '\n';
    }
    return t.failed || t.interrupted ? 1 : 0;
}

int chat_models(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json models = auth->client.get_authed(kAi + "/models", auth->session());
    if (ctx.json) std::cout << models.dump(2) << '\n';
    else print_models(models);
    return 0;
}

int chat_list(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json chats = auth->client.get_authed(kAi + "/chats", auth->session());
    if (ctx.json) {
        std::cout << chats.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Chats (" + std::to_string(chats.size()) + ")");
    if (chats.empty()) term::row("", term::dim("none yet; start one with gn chat"), 0);
    for (const auto& c : chats) {
        std::string model = chat_model_label(c);
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(c, "id"))), 5) << "  "
                  << pad_right(truncate(str(c, "name"), 40), 41) << term::dim(truncate(model, 30)) << '\n';
    }
    term::section_end();
    return 0;
}

int chat_show(const Context& ctx) {
    if (!need_args(ctx, 1, "chat show <id>")) return 2;
    auto id = parse_chat_id(ctx.args[0]);
    if (!id) {
        term::error("chat show takes a chat id; gn chat ls lists them.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json chat = auth->client.get_authed(kAi + "/chats/" + std::to_string(*id), auth->session());
    if (ctx.json) {
        std::cout << chat.dump(2) << '\n';
        return 0;
    }
    std::cout << term::bold(str(chat, "name")) << ' '
              << term::dim("(" + chat_model_label(chat) + ")") << "\n";
    for (const auto& m : chat.value("messages", json::array())) {
        std::string author = str(m, "author");
        std::string content = str(m, "content");
        bool user = author == "USER";
        if (author == "SYSTEM" || author == "TOOL") continue;
        for (const auto& c : m.value("toolCalls", json::array()))
            std::cout << term::dim("  " + std::string(term::g().ok) + " " + str(c, "name")) << '\n';
        if (content.empty()) continue;
        std::cout << '\n' << (user ? term::bold(term::green("you")) : term::bold(term::cyan("assistant"))) << '\n'
                  << content << (content.back() == '\n' ? "" : "\n");
    }
    return 0;
}

int chat_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "chat rm <id>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        auto id = parse_chat_id(arg);
        if (!id) {
            term::error("Not a chat id: " + arg);
            rc = 2;
            continue;
        }
        json chat = auth->client.get_authed(kAi + "/chats/" + arg, auth->session());
        if (!confirm(ctx, "Delete chat " + arg + " (" + str(chat, "name") + ") and its messages?")) {
            rc = 1;
            continue;
        }
        auth->client.call("DELETE", kAi + "/chats/" + arg, auth->session());
        term::success("Deleted chat " + arg + ".");
    }
    return rc;
}

}  // namespace commands

// gn breeze: Breeze TTS 2 on the LAN XPU server (BreezeTTSAPI): speech in four voice modes,
// your saved Breeze voices, and the model's state.
#include <fstream>
#include <iostream>
#include <iterator>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kBreeze = "/api/v2/tts/breeze";
// The server reads one request's text in one go, up to about two minutes of audio.
constexpr std::size_t kMaxText = 5000;
constexpr std::uintmax_t kMaxClip = 25ull * 1024 * 1024;

std::string fixed(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

bool has(const json& obj, const char* key) { return obj.contains(key) && !obj[key].is_null(); }

std::optional<std::string> read_clip(const std::string& path) {
    fs::path clip = from_utf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(clip, ec)) {
        term::error("Not a file: " + path);
        return std::nullopt;
    }
    if (fs::file_size(clip, ec) > kMaxClip) {
        term::error(path + " is over 25 MB; a reference clip of 5-20 seconds is plenty.");
        return std::nullopt;
    }
    std::ifstream in(clip, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void print_status(const json& s) {
    term::section_begin("Breeze TTS");
    if (!s.value("reachable", false)) {
        term::row("Server", term::red("unreachable") + term::dim("  (is the XPU box on?)"), 9);
        term::section_end();
        return;
    }
    bool loaded = s.value("loaded", false);
    std::string model = loaded ? term::green("loaded") + term::dim(" on " + str(s, "device")) : term::dim("not loaded");
    if (has(s, "lastLoadS")) model += term::dim("  (last load " + fixed(num(s, "lastLoadS"), 0) + " s)");
    term::row("Model", model, 9);
    std::string work = s.value("busy", false) ? term::yellow("speaking") : "idle";
    long long queued = static_cast<long long>(num(s, "queuedRequests"));
    if (queued > 0) work += term::dim(", " + std::to_string(queued) + " waiting");
    term::row("Now", work, 9);
    if (has(s, "unloadInS")) term::row("Unloads", "in " + fixed(num(s, "unloadInS") / 60, 1) + " min", 9);
    else if (has(s, "idleTimeoutS") && num(s, "idleTimeoutS") == 0) term::row("Unloads", term::dim("never"), 9);
    term::section_end();
}

}  // namespace

int breeze_status(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json s = auth->client.get_authed(kBreeze + "/status", auth->session());
    if (ctx.json) std::cout << s.dump(2) << '\n';
    else print_status(s);
    return s.value("reachable", false) ? 0 : 1;
}

int breeze_load(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    if (term::stderr_is_tty()) std::cerr << term::dim("loading the model (about a minute when cold)" + std::string(term::g().ellipsis)) << std::flush;
    json s = auth->client.call("POST", kBreeze + "/load", auth->session());
    if (term::stderr_is_tty()) std::cerr << "\r\x1b[K";
    if (ctx.json) std::cout << s.dump(2) << '\n';
    else print_status(s);
    return 0;
}

int breeze_unload(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json r = auth->client.call("POST", kBreeze + "/unload", auth->session());
    if (ctx.json) std::cout << r.dump(2) << '\n';
    else if (r.value("unloaded", false)) term::success("Unloaded the model; the XPU's memory is free.");
    else term::info("The model wasn't loaded.");
    return 0;
}

int breeze_voices(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json voices = auth->client.get_authed(kBreeze + "/voices", auth->session()).value("voices", json::array());
    if (ctx.json) {
        std::cout << voices.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Breeze voices (" + std::to_string(voices.size()) + ")");
    if (voices.empty()) term::row("", term::dim("none; add one with gn breeze add <name> <clip> <transcript>"), 0);
    for (const auto& v : voices) {
        std::string len = has(v, "durationS") ? fixed(num(v, "durationS"), 1) + " s" : "";
        std::string about = str(v, "description");
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(truncate(str(v, "name"), 24), 26) << pad_right(len, 8)
                  << term::dim(truncate(about.empty() ? "\"" + str(v, "refText") + "\"" : about, 48)) << '\n';
    }
    term::section_end();
    return 0;
}

// gn breeze add <name> <clip> <transcript...>: the transcript has to be exactly what the clip says.
int breeze_add(const Context& ctx) {
    if (!need_args(ctx, 3, "breeze add <name> <clip.wav|.flac|.ogg|.mp3> <exact transcript>")) return 2;
    auto clip = read_clip(ctx.args[1]);
    if (!clip) return 1;
    std::string transcript = join({ctx.args.begin() + 2, ctx.args.end()}, " ");
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    http::Form fields{{"name", ctx.args[0]}, {"refText", transcript}};
    if (!ctx.description.empty()) fields.emplace_back("description", ctx.description);
    http::FilePart part{"file", utf8(from_utf8(ctx.args[1]).filename()), clip->data(), clip->size(),
                        guess_content_type(ctx.args[1])};
    json v = auth->client.post_file(kBreeze + "/voices", fields, part, auth->session());
    if (ctx.json) std::cout << v.dump(2) << '\n';
    else term::success("Saved the voice " + term::bold(str(v, "name")) +
                       (has(v, "durationS") ? term::dim(" (" + fixed(num(v, "durationS"), 1) + " s)") : ""));
    return 0;
}

int breeze_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "breeze rm <name>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    for (const auto& name : ctx.args) {
        auth->client.call("DELETE", kBreeze + "/voices/" + name, auth->session());
        term::success("Deleted the voice " + name + ".");
    }
    return 0;
}

// gn breeze say <text...>: speech into -o (default speech.<format>). To stdout (-o -) it streams,
// so `gn breeze hi -o - | ffplay -nodisp -` starts playing before the end is generated.
int breeze_say(const Context& ctx) {
    std::string text = join(ctx.args, " ");
    if (text.empty() && !term::stdin_is_tty()) {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        text = trim(text);
    }
    if (text.empty() && !need_args(ctx, 1, "breeze say <text> [--voice <name>] [--instruction <how>] [-o speech.wav]")) return 2;
    if (text.empty()) text = join(ctx.args, " ");
    if (text.size() > kMaxText) {
        term::error("That's " + std::to_string(text.size()) + " characters; one go takes up to " + std::to_string(kMaxText) +
                    ". Split it up.");
        return 2;
    }
    std::string format = ctx.format.empty() ? "wav" : ctx.format;
    if (format != "wav" && format != "flac" && format != "pcm") {
        term::error("--format is wav, flac or pcm.");
        return 2;
    }
    if (!ctx.ref_audio.empty() && !ctx.voice.empty()) {
        term::error("Speak with a saved --voice or a --ref clip, not both.");
        return 2;
    }
    if (!ctx.ref_audio.empty() && ctx.ref_text.empty()) {
        term::error("--ref needs --ref-text: exactly what the clip says.");
        return 2;
    }

    json body = {{"text", text}, {"format", format}};
    if (!ctx.voice.empty()) body["voice"] = ctx.voice;
    if (!ctx.instruction.empty()) body["instruction"] = ctx.instruction;
    if (!ctx.ref_text.empty()) body["refText"] = ctx.ref_text;
    try {
        if (!ctx.guidance.empty()) body["cfgScale"] = std::stod(ctx.guidance);
        if (!ctx.seed.empty()) body["seed"] = std::stoll(ctx.seed);
    } catch (const std::exception&) {
        term::error("--guidance is a number like 4, --seed a whole number.");
        return 2;
    }
    std::optional<std::string> clip;
    if (!ctx.ref_audio.empty() && !(clip = read_clip(ctx.ref_audio))) return 1;

    std::string fallback = "speech." + format;
    fs::path dest = ctx.output == "-" ? fs::path() : ctx.output.empty() ? fs::path(fallback) : download_target(ctx, fallback);
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it, or -o another name.");
        return 1;
    }
    // Streaming sends audio as it's made; flac needs the whole thing first.
    bool stream = dest.empty() && format != "flac";

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    const char* mode = !ctx.voice.empty() || clip ? (ctx.instruction.empty() ? "cloning" : "directing")
                       : ctx.instruction.empty() ? "speaking" : "designing a voice";
    bool chatty = term::stderr_is_tty() && !dest.empty();
    if (chatty) std::cerr << term::dim(std::string(mode) + term::g().ellipsis) << std::flush;

    std::string path = kBreeze + (stream ? "/stream" : "/synthesize");
    http::Response res;
    fetch_into(dest, fallback, 0, "", [&](const http::Sink& sink, const http::Progress&) {
        if (clip) {
            http::Form fields;
            for (const auto& [k, v] : body.items()) fields.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
            http::FilePart part{"refAudio", utf8(from_utf8(ctx.ref_audio).filename()), clip->data(), clip->size(),
                                guess_content_type(ctx.ref_audio)};
            res = auth->client.post_download(path, fields, &part, auth->session(), sink);
        } else {
            res = auth->client.post_json_download(path, body, auth->session(), sink);
        }
    });
    if (chatty) std::cerr << "\r\x1b[K";
    if (!dest.empty()) {
        std::string seed = res.header("x-seed"), voice_mode = res.header("x-voice-mode");
        term::success("Saved " + term::bold(utf8(dest)) +
                      term::dim(" (" + (voice_mode.empty() ? "" : voice_mode + ", ") + "seed " + (seed.empty() ? "?" : seed) +
                                (format == "pcm" ? ", s16le mono " + res.header("x-sample-rate") + " Hz" : "") + ")"));
    }
    return 0;
}

}  // namespace commands

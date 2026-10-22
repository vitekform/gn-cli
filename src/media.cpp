// gn imagen (ImageGenAPI) and gn tts (TTSAPI).
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kImagen = "/api/v2/imagen";
const std::string kTts = "/api/v2/tts";
// Where finished images are kept (nextgen_frontend_ng shared/media-url.ts).
const char* kGeneratedProvider = "Cloudflare R2";
const char* kGeneratedBucket = "generatedimages";

// The models the RunPod worker serves; ImageGenerationService and the web app know the same ids.
struct ImagenModel {
    const char* id;
    const char* label;
    const char* about;
    int steps;
    double guidance;
};
constexpr ImagenModel kModels[] = {
    {"flux1-dev-fp8", "Flux 1 Dev FP8", "realistic and photographic", 10, 3.5},
    {"flux1-dev-fp8-lora", "Flux 1 Dev FP8 + LoRA", "Flux with the uncensored LoRA adapter", 10, 3.5},
    {"animagine-v4-xl", "Animagine XL 4", "anime and illustration, crisp line art", 25, 8},
    {"pony-diffusion-v6-xl", "Pony Diffusion V6 XL", "stylised and expressive; good for characters", 20, 8},
    {"illustrious-v1-xl", "Illustrious XL v1", "anime and illustration", 28, 6.5},
    {"illustrious-v2-xl", "Illustrious XL v2", "anime and illustration, refined", 28, 6.5},
};

struct SizePreset {
    const char* name;
    int width, height;
};
constexpr SizePreset kSizes[] = {
    {"square", 1024, 1024}, {"portrait", 768, 1024}, {"landscape", 1024, 768}, {"tall", 576, 1024}, {"wide", 1024, 576},
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const ImagenModel* find_imagen_model(const std::string& query) {
    const ImagenModel* hit = nullptr;
    int hits = 0;
    for (const auto& m : kModels) {
        if (lower(query) == m.id) return &m;
        if (lower(m.id).find(lower(query)) != std::string::npos || lower(m.label).find(lower(query)) != std::string::npos) {
            hit = &m;
            ++hits;
        }
    }
    return hits == 1 ? hit : nullptr;
}

const char* model_name(const std::string& id) {
    for (const auto& m : kModels)
        if (id == m.id) return m.label;
    return nullptr;
}

// "1024x768", or a preset name.
std::optional<std::pair<int, int>> parse_size(const std::string& s) {
    for (const auto& p : kSizes)
        if (lower(s) == p.name) return std::make_pair(p.width, p.height);
    auto x = lower(s).find('x');
    if (x == std::string::npos) return std::nullopt;
    try {
        int w = std::stoi(s.substr(0, x)), h = std::stoi(s.substr(x + 1));
        if (w >= 256 && h >= 256 && w <= 2048 && h <= 2048) return std::make_pair(w, h);
    } catch (const std::exception&) {}
    return std::nullopt;
}

std::string extension_of(const std::string& key) {
    auto dot = key.rfind('.');
    auto slash = key.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return ".png";
    return key.substr(dot);
}

std::string status_word(const std::string& s) {
    if (s == "COMPLETED") return term::green("done");
    if (s == "FAILED") return term::red("failed");
    if (s == "PENDING") return term::yellow("queued");
    if (s == "PROCESSING") return term::yellow("generating");
    return s;
}

// Polls a generation until it ends (or Ctrl-C), with the wait shown on stderr.
json wait_for(Auth& a, json gen) {
    std::string id = std::to_string(static_cast<long long>(num(gen, "id")));
    bool live = term::stderr_is_tty();
    auto started = std::chrono::steady_clock::now();
    CtrlC ctrl_c;
    while (!ctrl_c.pressed()) {
        std::string status = str(gen, "status");
        if (status != "PENDING" && status != "PROCESSING") break;
        if (live) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
            std::cerr << '\r' << term::dim((status == "PENDING" ? "queued" : "generating") + std::string(term::g().ellipsis) +
                                           " " + std::to_string(secs) + " s") << "\x1b[K" << std::flush;
        }
        for (int i = 0; i < 20 && !ctrl_c.pressed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!ctrl_c.pressed()) gen = a.client.get_authed(kImagen + "/" + id, a.session());
    }
    if (live) std::cerr << "\r\x1b[K" << std::flush;
    return gen;
}

// Saves a finished generation's image: -o, else imagen-<id>.<ext> in the current folder.
int save_image(Auth& a, const Context& ctx, const json& gen) {
    std::string key = str(gen, "r2Key");
    std::string id = std::to_string(static_cast<long long>(num(gen, "id")));
    if (key.empty()) {
        term::error("Generation " + id + " has no image.");
        return 1;
    }
    fs::path dest = download_target(ctx, "imagen-" + id + extension_of(key));
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    http::Form query{{"key", key}, {"download", "true"}};
    fetch_into(dest, base_name(key), 0, "", [&](const http::Sink& sink, const http::Progress& progress) {
        a.client.download(storage_object_path(kGeneratedProvider, kGeneratedBucket), query, a.session(), sink, progress);
    });
    if (!dest.empty()) term::success("Saved " + term::bold(utf8(dest)));
    return 0;
}

std::optional<long long> parse_id(const std::string& s) {
    if (s.empty() || s.size() > 18 || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return std::stoll(s);
}

// The voice to speak with: --voice (a name or id), else the only one there is.
std::optional<json> choose_voice(Auth& a, const Context& ctx) {
    json voices = a.client.get_authed(kTts + "/voices", a.session());
    if (voices.empty()) {
        term::error("You have no voices yet; add one with gn tts add <name> <sample.wav>.");
        return std::nullopt;
    }
    if (ctx.voice.empty()) {
        if (voices.size() == 1) return json(voices[0]);
        term::error("You have " + std::to_string(voices.size()) + " voices; pick one with --voice <name or id>:");
        for (const auto& v : voices)
            std::cerr << "  " << pad_left(std::to_string(static_cast<long long>(num(v, "id"))), 4) << "  " << str(v, "name") << '\n';
        return std::nullopt;
    }
    for (const auto& v : voices)
        if (std::to_string(static_cast<long long>(num(v, "id"))) == ctx.voice || lower(str(v, "name")) == lower(ctx.voice))
            return json(v);
    term::error("No voice called " + ctx.voice + "; gn tts voices lists them.");
    return std::nullopt;
}

}  // namespace

// ── imagen ──────────────────────────────────────────────────────────────────

int imagen_models(const Context& ctx) {
    if (ctx.json) {
        json out = json::array();
        for (const auto& m : kModels)
            out.push_back({{"id", m.id}, {"label", m.label}, {"steps", m.steps}, {"guidance", m.guidance}});
        std::cout << out.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Image models");
    for (const auto& m : kModels)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(m.id, 22) << pad_right(m.label, 24) << term::dim(m.about) << '\n';
    term::section_end();
    std::cout << term::dim("  Sizes: square, portrait, landscape, tall, wide, or WxH (256-2048).") << '\n';
    return 0;
}

int imagen_generate(const Context& ctx) {
    if (!need_args(ctx, 1, "imagen <prompt> [-m model] [--size square] [-o out.png]")) return 2;
    const ImagenModel* model = &kModels[0];
    if (!ctx.model.empty()) {
        model = find_imagen_model(ctx.model);
        if (!model) {
            std::vector<std::string> fits;
            for (const auto& m : kModels)
                if (lower(m.id).find(lower(ctx.model)) != std::string::npos || lower(m.label).find(lower(ctx.model)) != std::string::npos)
                    fits.push_back(m.id);
            if (fits.empty()) term::error("No image model matches '" + ctx.model + "'; gn imagen models lists them.");
            else term::error("'" + ctx.model + "' fits " + join(fits, ", ") + "; say which.");
            return 2;
        }
    }
    std::pair<int, int> size{1024, 1024};
    if (!ctx.size.empty()) {
        auto parsed = parse_size(ctx.size);
        if (!parsed) {
            term::error("--size takes square, portrait, landscape, tall, wide or WxH (256-2048).");
            return 2;
        }
        size = *parsed;
    }
    http::Form form{{"modelId", model->id},
                    {"positivePrompt", join(ctx.args, " ")},
                    {"width", std::to_string(size.first)},
                    {"height", std::to_string(size.second)},
                    {"numSteps", ctx.steps.empty() ? std::to_string(model->steps) : ctx.steps},
                    {"guidanceScale", ctx.guidance.empty() ? std::to_string(model->guidance) : ctx.guidance}};
    if (!ctx.negative.empty()) form.emplace_back("negativePrompt", ctx.negative);

    auto auth = authenticate(ctx);
    if (!auth) return 1;
    Auth& a = *auth;
    json gen = a.client.post_form(kImagen + "/generate", form, a.session());
    std::string id = std::to_string(static_cast<long long>(num(gen, "id")));
    if (ctx.no_wait) {
        if (ctx.json) std::cout << gen.dump(2) << '\n';
        else term::success("Started generation " + term::bold(id) + "; fetch it with gn imagen get " + id);
        return 0;
    }
    term::info(std::string(model->label) + ", " + std::to_string(size.first) + "x" + std::to_string(size.second) + " " +
               term::dim("(generation " + id + ")"));
    gen = wait_for(a, gen);
    std::string status = str(gen, "status");
    if (status == "PENDING" || status == "PROCESSING") {
        term::info("Still " + status_word(status) + "; fetch it later with gn imagen get " + id);
        return 1;
    }
    if (status != "COMPLETED") {
        std::string why = str(gen, "errorMessage");
        term::error("Generation " + id + " " + lower(status) + (why.empty() ? "." : ": " + why));
        return 1;
    }
    if (ctx.json) {
        std::cout << gen.dump(2) << '\n';
        return 0;
    }
    return save_image(a, ctx, gen);
}

int imagen_get(const Context& ctx) {
    if (!need_args(ctx, 1, "imagen get <id> [-o out.png]")) return 2;
    if (!parse_id(ctx.args[0])) {
        term::error("imagen get takes a generation id; gn imagen ls lists them.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json gen = wait_for(*auth, auth->client.get_authed(kImagen + "/" + ctx.args[0], auth->session()));
    if (ctx.json) {
        std::cout << gen.dump(2) << '\n';
        return 0;
    }
    if (str(gen, "status") != "COMPLETED") {
        std::string why = str(gen, "errorMessage");
        term::error("Generation " + ctx.args[0] + " is " + lower(str(gen, "status")) + (why.empty() ? "." : ": " + why));
        return 1;
    }
    return save_image(*auth, ctx, gen);
}

int imagen_list(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json gens = auth->client.get_authed(kImagen, auth->session());
    if (ctx.json) {
        std::cout << gens.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Image generations (" + std::to_string(gens.size()) + ")");
    if (gens.empty()) term::row("", term::dim("none yet; try gn imagen \"a cat in a spacesuit\""), 0);
    for (const auto& g : gens) {
        const char* label = model_name(str(g, "modelId"));
        std::string when = g.contains("createdAt") && g["createdAt"].is_number() ? format_epoch(num(g, "createdAt") / 1000) : "";
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(g, "id"))), 5) << "  "
                  << pad_right(status_word(str(g, "status")), 11)
                  << pad_right(truncate(str(g, "positivePrompt"), 40), 42) << term::dim(label ? label : str(g, "modelId"))
                  << "  " << term::dim(when) << '\n';
    }
    term::section_end();
    return 0;
}

// ── tts ─────────────────────────────────────────────────────────────────────

int tts_voices(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json voices = auth->client.get_authed(kTts + "/voices", auth->session());
    if (ctx.json) {
        std::cout << voices.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Voices (" + std::to_string(voices.size()) + ")");
    if (voices.empty()) term::row("", term::dim("none; add one with gn tts add <name> <sample.wav>"), 0);
    for (const auto& v : voices) {
        std::string when = v.contains("createdAt") && v["createdAt"].is_number() ? format_epoch(num(v, "createdAt") / 1000) : "";
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(v, "id"))), 5) << "  "
                  << pad_right(truncate(str(v, "name"), 32), 34) << term::dim(when) << '\n';
    }
    term::section_end();
    return 0;
}

int tts_add(const Context& ctx) {
    if (!need_args(ctx, 2, "tts add <name> <sample.wav|.mp3>")) return 2;
    fs::path sample = from_utf8(ctx.args[1]);
    std::error_code ec;
    if (!fs::is_regular_file(sample, ec)) {
        term::error("Not a file: " + ctx.args[1]);
        return 1;
    }
    std::string type = guess_content_type(ctx.args[1]);
    if (type != "audio/wav" && type != "audio/mpeg") {
        term::error("A voice sample has to be a .wav or .mp3 file.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    std::ifstream in(sample, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    http::FilePart part{"file", utf8(sample.filename()), data.data(), data.size(), type};
    json v = auth->client.post_file(kTts + "/voices", {{"name", ctx.args[0]}}, part, auth->session());
    term::success("Added the voice " + term::bold(str(v, "name")) + " " + term::dim("(id " + std::to_string(static_cast<long long>(num(v, "id"))) + ")"));
    return 0;
}

int tts_remove(const Context& ctx) {
    if (!need_args(ctx, 1, "tts rm <id>...")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    int rc = 0;
    for (const auto& arg : ctx.args) {
        if (!parse_id(arg)) {
            term::error("Not a voice id: " + arg + " (gn tts voices lists them)");
            rc = 2;
            continue;
        }
        auth->client.call("DELETE", kTts + "/voices/" + arg, auth->session());
        term::success("Deleted voice " + arg + ".");
    }
    return rc;
}

// gn tts say <text...>: speech as WAV into -o (default speech.wav; - for stdout). Text may come on stdin.
int tts_say(const Context& ctx) {
    std::string text = join(ctx.args, " ");
    if (text.empty() && !term::stdin_is_tty()) {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        text = trim(text);
    }
    if (text.empty() && !need_args(ctx, 1, "tts say <text> [--voice <name>] [-o speech.wav]")) return 2;
    if (text.empty()) text = join(ctx.args, " ");
    // The text travels in the query string (GET /generate), which servers cap at a few KiB.
    constexpr std::size_t kMaxText = 2000;
    if (text.size() > kMaxText) {
        term::error("That's " + std::to_string(text.size()) + " bytes of text; one go takes up to " + std::to_string(kMaxText) + ".");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto voice = choose_voice(*auth, ctx);
    if (!voice) return 1;
    fs::path dest = ctx.output == "-" ? fs::path() : ctx.output.empty() ? fs::path("speech.wav") : download_target(ctx, "speech.wav");
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it, or -o another name.");
        return 1;
    }
    if (term::stderr_is_tty() && !dest.empty())
        std::cerr << term::dim("speaking as " + str(*voice, "name") + term::g().ellipsis) << std::flush;
    http::Form query{{"text", text}, {"voiceId", std::to_string(static_cast<long long>(num(*voice, "id")))}};
    fetch_into(dest, "speech.wav", 0, "", [&](const http::Sink& sink, const http::Progress& progress) {
        auth->client.download(kTts + "/generate", query, auth->session(), sink, progress, true);
    });
    if (term::stderr_is_tty() && !dest.empty()) std::cerr << "\r\x1b[K";
    if (!dest.empty()) term::success("Saved " + term::bold(utf8(dest)));
    return 0;
}

}  // namespace commands

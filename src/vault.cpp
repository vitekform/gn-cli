// gn vault: secure storage (SecureStorageAPI), files encrypted and hidden in PNG images in a
// bucket. The server does the cryptography (scripts/encode.py, decode.py); this picks the
// files, checks they fit, uploads, and fetches.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kVault = "/api/v2/secure-storage";

fs::path vault_file() { return config::dir() / "vault.json"; }

// The vault's provider and bucket: remembered by gn vault use, as the web app remembers them.
std::optional<std::pair<std::string, std::string>> location() {
    std::ifstream in(vault_file());
    json v = in ? json::parse(in, nullptr, false) : json();
    if (!v.is_object() || str(v, "provider").empty() || str(v, "bucket").empty()) {
        term::error("Pick the vault's bucket first: gn vault use <provider> <bucket> (gn s3 ls lists them).");
        return std::nullopt;
    }
    return std::make_pair(str(v, "provider"), str(v, "bucket"));
}

// The key the vault is locked with: --keyfile, else a password (prompted, or --password-stdin).
struct Key {
    std::string password;
    std::string keyfile;  // the key file's bytes
    std::string keyfile_name;
};

std::optional<Key> read_key(const Context& ctx, bool twice) {
    Key k;
    if (!ctx.keyfile.empty()) {
        std::ifstream in(from_utf8(ctx.keyfile), std::ios::binary);
        if (!in) {
            term::error("Can't read the key file " + ctx.keyfile + ".");
            return std::nullopt;
        }
        k.keyfile.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        k.keyfile_name = utf8(from_utf8(ctx.keyfile).filename());
        if (k.keyfile.empty()) {
            term::error("The key file is empty.");
            return std::nullopt;
        }
        return k;
    }
    if (ctx.password_stdin) {
        if (!std::getline(std::cin, k.password)) k.password.clear();
        if (!k.password.empty() && k.password.back() == '\r') k.password.pop_back();
    } else {
        std::string again;
        if (!term::prompt_secret("Vault password:", k.password)) {
            missing_input("No password given.", "use --password-stdin or --keyfile");
            return std::nullopt;
        }
        if (twice && (!term::prompt_secret("Again:", again) || again != k.password)) {
            term::error("The two passwords differ.");
            return std::nullopt;
        }
    }
    if (k.password.empty()) {
        term::error("No password given.");
        return std::nullopt;
    }
    return k;
}

// POSTs a form to a secure-storage endpoint with the key: as a field, or the key file as a part.
json post_with_key(Auth& a, const std::string& path, http::Form form, const Key& key) {
    if (!key.keyfile.empty()) {
        http::FilePart part{"keyfile", key.keyfile_name, key.keyfile.data(), key.keyfile.size()};
        return a.client.post_file(kVault + path, form, part, a.session());
    }
    form.emplace_back("password", key.password);
    return a.client.post_form(kVault + path, form, a.session(), 0);
}

// What the key opens: [{fileId, filename, mimeType, size, stegoImages, hasPreview}].
json list_files(Auth& a, const std::pair<std::string, std::string>& loc, const Key& key) {
    json files = post_with_key(a, "/decrypt", {{"provider", loc.first}, {"bucket", loc.second}, {"metadataOnly", "true"}}, key);
    return files.is_array() ? files : json::array();
}

std::optional<json> find_file(const json& files, const std::string& which) {
    std::vector<json> partial;
    for (const auto& f : files) {
        if (str(f, "fileId") == which || str(f, "filename") == which) return json(f);
        if (str(f, "filename").find(which) != std::string::npos) partial.push_back(f);
    }
    if (partial.size() == 1) return json(partial[0]);
    term::error(partial.empty() ? "This key opens no file called " + which + " (gn vault ls lists them)."
                                : "'" + which + "' fits " + std::to_string(partial.size()) + " files; give the file id.");
    return std::nullopt;
}

// Width and height of a PNG, JPEG or GIF, read from its header; nullopt for anything else.
std::optional<std::pair<std::uint32_t, std::uint32_t>> image_size(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string head(32, '\0');
    in.read(head.data(), 32);
    auto u8 = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(head[i])); };
    if (head.compare(1, 3, "PNG") == 0) return std::make_pair(u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19), u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23));
    if (head.compare(0, 3, "GIF") == 0) return std::make_pair(u8(6) | u8(7) << 8, u8(8) | u8(9) << 8);
    if (u8(0) == 0xFF && u8(1) == 0xD8) {  // JPEG: walk the markers to a start-of-frame
        in.clear();
        in.seekg(2);
        for (;;) {
            int b = in.get();
            while (b != EOF && b != 0xFF) b = in.get();
            while (b == 0xFF) b = in.get();
            if (b == EOF) return std::nullopt;
            unsigned char len_bytes[2];
            if (!in.read(reinterpret_cast<char*>(len_bytes), 2)) return std::nullopt;
            std::uint32_t len = static_cast<std::uint32_t>(len_bytes[0] << 8 | len_bytes[1]);
            bool sof = b >= 0xC0 && b <= 0xCF && b != 0xC4 && b != 0xC8 && b != 0xCC;
            if (sof) {
                unsigned char f[5];
                if (!in.read(reinterpret_cast<char*>(f), 5)) return std::nullopt;
                return std::make_pair(static_cast<std::uint32_t>(f[3] << 8 | f[4]), static_cast<std::uint32_t>(f[1] << 8 | f[2]));
            }
            if (len < 2) return std::nullopt;
            in.seekg(len - 2, std::ios::cur);
        }
    }
    return std::nullopt;
}

// stego.ts coverCapacity / requiredCapacity: 6 bits per pixel, and each image repeats a header.
std::uint64_t cover_capacity(std::uint32_t w, std::uint32_t h) { return static_cast<std::uint64_t>(w) * h * 6 / 8; }
std::uint64_t required_capacity(std::uint64_t size, const std::string& name, const std::string& mime, std::size_t covers) {
    std::uint64_t per_image = 40 + std::min<std::size_t>(name.size(), 512) + 16 + 2 + name.size() + 2 + mime.size() + 16 + 2 + 2;
    return size + std::max<std::size_t>(covers, 1) * per_image;
}

std::string random_file_id() {
    std::random_device rd;
    std::string id;
    char buf[3];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned>(rd() & 0xFF));
        id += buf;
    }
    return id;
}

}  // namespace

int vault_use(const Context& ctx) {
    if (ctx.args.empty()) {
        std::ifstream in(vault_file());
        json v = in ? json::parse(in, nullptr, false) : json();
        if (v.is_object()) std::cout << str(v, "provider") << ' ' << str(v, "bucket") << '\n';
        else term::info("No vault bucket picked yet: gn vault use <provider> <bucket>");
        return 0;
    }
    if (!need_args(ctx, 2, "vault use <provider> <bucket>")) return 2;
    std::error_code ec;
    fs::create_directories(config::dir(), ec);
    std::ofstream(vault_file()) << json{{"provider", ctx.args[0]}, {"bucket", ctx.args[1]}}.dump();
    term::success("The vault is in " + ctx.args[0] + ":" + ctx.args[1] + ".");
    return 0;
}

int vault_list(const Context& ctx) {
    auto loc = location();
    if (!loc) return 1;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto key = read_key(ctx, false);
    if (!key) return 1;
    json files = list_files(*auth, *loc, *key);
    if (ctx.json) {
        std::cout << files.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Vault " + loc->first + ":" + loc->second + " (" + std::to_string(files.size()) + ")");
    if (files.empty()) term::row("", term::dim("nothing opens with this key"), 0);
    for (const auto& f : files)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(str(f, "fileId"), 18) << pad_right(truncate(str(f, "filename"), 34), 36)
                  << pad_left(human_size(static_cast<std::uint64_t>(num(f, "size"))), 10)
                  << term::dim("  " + std::to_string(f.value("stegoImages", json::array()).size()) +
                               (f.value("stegoImages", json::array()).size() == 1 ? " image  " : " images  ") + str(f, "mimeType"))
                  << '\n';
    term::section_end();
    return 0;
}

int vault_get(const Context& ctx) {
    if (!need_args(ctx, 1, "vault get <file name or id> [-o <path>]")) return 2;
    auto loc = location();
    if (!loc) return 1;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto key = read_key(ctx, false);
    if (!key) return 1;
    auto file = find_file(list_files(*auth, *loc, *key), ctx.args[0]);
    if (!file) return 1;
    std::string name = base_name(str(*file, "filename", str(*file, "fileId")));
    fs::path dest = download_target(ctx, name);
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    // decrypt-file answers with the file itself, so it is posted as a download.
    http::Form form{{"provider", loc->first}, {"bucket", loc->second}, {"stegoImages", join(string_list(*file, "stegoImages"), ",")}};
    http::FilePart keyfile{"keyfile", key->keyfile_name, key->keyfile.data(), key->keyfile.size()};
    if (key->keyfile.empty()) form.emplace_back("password", key->password);
    if (term::stderr_is_tty() && !dest.empty())
        std::cerr << term::dim("decrypting on the server" + std::string(term::g().ellipsis)) << std::flush;
    fetch_into(dest, name, static_cast<std::uint64_t>(num(*file, "size")), "", [&](const http::Sink& sink, const http::Progress&) {
        auth->client.post_download(kVault + "/decrypt-file", form, key->keyfile.empty() ? nullptr : &keyfile, auth->session(), sink);
    });
    if (term::stderr_is_tty() && !dest.empty()) std::cerr << "\r\x1b[K";
    if (!dest.empty()) term::success("Extracted " + term::bold(name) + " " + term::dim("→ " + utf8(dest)));
    return 0;
}

int vault_hide(const Context& ctx) {
    if (!need_args(ctx, 2, "vault hide <file> <cover image>...")) return 2;
    fs::path payload = from_utf8(ctx.args[0]);
    std::error_code ec;
    if (!fs::is_regular_file(payload, ec)) {
        term::error("Not a file: " + ctx.args[0]);
        return 1;
    }
    std::vector<fs::path> covers;
    std::uint64_t capacity = 0;
    bool measured = true;
    for (std::size_t i = 1; i < ctx.args.size(); ++i) {
        fs::path c = from_utf8(ctx.args[i]);
        if (!fs::is_regular_file(c, ec)) {
            term::error("Not a file: " + ctx.args[i]);
            return 1;
        }
        if (auto wh = image_size(c)) capacity += cover_capacity(wh->first, wh->second);
        else measured = false;
        covers.push_back(c);
    }
    std::string name = utf8(payload.filename());
    std::uint64_t size = fs::file_size(payload);
    std::uint64_t need = required_capacity(size, name, guess_content_type(name), covers.size());
    if (measured && need > capacity) {
        term::error(name + " needs " + human_size(need) + " of room; these covers hold " + human_size(capacity) +
                    ". Add covers or use bigger ones (a 1000x1000 image holds about 732 KiB).");
        return 1;
    }
    if (!measured) term::warn("Some covers aren't PNG, JPEG or GIF, so the room left can't be checked here.");

    auto loc = location();
    if (!loc) return 1;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto key = read_key(ctx, true);
    if (!key) return 1;
    Auth& a = *auth;
    constexpr std::size_t kPart = 8 * 1024 * 1024;
    std::string payload_id = multipart_upload(a, payload, name, kPart, "uploaded");
    http::Form form{{"provider", loc->first}, {"bucket", loc->second}, {"fileId", random_file_id()},
                    {"payloadUploadId", payload_id}, {"payloadName", name}};
    for (const auto& c : covers) {
        form.emplace_back("coverUploadIds", multipart_upload(a, c, utf8(c.filename()), kPart, "uploaded"));
        form.emplace_back("coverNames", utf8(c.filename()));
    }
    if (term::stderr_is_tty()) std::cerr << term::dim("encrypting and packing on the server" + std::string(term::g().ellipsis)) << std::flush;
    post_with_key(a, "/encode", form, *key);
    if (term::stderr_is_tty()) std::cerr << "\r\x1b[K";
    term::success("Hid " + term::bold(name) + " in " + std::to_string(covers.size()) + (covers.size() == 1 ? " image" : " images") + " in " +
                  loc->first + ":" + loc->second + "/secure_storage/.");
    return 0;
}

int vault_link(const Context& ctx) {
    if (!need_args(ctx, 1, "vault link <file name or id>")) return 2;
    auto loc = location();
    if (!loc) return 1;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto key = read_key(ctx, false);
    if (!key) return 1;
    auto file = find_file(list_files(*auth, *loc, *key), ctx.args[0]);
    if (!file) return 1;
    json body = {{"provider", loc->first}, {"bucket", loc->second}, {"fileId", str(*file, "fileId")},
                 {"stegoImages", file->value("stegoImages", json::array())}, {"filename", str(*file, "filename")}};
    if (!key->keyfile.empty()) {
        json bytes = json::array();
        for (unsigned char b : key->keyfile) bytes.push_back(static_cast<int>(b));
        body["keyfileBytes"] = bytes;
    } else {
        body["password"] = key->password;
    }
    if (!confirm(ctx, "Anyone with the link can download " + str(*file, "filename") + " (the server keeps the key with it). Create it?"))
        return 1;
    json r = auth->client.call("POST", kVault + "/create-link", auth->session(), &body);
    std::string url = str(r, "url");
    if (!term::stdout_is_tty()) std::cout << url << '\n';
    else {
        term::success("Share link for " + term::bold(str(*file, "filename")) + ":");
        std::cout << "  " << term::cyan(url) << '\n';
    }
    return 0;
}

}  // namespace commands

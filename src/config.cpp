#include "config.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace config {
namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

void write_private(const fs::path& path, const std::string& data) {
#ifdef _WIN32
    // %APPDATA% is already private to the user.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << data;
    if (!out) throw std::runtime_error("cannot write " + path.string());
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("cannot write " + path.string());
    ::fchmod(fd, 0600);
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0) {
            ::close(fd);
            throw std::runtime_error("cannot write " + path.string());
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    ::close(fd);
#endif
}

}  // namespace

fs::path dir() {
    if (const char* d = env("GN_CLI_CONFIG_DIR")) return fs::path(d);
#ifdef _WIN32
    if (const char* d = env("APPDATA")) return fs::path(d) / "gn-cli";
    if (const char* d = env("USERPROFILE")) return fs::path(d) / "AppData" / "Roaming" / "gn-cli";
#else
    if (const char* d = env("XDG_CONFIG_HOME")) return fs::path(d) / "gn-cli";
    if (const char* d = env("HOME")) return fs::path(d) / ".config" / "gn-cli";
#endif
    return fs::path(".gn-cli");
}

fs::path credentials_path() { return dir() / "credentials.json"; }

std::optional<Credentials> load() {
    std::ifstream in(credentials_path(), std::ios::binary);
    if (!in) return std::nullopt;
    try {
        json j = json::parse(in);
        Credentials c;
        c.api_url = j.value("apiUrl", "");
        c.user_id = j.value("userId", 0LL);
        c.email = j.value("email", "");
        c.token = j.value("token", "");
        c.master_token = j.value("masterToken", "");
        c.user_agent = j.value("userAgent", "");
        if (c.user_id == 0 || c.token.empty()) return std::nullopt;
        return c;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

void save(const Credentials& c) {
    fs::path d = dir();
    std::error_code ec;
    fs::create_directories(d, ec);
    if (ec) throw std::runtime_error("cannot create " + d.string() + ": " + ec.message());
#ifndef _WIN32
    ::chmod(d.c_str(), 0700);
#endif
    json j = {
        {"apiUrl", c.api_url},
        {"userId", c.user_id},
        {"email", c.email},
        {"token", c.token},
        {"masterToken", c.master_token},
        {"userAgent", c.user_agent},
    };
    // Write aside and rename, so a crash never leaves half a file behind.
    fs::path tmp = d / "credentials.json.tmp";
    write_private(tmp, j.dump(2) + "\n");
    fs::rename(tmp, credentials_path(), ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot write " + credentials_path().string());
    }
}

bool remove() {
    std::error_code ec;
    return fs::remove(credentials_path(), ec);
}

}  // namespace config

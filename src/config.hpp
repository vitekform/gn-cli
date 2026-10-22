// Where gn-cli keeps the session between runs.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace config {

struct Credentials {
    std::string api_url;
    long long user_id = 0;
    std::string email;
    std::string token;
    std::string master_token;
    // The backend binds a token to the User-Agent it was issued for, so the one
    // used at login is kept and sent on every later request.
    std::string user_agent;
};

// $GN_CLI_CONFIG_DIR, else %APPDATA%\gn-cli on Windows, else
// $XDG_CONFIG_HOME/gn-cli or ~/.config/gn-cli.
std::filesystem::path dir();
std::filesystem::path credentials_path();

std::optional<Credentials> load();
void save(const Credentials& c);  // throws std::runtime_error
bool remove();                    // true when there was a file to remove

}  // namespace config

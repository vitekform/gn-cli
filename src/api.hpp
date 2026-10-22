// The nextgen backend's auth endpoints (/api/v2/auth/…), as gn-cli uses them.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.hpp"
#include "http.hpp"

namespace api {

inline constexpr const char* kDefaultUrl = "https://ganamaga.me";
// The release system answers only on its own host.
inline constexpr const char* kDefaultRsysUrl = "https://rsys.ganamaga.me";

// The server answered, but not with what we wanted.
struct ApiError : std::runtime_error {
    long status;
    ApiError(long status_code, const std::string& msg) : std::runtime_error(msg), status(status_code) {}
};

// The session is gone and could not be renewed; the user has to log in again.
struct Unauthorized : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Mirrors AuthAPI.LoginResponse.
struct LoginResponse {
    std::string error;  // "ok" on success
    std::string message;
    std::string token;  // session token, or the verification token when error == device_verification_required
    long long user_id = 0;
    int role_access_level = 0;
    std::string master_token;

    bool ok() const { return error == "ok" && !token.empty() && user_id != 0; }
};

std::string default_user_agent();
std::string describe_login_error(const std::string& code);

class Client {
public:
    Client(std::string base_url, std::string user_agent);

    const std::string& base_url() const { return base_; }
    const std::string& user_agent() const { return ua_; }

    // Authenticate every call with a CLI token (X-CLI-Token) instead of the saved session.
    void use_cli_token(std::string token) { cli_token_ = std::move(token); }
    bool using_cli_token() const { return !cli_token_.empty(); }
    // Sent with every authenticated call besides the credentials (BakaLink's X-BakaLink-Token).
    void add_header(std::string name, std::string value) { extra_headers_.emplace_back(std::move(name), std::move(value)); }

    LoginResponse login(const std::string& email, const std::string& password,
                        const std::string& master_token = "");
    LoginResponse verify_device(const std::string& verification_token, const std::string& code);
    LoginResponse login_with_master_token(long long user_id, const std::string& master_token);

    // Call an authenticated endpoint ("GET", "POST", "PATCH" or "PUT" with a JSON body, or "DELETE").
    //
    // With a session (creds): on a 401 the session is renewed once with the master token
    // (session tokens are bound to the caller's IP, so a new network kills them) and the
    // renewed credentials are written back to disk.
    // With a CLI token (creds may be null): no renewal; a 401 throws Unauthorized and a
    // 403 (missing scope) throws ApiError carrying the server's explanation.
    nlohmann::json call(const std::string& method, const std::string& path, config::Credentials* creds,
                        const nlohmann::json* body = nullptr);

    nlohmann::json get_authed(const std::string& path, config::Credentials* creds) {
        return call("GET", path, creds);
    }

    // GET a public endpoint with no credentials at all; >= 400 throws ApiError.
    nlohmann::json get_public(const std::string& path, long timeout_s = 60);
    // GET an absolute URL with no credentials, following redirects, streaming the body into the sink.
    void download_public(const std::string& url, const http::Sink& sink, const http::Progress& progress = {});

    // Like get_authed, but a 403/404 yields nullopt instead of throwing.
    std::optional<nlohmann::json> try_get_authed(const std::string& path, config::Credentials* creds);

    // POST an application/x-www-form-urlencoded body (what @RequestParam endpoints take).
    // timeout_s = 0 waits as long as the server needs, for calls that do slow work.
    nlohmann::json post_form(const std::string& path, const http::Form& form, config::Credentials* creds,
                             long timeout_s = 60);
    // PUT (or another method) with a form body, for @RequestParam endpoints that aren't POST.
    nlohmann::json send_form(const char* method, const std::string& path, const http::Form& form, config::Credentials* creds);
    // POST multipart/form-data: the fields plus one file.
    nlohmann::json post_file(const std::string& path, const http::Form& fields, const http::FilePart& file,
                             config::Credentials* creds, const http::Progress& progress = {});
    // GET path?query, streaming the body into the sink. Same errors as call().
    void download(const std::string& path, const http::Form& query, config::Credentials* creds,
                  const http::Sink& sink, const http::Progress& progress = {}, bool patient = false);

    // POST a form (plus `file` as multipart, if given) to an endpoint that answers with a file,
    // streaming it into the sink. Same errors as call(). The response carries its headers.
    http::Response post_download(const std::string& path, const http::Form& fields, const http::FilePart* file,
                                 config::Credentials* creds, const http::Sink& sink);
    // The same with a JSON body.
    http::Response post_json_download(const std::string& path, const nlohmann::json& body, config::Credentials* creds,
                                      const http::Sink& sink);

    // POST a form (or, with json_body, JSON) to an endpoint that answers with server-sent events;
    // each event's data (the "data:" lines joined) goes to on_event, which returns false to stop.
    void stream_events(const std::string& path, const http::Form& form, config::Credentials* creds,
                       const std::function<bool(const std::string& data)>& on_event, const nlohmann::json* json_body = nullptr);

private:
    LoginResponse post_login(const std::string& path, const std::vector<std::pair<std::string, std::string>>& form);
    // Sends a request built by `send` with this client's credentials, renews the session once on
    // a 401 (see call()), and throws Unauthorized/ApiError on failure.
    http::Response authed(const std::function<http::Response(const http::Headers&)>& send, config::Credentials* creds);
    nlohmann::json parse_ok(const http::Response& res);

    std::string base_;
    std::string ua_;
    std::string cli_token_;
    http::Headers extra_headers_;
};

}  // namespace api

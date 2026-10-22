#include "api.hpp"

#include "http.hpp"

#ifndef GN_CLI_USER_AGENT_PRODUCT
#define GN_CLI_USER_AGENT_PRODUCT "gn-cli"
#endif

using nlohmann::json;

namespace api {
namespace {

constexpr const char* kAuth = "/api/v2/auth";

const char* os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

json parse_body(const http::Response& res) {
    json j = json::parse(res.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        std::string snippet = res.body.substr(0, 120);
        for (char& c : snippet)
            if (c == '\n' || c == '\r') c = ' ';
        throw ApiError(res.status, "unexpected response (HTTP " + std::to_string(res.status) + ")" +
                                       (snippet.empty() ? "" : ": " + snippet));
    }
    return j;
}

std::string str_or_empty(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

http::Headers auth_headers(const config::Credentials& c) {
    std::string id = std::to_string(c.user_id);
    return {{"X-User-Id", id}, {"X-User-Token", c.token}};
}

std::string server_error(const http::Response& res) {
    json j = json::parse(res.body, nullptr, false);
    if (j.is_object()) {
        if (auto m = str_or_empty(j, "message"); !m.empty()) return m;
        if (auto e = str_or_empty(j, "error"); !e.empty()) return e;
        return "HTTP " + std::to_string(res.status);
    }
    // Not JSON, so not the backend: a proxy, WAF or bot filter in front of it. Its page's
    // <title> (or the start of the body) is the only hint as to which one said no.
    std::string hint = res.body;
    if (auto open = hint.find("<title>"); open != std::string::npos) {
        auto close = hint.find("</title>", open);
        hint = hint.substr(open + 7, close == std::string::npos ? std::string::npos : close - open - 7);
    }
    hint = hint.substr(0, 120);
    for (char& c : hint)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    hint.erase(0, hint.find_first_not_of(' '));
    std::string msg = "HTTP " + std::to_string(res.status) + (hint.empty() ? "" : " (not from the backend: " + hint + ")");
    // Cloudflare says why in headers, and cf-ray finds the request in its firewall events log.
    std::string details;
    auto add = [&](const char* name) {
        if (auto v = res.header(name); !v.empty()) details += std::string(details.empty() ? "" : ", ") + name + ": " + v;
    };
    add("cf-mitigated");
    add("cf-ray");
    if (details.empty()) add("server");
    return details.empty() ? msg : msg + " [" + details + "]";
}

}  // namespace

std::string default_user_agent() {
    return std::string(GN_CLI_USER_AGENT_PRODUCT "/") + GN_CLI_VERSION + " (" + os_name() + ")";
}

std::string describe_login_error(const std::string& code) {
    if (code == "email_not_found") return "No account uses this email address.";
    if (code == "wrong_password") return "That password is wrong.";
    if (code == "wrong_code") return "That code is wrong. Check the email and try again.";
    if (code == "expired_code") return "That code has expired. Log in again to get a new one.";
    if (code == "invalid_token") return "This verification attempt is no longer valid. Log in again.";
    if (code == "invalid_master_token") return "The saved session has expired.";
    return "Login failed (" + code + ").";
}

Client::Client(std::string base_url, std::string user_agent) : base_(std::move(base_url)), ua_(std::move(user_agent)) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

LoginResponse Client::post_login(const std::string& path, const http::Form& form) {
    http::Response res = http::post_form(base_ + kAuth + path, form, {}, ua_);
    if (res.status >= 400) throw ApiError(res.status, server_error(res));
    json j = parse_body(res);

    LoginResponse lr;
    lr.error = str_or_empty(j, "error");
    lr.message = str_or_empty(j, "message");
    lr.token = str_or_empty(j, "token");
    lr.master_token = str_or_empty(j, "masterToken");
    if (auto it = j.find("userId"); it != j.end() && it->is_number_integer()) lr.user_id = it->get<long long>();
    if (auto it = j.find("roleAccessLevel"); it != j.end() && it->is_number_integer())
        lr.role_access_level = it->get<int>();
    return lr;
}

LoginResponse Client::login(const std::string& email, const std::string& password, const std::string& master_token) {
    http::Form form{{"email", email}, {"password", password}};
    // A master token from an earlier session lets the server skip device verification.
    if (!master_token.empty()) form.emplace_back("masterToken", master_token);
    return post_login("/login", form);
}

LoginResponse Client::verify_device(const std::string& verification_token, const std::string& code) {
    return post_login("/verify-device", {{"verificationToken", verification_token}, {"code", code}});
}

LoginResponse Client::login_with_master_token(long long user_id, const std::string& master_token) {
    return post_login("/login-with-master-token", {{"userId", std::to_string(user_id)}, {"masterToken", master_token}});
}

http::Response Client::authed(const std::function<http::Response(const http::Headers&)>& send,
                              config::Credentials* creds) {
    auto headers = [&]() -> http::Headers {
        http::Headers h;
        if (using_cli_token()) h = {{"X-CLI-Token", cli_token_}};
        else if (creds) h = auth_headers(*creds);
        h.insert(h.end(), extra_headers_.begin(), extra_headers_.end());
        return h;
    };

    http::Response res = send(headers());

    if (res.status == 401 && !using_cli_token() && creds && !creds->master_token.empty()) {
        LoginResponse lr = login_with_master_token(creds->user_id, creds->master_token);
        if (!lr.ok()) throw Unauthorized("your session has expired");
        creds->token = lr.token;
        if (!lr.master_token.empty()) creds->master_token = lr.master_token;
        config::save(*creds);
        res = send(headers());
    }
    if (res.status == 401)
        throw Unauthorized(using_cli_token() ? server_error(res) : "the server refused the saved session");
    if (res.status >= 400) throw ApiError(res.status, server_error(res));
    return res;
}

json Client::parse_ok(const http::Response& res) {
    if (res.body.empty()) return json::object();
    return parse_body(res);
}

json Client::call(const std::string& method, const std::string& path, config::Credentials* creds,
                  const json* body) {
    std::string url = base_ + path;
    return parse_ok(authed(
        [&](const http::Headers& headers) {
            if (method == "DELETE") return http::del(url, headers, ua_);
            if (method == "POST") return http::post_json(url, body ? body->dump() : "{}", headers, ua_);
            if (method == "PATCH") return http::send_json("PATCH", url, body ? body->dump() : "{}", headers, ua_);
            if (method == "PUT") return http::send_json("PUT", url, body ? body->dump() : "{}", headers, ua_);
            return http::get(url, headers, ua_);
        },
        creds));
}

json Client::post_form(const std::string& path, const http::Form& form, config::Credentials* creds, long timeout_s) {
    std::string url = base_ + path;
    return parse_ok(authed(
        [&](const http::Headers& headers) { return http::post_form(url, form, headers, ua_, timeout_s); }, creds));
}

json Client::send_form(const char* method, const std::string& path, const http::Form& form, config::Credentials* creds) {
    std::string url = base_ + path;
    return parse_ok(authed([&](const http::Headers& headers) { return http::send_form(method, url, form, headers, ua_); }, creds));
}

json Client::post_file(const std::string& path, const http::Form& fields, const http::FilePart& file,
                       config::Credentials* creds, const http::Progress& progress) {
    std::string url = base_ + path;
    return parse_ok(authed(
        [&](const http::Headers& headers) { return http::post_file(url, fields, file, headers, ua_, progress); },
        creds));
}

void Client::download(const std::string& path, const http::Form& query, config::Credentials* creds,
                      const http::Sink& sink, const http::Progress& progress, bool patient) {
    std::string url = base_ + path + (query.empty() ? "" : "?" + http::encode_query(query));
    authed([&](const http::Headers& headers) { return http::download(url, headers, ua_, sink, progress, patient); }, creds);
}

http::Response Client::post_download(const std::string& path, const http::Form& fields, const http::FilePart* file,
                                     config::Credentials* creds, const http::Sink& sink) {
    std::string url = base_ + path;
    return authed([&](const http::Headers& headers) { return http::post_download(url, fields, file, headers, ua_, sink); }, creds);
}

http::Response Client::post_json_download(const std::string& path, const json& body, config::Credentials* creds,
                                          const http::Sink& sink) {
    std::string url = base_ + path;
    std::string data = body.dump();
    return authed([&](const http::Headers& headers) { return http::post_json_download(url, data, headers, ua_, sink); }, creds);
}

void Client::stream_events(const std::string& path, const http::Form& form, config::Credentials* creds,
                           const std::function<bool(const std::string&)>& on_event, const json* json_body) {
    std::string url = base_ + path;
    std::string body = json_body ? json_body->dump() : http::encode_query(form);
    const char* type = json_body ? "application/json" : "application/x-www-form-urlencoded";
    // A renewed session repeats the request, so the parser starts over with each attempt.
    std::string buffer;
    bool stopped = false;
    auto sink = [&](const char* data, std::size_t n) {
        buffer.append(data, n);
        // Events end with a blank line; Spring writes \n, a proxy may turn it into \r\n.
        for (;;) {
            std::size_t end = buffer.find("\n\n");
            std::size_t crlf = buffer.find("\r\n\r\n");
            std::size_t sep_len = 2;
            if (crlf != std::string::npos && (end == std::string::npos || crlf < end)) {
                end = crlf;
                sep_len = 4;
            }
            if (end == std::string::npos) return true;
            std::string event = buffer.substr(0, end);
            buffer.erase(0, end + sep_len);
            std::string payload;
            std::size_t start = 0;
            while (start <= event.size()) {
                std::size_t nl = event.find('\n', start);
                std::string line = event.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) == 0) {
                    std::string value = line.substr(5);
                    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
                    payload += (payload.empty() ? "" : "\n") + value;
                }
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            if (!payload.empty() && !on_event(payload)) {
                stopped = true;
                return false;
            }
        }
    };
    try {
        authed([&](const http::Headers& headers) {
            buffer.clear();
            return http::stream("POST", url, body, type, headers, ua_, sink);
        }, creds);
    } catch (const http::TransportError&) {
        if (!stopped) throw;  // stopping on purpose aborts the transfer; that's no error
    }
}

json Client::get_public(const std::string& path, long timeout_s) {
    http::Response res = http::get(base_ + path, {}, ua_, timeout_s);
    if (res.status >= 400) throw ApiError(res.status, server_error(res));
    return parse_ok(res);
}

void Client::download_public(const std::string& url, const http::Sink& sink, const http::Progress& progress) {
    http::Response res = http::download(url, {}, ua_, sink, progress);
    if (res.status >= 400) throw ApiError(res.status, server_error(res));
}

std::optional<json> Client::try_get_authed(const std::string& path, config::Credentials* creds) {
    try {
        return get_authed(path, creds);
    } catch (const ApiError& e) {
        if (e.status == 403 || e.status == 404) return std::nullopt;
        throw;
    }
}

}  // namespace api

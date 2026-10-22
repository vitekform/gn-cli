#include "http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>

namespace http {
namespace {

bool g_debug = false;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Headers that carry credentials: debug output names them but never shows their value.
bool secret_header(const std::string& name) {
    std::string n = lower(name);
    return n == "x-cli-token" || n == "x-user-token" || n == "authorization" || n == "cookie" || n == "x-bakalink-token";
}

struct CurlDeleter {
    void operator()(CURL* c) const { curl_easy_cleanup(c); }
};
struct SlistDeleter {
    void operator()(curl_slist* l) const { curl_slist_free_all(l); }
};
struct MimeDeleter {
    void operator()(curl_mime* m) const { curl_mime_free(m); }
};

struct Body {
    std::string data;
    const char* content_type;
};

// Everything a request may carry besides method, URL and headers.
struct Options {
    const Body* body = nullptr;
    const Form* mime_fields = nullptr;  // with mime_file: a multipart/form-data body
    const FilePart* mime_file = nullptr;
    const Sink* sink = nullptr;
    const Progress* progress = nullptr;
    long timeout_s = 60;  // 0: no overall limit, only the stall check
    bool stall_check = true;      // with timeout_s = 0: give up below 1 KiB/s for a minute
    const char* accept = nullptr;  // overrides the Accept header
};

struct Transfer {
    CURL* curl;
    Response* res;
    const Sink* sink;
    bool sink_failed = false;
};

size_t write_cb(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    size_t n = size * nmemb;
    if (t->sink) {
        long status = 0;
        curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
        // Error bodies are JSON for the caller to read, not file content.
        if (status >= 200 && status < 300) {
            if (!(*t->sink)(data, n)) {
                t->sink_failed = true;
                return 0;
            }
            return n;
        }
    }
    t->res->body.append(data, n);
    return n;
}

size_t header_cb(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* res = static_cast<Response*>(userdata);
    size_t n = size * nmemb;
    std::string_view line(data, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    // A status line starts a new response (a redirect or 100 Continue came first): keep only the last.
    if (line.rfind("HTTP/", 0) == 0) {
        res->headers.clear();
        return n;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return n;
    std::string name = lower(std::string(line.substr(0, colon)));
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    std::string& slot = res->headers[name];
    if (!slot.empty()) slot += ", ";
    slot += value;
    return n;
}

void debug_request(const char* method, const std::string& url, const Headers& headers, const std::string& user_agent) {
    std::cerr << "[debug] > " << method << ' ' << url << '\n' << "[debug] >   user-agent: " << user_agent << '\n';
    for (const auto& [k, v] : headers)
        std::cerr << "[debug] >   " << lower(k) << ": " << (secret_header(k) ? "<redacted, " + std::to_string(v.size()) + " chars>" : v)
                  << '\n';
}

void debug_response(CURL* c, const Response& res, double seconds) {
    char* ip = nullptr;
    char* effective = nullptr;
    long http_version = 0;
    curl_easy_getinfo(c, CURLINFO_PRIMARY_IP, &ip);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
    curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &http_version);
    const char* ver = http_version == CURL_HTTP_VERSION_2_0 ? "2" : http_version == CURL_HTTP_VERSION_1_1 ? "1.1"
                    : http_version == CURL_HTTP_VERSION_1_0 ? "1.0" : "?";
    std::cerr << "[debug] < HTTP/" << ver << ' ' << res.status << " from " << (ip ? ip : "?") << " in "
              << static_cast<long>(seconds * 1000) << " ms" << '\n';
    if (effective) std::cerr << "[debug] <   (final url: " << effective << ")\n";
    for (const auto& [k, v] : res.headers) {
        if (k == "set-cookie") {
            std::cerr << "[debug] <   set-cookie: " << v.substr(0, v.find('=')) << "=<redacted>\n";
            continue;
        }
        std::cerr << "[debug] <   " << k << ": " << v << '\n';
    }
    if (res.status >= 400 && !res.body.empty()) {
        std::string snippet = res.body.substr(0, 2000);
        std::cerr << "[debug] <   body (" << res.body.size() << " bytes" << (res.body.size() > 2000 ? ", first 2000" : "")
                  << "):\n" << snippet << (snippet.empty() || snippet.back() == '\n' ? "" : "\n");
    }
}

int progress_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    const auto& progress = *static_cast<const Progress*>(userdata);
    if (ultotal > 0) progress(static_cast<std::uint64_t>(ulnow), static_cast<std::uint64_t>(ultotal));
    else progress(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal));
    return 0;
}

std::string encode_pairs(const Form& form) {
    std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init());
    CURL* curl = handle.get();
    std::string out;
    for (const auto& [k, v] : form) {
        if (!out.empty()) out += '&';
        char* ek = curl_easy_escape(curl, k.c_str(), static_cast<int>(k.size()));
        char* ev = curl_easy_escape(curl, v.c_str(), static_cast<int>(v.size()));
        out += ek;
        out += '=';
        out += ev;
        curl_free(ek);
        curl_free(ev);
    }
    return out;
}

Response perform(const char* method, const std::string& url, const Headers& headers, const std::string& user_agent,
                 const Options& opts) {
    std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
    if (!curl) throw TransportError("could not initialise libcurl");
    CURL* c = curl.get();

    Response res;
    Transfer transfer{c, &res, opts.sink};
    char errbuf[CURL_ERROR_SIZE] = {0};

    curl_slist* raw = nullptr;
    raw = curl_slist_append(raw, opts.accept ? (std::string("Accept: ") + opts.accept).c_str()
                                 : opts.sink ? "Accept: */*" : "Accept: application/json");
    if (opts.body) raw = curl_slist_append(raw, (std::string("Content-Type: ") + opts.body->content_type).c_str());
    for (const auto& [k, v] : headers) raw = curl_slist_append(raw, (k + ": " + v).c_str());
    std::unique_ptr<curl_slist, SlistDeleter> list(raw);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list.get());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &res);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, opts.timeout_s > 0 && opts.timeout_s < 15 ? opts.timeout_s : 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, opts.timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    // Downloads are written as they come; let the server's own encoding through untouched.
    if (!opts.sink) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (opts.timeout_s == 0 && opts.stall_check) {
        // Under 1 KiB/s for a whole minute: the connection is dead, not slow.
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    }
    if (opts.progress && *opts.progress) {
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, opts.progress);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    }
    if (opts.body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, opts.body->data.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(opts.body->data.size()));
    }
    std::unique_ptr<curl_mime, MimeDeleter> mime;
    if (opts.mime_file) {
        mime.reset(curl_mime_init(c));
        if (opts.mime_fields) {
            for (const auto& [k, v] : *opts.mime_fields) {
                curl_mimepart* part = curl_mime_addpart(mime.get());
                curl_mime_name(part, k.c_str());
                curl_mime_data(part, v.c_str(), CURL_ZERO_TERMINATED);
            }
        }
        const FilePart& f = *opts.mime_file;
        curl_mimepart* part = curl_mime_addpart(mime.get());
        curl_mime_name(part, f.field.c_str());
        curl_mime_filename(part, f.filename.c_str());
        curl_mime_type(part, f.content_type.c_str());
        curl_mime_data(part, f.data ? f.data : "", f.size);
        curl_easy_setopt(c, CURLOPT_MIMEPOST, mime.get());
    }
    if (std::string_view(method) != "GET" && std::string_view(method) != "POST")
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);

    if (g_debug) debug_request(method, url, headers, user_agent);
    auto started = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(c);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (transfer.sink_failed) throw TransportError("could not write the downloaded data");
    if (rc != CURLE_OK) {
        std::string msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (g_debug) std::cerr << "[debug] < transport error after " << static_cast<long>(seconds * 1000) << " ms: " << msg << '\n';
        throw TransportError(msg);
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    if (g_debug) debug_response(c, res, seconds);
    return res;
}

}  // namespace

void global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void global_cleanup() { curl_global_cleanup(); }

void set_debug(bool on) { g_debug = on; }
bool debug() { return g_debug; }

std::string encode_query(const Form& form) { return encode_pairs(form); }

Response get(const std::string& url, const Headers& headers, const std::string& user_agent, long timeout_s) {
    Options opts;
    opts.timeout_s = timeout_s;
    return perform("GET", url, headers, user_agent, opts);
}

Response del(const std::string& url, const Headers& headers, const std::string& user_agent) {
    return perform("DELETE", url, headers, user_agent, {});
}

Response post_form(const std::string& url, const Form& form, const Headers& headers,
                   const std::string& user_agent, long timeout_s) {
    Body body{encode_pairs(form), "application/x-www-form-urlencoded"};
    Options opts;
    opts.body = &body;
    opts.timeout_s = timeout_s;
    return perform("POST", url, headers, user_agent, opts);
}

Response post_json(const std::string& url, const std::string& json_body, const Headers& headers,
                   const std::string& user_agent) {
    Body body{json_body, "application/json"};
    Options opts;
    opts.body = &body;
    return perform("POST", url, headers, user_agent, opts);
}

Response send_json(const char* method, const std::string& url, const std::string& json_body, const Headers& headers,
                   const std::string& user_agent) {
    Body body{json_body, "application/json"};
    Options opts;
    opts.body = &body;
    return perform(method, url, headers, user_agent, opts);
}

Response send_form(const char* method, const std::string& url, const Form& form, const Headers& headers,
                   const std::string& user_agent) {
    Body body{encode_pairs(form), "application/x-www-form-urlencoded"};
    Options opts;
    opts.body = &body;
    return perform(method, url, headers, user_agent, opts);
}

Response post_file(const std::string& url, const Form& fields, const FilePart& file, const Headers& headers,
                   const std::string& user_agent, const Progress& progress) {
    Options opts;
    opts.mime_fields = &fields;
    opts.mime_file = &file;
    opts.progress = &progress;
    opts.timeout_s = 0;
    return perform("POST", url, headers, user_agent, opts);
}

Response stream(const char* method, const std::string& url, const std::string& body, const char* content_type,
                const Headers& headers, const std::string& user_agent, const Sink& sink) {
    Body b{body, content_type};
    Options opts;
    opts.body = &b;
    opts.sink = &sink;
    opts.timeout_s = 0;
    opts.stall_check = false;  // a model can think for minutes between two events
    opts.accept = "text/event-stream";
    return perform(method, url, headers, user_agent, opts);
}

Response post_download(const std::string& url, const Form& fields, const FilePart* file, const Headers& headers,
                       const std::string& user_agent, const Sink& sink) {
    Body body{encode_pairs(fields), "application/x-www-form-urlencoded"};
    Options opts;
    if (file) {
        opts.mime_fields = &fields;
        opts.mime_file = file;
    } else {
        opts.body = &body;
    }
    opts.sink = &sink;
    opts.timeout_s = 0;
    opts.stall_check = false;
    return perform("POST", url, headers, user_agent, opts);
}

Response post_json_download(const std::string& url, const std::string& json_body, const Headers& headers,
                            const std::string& user_agent, const Sink& sink) {
    Body body{json_body, "application/json"};
    Options opts;
    opts.body = &body;
    opts.sink = &sink;
    opts.timeout_s = 0;
    opts.stall_check = false;
    return perform("POST", url, headers, user_agent, opts);
}

Response download(const std::string& url, const Headers& headers, const std::string& user_agent, const Sink& sink,
                  const Progress& progress, bool patient) {
    Options opts;
    opts.stall_check = !patient;
    opts.sink = &sink;
    opts.progress = &progress;
    opts.timeout_s = 0;
    return perform("GET", url, headers, user_agent, opts);
}

}  // namespace http

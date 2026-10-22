// A small libcurl wrapper: JSON and form POSTs, GETs, file uploads and streamed downloads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;
using Form = std::vector<std::pair<std::string, std::string>>;

struct Response {
    long status = 0;
    std::string body;
    // Headers of the final response (after redirects), names lower-cased.
    std::map<std::string, std::string> headers;

    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

// Thrown when no HTTP response came back at all (DNS, TLS, timeout, …).
struct TransportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Transfer progress in bytes; total is 0 while unknown.
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
// Receives a downloaded body piece by piece; returning false aborts the transfer.
using Sink = std::function<bool(const char* data, std::size_t size)>;

// One file field of a multipart/form-data body, sent from memory.
struct FilePart {
    std::string field;
    std::string filename;
    const char* data = nullptr;
    std::size_t size = 0;
    std::string content_type = "application/octet-stream";
};

void global_init();
void global_cleanup();

// Log every request and response (minus credentials) to stderr: --debug or $GN_DEBUG.
void set_debug(bool on);
bool debug();

Response get(const std::string& url, const Headers& headers, const std::string& user_agent, long timeout_s = 60);
Response del(const std::string& url, const Headers& headers, const std::string& user_agent);
Response post_json(const std::string& url, const std::string& json_body, const Headers& headers,
                   const std::string& user_agent);
// A JSON body with another method: PATCH, PUT.
Response send_json(const char* method, const std::string& url, const std::string& json_body, const Headers& headers,
                   const std::string& user_agent);
// application/x-www-form-urlencoded body, which Spring binds like query parameters
// without putting passwords into access logs. timeout_s = 0 waits as long as it takes.
Response post_form(const std::string& url, const Form& form, const Headers& headers,
                   const std::string& user_agent, long timeout_s = 60);
// A form body with another method: PUT.
Response send_form(const char* method, const std::string& url, const Form& form, const Headers& headers,
                   const std::string& user_agent);
// multipart/form-data: the text fields, then the file. Gives up only when the upload stalls.
Response post_file(const std::string& url, const Form& fields, const FilePart& file, const Headers& headers,
                   const std::string& user_agent, const Progress& progress = {});
// GET whose body goes to the sink instead of memory when the status is 2xx;
// an error body is kept in Response::body as usual. Gives up only when the transfer stalls.
// patient: no stall check either, for responses the server computes before sending (speech).
Response download(const std::string& url, const Headers& headers, const std::string& user_agent, const Sink& sink,
                  const Progress& progress = {}, bool patient = false);

// A request whose 2xx response is read as it arrives (server-sent events): the body goes to the
// sink chunk by chunk; an error body stays in Response::body. No timeout: the server ends it.
Response stream(const char* method, const std::string& url, const std::string& body, const char* content_type,
                const Headers& headers, const std::string& user_agent, const Sink& sink);

// POST a form (with `file`, as multipart/form-data) whose 2xx response is a file, streamed into
// the sink. No timeout or stall check: the server may work a while before the first byte.
Response post_download(const std::string& url, const Form& fields, const FilePart* file, const Headers& headers,
                       const std::string& user_agent, const Sink& sink);

// The same with a JSON body, for endpoints that take one.
Response post_json_download(const std::string& url, const std::string& json_body, const Headers& headers,
                            const std::string& user_agent, const Sink& sink);

// "a=1&b=x%20y", for query strings.
std::string encode_query(const Form& form);

}  // namespace http

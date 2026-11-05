// What `gn <command> --help` adds below the command's lines: every subcommand with its
// parameters and defaults, what the output looks like, and examples that work as written.
//
// Written for whoever reads only this, which in the AI sandbox is a model: it gets one help
// page per command, cut after a few thousand characters, and nothing else to go on. So each
// block names the output's fields, says the defaults that surprise (logs search looks back one
// hour), and stays under 4,000 characters (tests/test_aitools.py checks it).

#include <string>
#include <utility>
#include <vector>

#include "commands.hpp"

namespace help {

namespace {

const char* kLogs = R"HELP(Details
  The central log: backend requests and API calls, outbound calls, database changes, nginx,
  containers, booru activity. Needs admin.panel and admin.logs. Output is JSON: pipe into jq.

  gn logs sources [since=24h]
      What exists: kinds:[{source, kind, count, problems, last}], collector health.
  gn logs search [where=...] [since=1h] [limit=20] [before=<next>]
      Records, newest first: {records:[{time, source, kind, level, request, actor, id, data}],
      next}. The default window is ONE HOUR: say since=7d for older. limit is 1-50; for more,
      pass the result's next as before=... and run it again.
  gn logs count [where=...] [group_by=...] [since=24h]
      {total, groups:[{key, count}]}, largest first, at most 100. group_by: source, kind,
      level, actor, request, minute, hour, day, week, or any data field (path, status, tags).
  gn logs trace request_id=<a record's request>
      Everything one request left, oldest first (nginx, backend, SQL, outbound calls).
  gn logs booru actor_id=<user id> [since=7d] [only=opened|downloaded]
      A user's booru searches {time, booru, tags, page, results} (the 50 newest) and per post
      how often it was shown, opened and downloaded.

  Filters for search and count: since=15m|6h|7d|2w (at most 90d), or from=/to= ISO times
  (2026-11-04T10:00:00Z); actor_id=<user id>, request_id=..., text=<anywhere in the record>,
  kinds='["http.completed"]', levels='["ERROR","WARN"]', sources='["backend"]'.

  where= is an SQL-like condition. Fields: time, source, kind, level, request, actor (the
  user id, as text: actor = '252'), id, and any field of the record's data by name (status,
  path, method, duration_ms, handler, host, tags, message, data['odd key']).
  Operators: = != < <= > >=  IN ('a','b')  LIKE/ILIKE '%x%'  CONTAINS  ~ (regex)  BETWEEN
  IS NULL, combined with AND, OR, NOT and parentheses. level >= warn compares by rank.
  Values: 'text', numbers, true/false, now(), ago(15m).
  Kinds: http.completed (method, path, status, duration_ms), api.call (handler, outcome),
  outbound.request (host, status), application.log (logger, message, exception),
  database.change (table, operation), booru.search (tags, result_count), booru.open,
  booru.download (post, bytes), proxy.request, container.log.

Examples
  gn logs search where="kind = 'http.completed' AND status >= 500" since=1d limit=10
  gn logs search where="actor = '252' AND kind = 'booru.search'" since=3d | jq -r '.records[].data.tags'
  gn logs count where="level >= error" group_by=kind since=1d
  gn logs count where="kind = 'booru.search' AND actor = '252'" group_by=tags since=30d
The same as the server describes it, with every parameter: gn tools logs_search (logs_count, ...).)HELP";

const char* kMemory = R"HELP(Details
  The AI's memories of this user, read-only (saving and editing are the chat's own tools).
  Output is text, not JSON.

  gn memory list            Every memory you may read here: "#<id>  <title>" lines,
                            global ones and this chat's.
  gn memory read <id>       One global memory (this user, every chat): gn memory read 12
  gn memory read-chat <id>  One memory of this chat only.

  The chat comes from --chat <id>, or $GN_SANDBOX_CHAT, which the AI sandbox sets. A title is
  not the content: read a memory before relying on it.)HELP";

const char* kWeb = R"HELP(Details
  Web search and page reading. Each call spends the server's web-search budget: search once,
  then fetch only what you need.

  gn web search <words...> [--limit 1-10]
      {query, count, results:[{title, url, source, published, age, snippet}]}, 5 by default.
  gn web fetch <url> [<url>...]
      The pages' main text, to read results in full.
  gn web research <question...>
      A slower answer drawn from several sources, with the sources listed.

  More options: gn tool search_web query="rust 2026 roadmap" freshness=week count=8
  (freshness: day, week, month, year; country=us...). gn tools search_web lists them.
  In the AI sandbox curl reaches only allowlisted hosts; read other pages with gn web fetch.

Example
  gn web search "firecracker vsock" --limit 3 | jq -r '.results[] | "\(.title)  \(.url)"')HELP";

const char* kTools = R"HELP(Details
  The server's read-only AI tools, run directly: the same ones a chat calls, with the same
  permission checks.

  gn tools                  The tools you may run, one line each.
  gn tools <name>           One tool: its full description and every parameter (type,
                            allowed values, what it means). The logs tools' description
                            includes the whole query language.
  gn tool <name> key=value...
                            Run one. A value is JSON when it parses (20, true, null,
                            ["a","b"], {"k":1}) and a string otherwise. Quote for the shell:
                            where="status >= 500", tags='["a","b"]'.
                            Output is the tool's result, usually JSON; --json wraps it as
                            {tool, result}.

  gn logs, gn memory and gn web are short forms of gn tool for the common ones.

Example
  gn tool logs_count where="level >= error" group_by=source since=1d)HELP";

const char* kChat = R"HELP(Details
  gn chat ls                Your chats: id, name, model.
  gn chat show <id>         One chat: its messages, with each tool call as a line naming the
                            tool. --json gives everything: messages[] with author (USER,
                            ASSISTANT), content, timestamp, toolCalls[] {name, arguments,
                            result, status}.
  gn chat models            Models with their prices and context sizes.
  gn chat <question>        Ask once (needs aiChatBasic; paid models spend credits).
                            Not available with a read-only token, as in the AI sandbox.

Example
  gn --json chat show 42 | jq -r '.messages[] | "\(.author): \(.content[0:200])"')HELP";

const char* kFiles = R"HELP(Details
  gn files ls               Your persistent files: id, name, size, downloads, public link.
                            --json: an array of {id, filename, size, downloadCount,
                            status, url, ...}.
  gn files <file>...        Upload; prints the link. gn files rm <id|link> deletes.
  gn files get <link>       Download one (-o <path>); needs no login.
  gn files limits           The largest file your level may upload.)HELP";

const char* kAccount = R"HELP(Details
  gn me (or gn info)        Your account: email, user id, role, permissions; with a CLI
                            token, its name, scopes and expiry. --json: the raw data.
  gn credits                Balance and the last 30 requests (when, model, tokens in and
                            out, cost).
                            --json: the raw data; --watch refreshes every 10 s.)HELP";

const std::vector<std::pair<std::string, const char*>> kDetails = {
    {"logs", kLogs},  {"memory", kMemory}, {"web", kWeb},      {"tools", kTools},      {"tool", kTools},
    {"chat", kChat},  {"files", kFiles},   {"me", kAccount}, {"info", kAccount}, {"credits", kAccount},
};

}  // namespace

const char* details(const std::string& command) {
    for (const auto& [name, text] : kDetails)
        if (name == command) return text;
    return nullptr;
}

}  // namespace help

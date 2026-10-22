// gn home: the smart home (SmartHomeAPI): devices and their readings, room targets, and
// commands that the backend publishes to the devices over MQTT.
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kHome = "/api/v2/smarthome";

std::string fixed(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// "12 s ago", "5 min ago", "3 h ago" from epoch milliseconds.
std::string ago(double millis) {
    if (millis <= 0) return "never";
    long long secs = static_cast<long long>(std::time(nullptr)) - static_cast<long long>(millis / 1000);
    if (secs < 0) secs = 0;
    if (secs < 90) return std::to_string(secs) + " s ago";
    if (secs < 90 * 60) return std::to_string(secs / 60) + " min ago";
    if (secs < 48 * 3600) return std::to_string(secs / 3600) + " h ago";
    return std::to_string(secs / 86400) + " d ago";
}

json nodes_of(Auth& a) { return a.client.get_authed(kHome + "/nodes", a.session()); }

// A device by its uuid or the start of it.
std::optional<json> find_node(const json& nodes, const std::string& who) {
    std::vector<json> hits;
    for (const auto& n : nodes) {
        if (str(n, "uuid") == who) return json(n);
        if (str(n, "uuid").rfind(who, 0) == 0) hits.push_back(n);
    }
    if (hits.size() == 1) return json(hits[0]);
    term::error(hits.empty() ? "No device " + who + " (gn home lists them)." : who + " is the start of " + std::to_string(hits.size()) + " devices' ids; give more of it.");
    return std::nullopt;
}

void print_nodes(const json& nodes) {
    term::section_begin("Devices (" + std::to_string(nodes.size()) + ")");
    if (nodes.empty()) term::row("", term::dim("none have reported; try gn home poll"), 0);
    for (const auto& n : nodes) {
        std::string type = str(n, "type");
        bool inactive = type == "INACTIVE";
        std::string reading = inactive ? "" : fixed(num(n, "temperature"), 1) + " °C  " + fixed(num(n, "humidity"), 0) + " %";
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(truncate(str(n, "uuid"), 14), 15) << pad_right(inactive ? term::dim(type) : type, 9)
                  << pad_right(truncate(str(n, "room", "-"), 16), 17) << pad_right(reading, 16)
                  << term::dim(pad_right(ago(num(n, "lastSeen")), 12)) << term::dim(truncate(str(n, "tasks"), 22))
                  << (n.value("funStuffActive", false) ? " " + term::magenta("fun") : "") << '\n';
    }
    term::section_end();
}

void post(Auth& a, const std::string& path, const json& body) { a.client.call("POST", kHome + path, a.session(), &body); }

}  // namespace

int home_nodes(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json nodes = nodes_of(*auth);
    if (ctx.json) std::cout << nodes.dump(2) << '\n';
    else print_nodes(nodes);
    return 0;
}

int home_rooms(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json rooms = auth->client.get_authed(kHome + "/rooms", auth->session());
    if (ctx.json) {
        std::cout << rooms.dump(2) << '\n';
        return 0;
    }
    json nodes = nodes_of(*auth);
    term::section_begin("Rooms (" + std::to_string(rooms.size()) + ")");
    for (const auto& r : rooms) {
        // The room's current temperature: the mean of its devices' readings.
        double sum = 0;
        int count = 0;
        for (const auto& n : nodes)
            if (str(n, "room") == str(r, "room") && str(n, "type") != "INACTIVE") {
                sum += num(n, "temperature");
                ++count;
            }
        double target = num(r, "optimalTemp"), band = num(r, "tempThreshold");
        std::string now = count ? fixed(sum / count, 1) + " °C" : term::dim("no reading");
        std::string state;
        if (count) {
            double t = sum / count;
            state = t < target - band ? term::blue(" cold") : t > target + band ? term::red(" warm") : term::green(" ok");
        }
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(str(r, "room"), 20) << pad_right(now, 12) << state
                  << term::dim("   target " + fixed(target, 1) + " ± " + fixed(band, 1) + " °C") << '\n';
    }
    term::section_end();
    return 0;
}

// gn home room <name> <target °C> [threshold °C]
int home_room(const Context& ctx) {
    if (!need_args(ctx, 2, "home room <room> <target °C> [threshold °C]")) return 2;
    double target = 0, band = 0.5;
    try {
        target = std::stod(ctx.args[1]);
        if (ctx.args.size() > 2) band = std::stod(ctx.args[2]);
    } catch (const std::exception&) {
        term::error("Temperatures are numbers, like 21.5.");
        return 2;
    }
    if (target < 5 || target > 35 || band <= 0 || band > 5) {
        term::error("A target between 5 and 35 °C and a threshold between 0 and 5 °C, please.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    post(*auth, "/rooms", {{"room", ctx.args[0]}, {"optimalTemp", target}, {"tempThreshold", band}});
    term::success(ctx.args[0] + ": keeping " + fixed(target, 1) + " ± " + fixed(band, 1) + " °C.");
    return 0;
}

int home_poll(const Context& ctx) {
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    post(*auth, "/nodes/poll", json::object());
    term::info("Asked every device to report" + std::string(term::g().ellipsis));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    json nodes = nodes_of(*auth);
    if (ctx.json) std::cout << nodes.dump(2) << '\n';
    else print_nodes(nodes);
    return 0;
}

// gn home config <device> room=<room> task=<tasks> state=<state>
int home_config(const Context& ctx) {
    if (!need_args(ctx, 2, "home config <device> [room=<room>] [task=<tasks>] [state=<state>]")) return 2;
    json body = json::object();
    for (std::size_t i = 1; i < ctx.args.size(); ++i) {
        const std::string& kv = ctx.args[i];
        auto eq = kv.find('=');
        std::string key = eq == std::string::npos ? "" : kv.substr(0, eq);
        if (key != "room" && key != "task" && key != "state") {
            term::error("Expected room=, task= or state=, not " + kv + ".");
            return 2;
        }
        body[key] = kv.substr(eq + 1);
    }
    if (body.contains("state")) {
        std::string s = body["state"].get<std::string>();
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s != "MASTER" && s != "SLAVE" && s != "METEO" && s != "INACTIVE") {
            term::error("state is MASTER, SLAVE, METEO or INACTIVE.");
            return 2;
        }
        body["state"] = s;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto node = find_node(nodes_of(*auth), ctx.args[0]);
    if (!node) return 1;
    body["uuid"] = str(*node, "uuid");
    // The server sets all three every time (and needs a state); keep what isn't being changed.
    if (!body.contains("room")) body["room"] = str(*node, "room");
    if (!body.contains("task")) body["task"] = str(*node, "tasks");
    if (!body.contains("state")) body["state"] = str(*node, "type");
    post(*auth, "/nodes/config", body);
    term::success("Sent the new configuration to " + str(*node, "uuid") + ".");
    return 0;
}

int home_reset(const Context& ctx) {
    if (!need_args(ctx, 1, "home reset <device>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto node = find_node(nodes_of(*auth), ctx.args[0]);
    if (!node) return 1;
    if (!confirm(ctx, "Reset " + str(*node, "uuid") + " (" + str(node.value(), "room", "no room") + ")? It loses its configuration.")) return 1;
    post(*auth, "/nodes/reset", {{"uuid", str(*node, "uuid")}});
    term::success("Reset sent to " + str(*node, "uuid") + ".");
    return 0;
}

int home_fun(const Context& ctx) {
    if (!need_args(ctx, 2, "home fun <device> on|off")) return 2;
    const std::string& state = ctx.args[1];
    if (state != "on" && state != "off") {
        term::error("fun takes on or off.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto node = find_node(nodes_of(*auth), ctx.args[0]);
    if (!node) return 1;
    post(*auth, "/nodes/fun_stuff", {{"uuid", str(*node, "uuid")}, {"active", state == "on"}});
    term::success("Switched fun mode " + state + " for " + str(*node, "uuid") + ".");
    return 0;
}

}  // namespace commands

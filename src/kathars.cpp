// gn kathars: the Kathars Minecraft network's public staff list and punishments
// (StaffController, PunishmentController), and editing the staff list (StaffAdminController).
#include <algorithm>
#include <iostream>
#include <map>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kPublic = "/api/kathars/public";
const std::string kAdmin = "/api/kathars/admin/staff";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

api::Client public_client(const Context& ctx) {
    auto creds = config::load();
    return api::Client(resolve_url(ctx, creds ? &*creds : nullptr), api::default_user_agent());
}

// key=value arguments from `from` on, checked against the keys a command takes.
std::optional<std::map<std::string, std::string>> fields(const Context& ctx, std::size_t from,
                                                         std::initializer_list<const char*> allowed) {
    std::map<std::string, std::string> out;
    for (std::size_t i = from; i < ctx.args.size(); ++i) {
        const std::string& kv = ctx.args[i];
        auto eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        bool ok = eq != std::string::npos && std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return key == a; });
        if (!ok) {
            std::string names;
            for (const char* a : allowed) names += std::string(names.empty() ? "" : ", ") + a + "=";
            term::error("Expected " + names + " not " + kv + ".");
            return std::nullopt;
        }
        out[key] = kv.substr(eq + 1);
    }
    return out;
}

std::optional<int> to_int(const std::string& s) {
    try {
        std::size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size()) return v;
    } catch (const std::exception&) {}
    return std::nullopt;
}

// A group by id or (case-insensitive) name.
std::optional<json> find_group(const json& groups, const std::string& who) {
    for (const auto& g : groups)
        if (std::to_string(static_cast<long long>(num(g, "id"))) == who || lower(str(g, "name")) == lower(who)) return json(g);
    term::error("No staff group " + who + " (gn kathars staff lists them).");
    return std::nullopt;
}

std::optional<json> find_member(const json& groups, const std::string& who) {
    for (const auto& g : groups)
        for (const auto& m : g.value("members", json::array()))
            if (std::to_string(static_cast<long long>(num(m, "id"))) == who || lower(str(m, "name")) == lower(who)) return json(m);
    term::error("No staff member " + who + " (gn kathars staff lists them).");
    return std::nullopt;
}

std::string id_of(const json& o) { return std::to_string(static_cast<long long>(num(o, "id"))); }

}  // namespace

int kathars_staff(const Context& ctx) {
    json groups = public_client(ctx).get_public(kPublic + "/staff");
    if (ctx.json) {
        std::cout << groups.dump(2) << '\n';
        return 0;
    }
    for (const auto& g : groups) {
        auto members = g.value("members", json::array());
        term::section_begin(str(g, "name") + " " + term::dim("(" + std::to_string(members.size()) + ", group " + id_of(g) + ")"));
        if (members.empty()) term::row("", term::dim("nobody"), 0);
        for (const auto& m : members) {
            std::string mc = str(m, "minecraftName");
            std::cout << term::dim(term::g().vbar) << ' ' << pad_left(id_of(m), 4) << "  " << pad_right(term::bold(str(m, "name")), 22)
                      << pad_right(str(m, "role"), 26) << term::dim(mc.empty() || mc == str(m, "name") ? "" : "MC: " + mc) << '\n';
        }
        term::section_end();
    }
    return 0;
}

int kathars_punishments(const Context& ctx) {
    json all = public_client(ctx).get_public(kPublic + "/punishments");
    std::string player = ctx.args.empty() ? "" : lower(ctx.args[0]);
    json shown = json::array();
    for (const auto& p : all)
        if (player.empty() || lower(str(p, "playerName")).find(player) != std::string::npos) shown.push_back(p);
    // What is in force now comes first; otherwise the server's order (newest first) stands.
    std::stable_sort(shown.begin(), shown.end(),
                     [](const json& a, const json& b) { return a.value("active", false) && !b.value("active", false); });
    if (ctx.json) {
        std::cout << shown.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Punishments (" + std::to_string(shown.size()) + (player.empty() ? "" : " for " + ctx.args[0]) + ")");
    if (shown.empty()) term::row("", term::dim("none"), 0);
    for (const auto& p : shown) {
        std::string state = p.value("active", false) ? term::red("active") : p.value("revoked", false) ? term::dim("lifted") : term::dim("over");
        std::string until = p.contains("expiresAt") && !p["expiresAt"].is_null() ? "until " + format_time(p["expiresAt"]) : "permanent";
        std::cout << term::dim(term::g().vbar) << ' ' << pad_right(term::bold(str(p, "type")), 8) << pad_right(str(p, "playerName"), 18)
                  << pad_right(state, 8) << term::dim(format_time(p["createdAt"]) + "  " + until) << '\n'
                  << term::dim(term::g().vbar) << "   " << str(p, "reason", "(no reason given)")
                  << term::dim("  by " + str(p, "staffName", "?") + (str(p, "server").empty() ? "" : " on " + str(p, "server"))) << '\n';
    }
    term::section_end();
    return 0;
}

// ── editing the staff list (katharsAdmin) ──────────────────────────────────

int kathars_add_member(const Context& ctx) {
    if (!need_args(ctx, 1, "kathars add-member name=<name> group=<group> [role=<role>] [mc=<minecraft name>] [order=<n>]")) return 2;
    auto f = fields(ctx, 0, {"name", "group", "role", "mc", "order"});
    if (!f) return 2;
    if (!f->count("name") || !f->count("group")) {
        term::error("A member needs name= and group=.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json groups = auth->client.get_authed(kAdmin, auth->session());
    auto g = find_group(groups, (*f)["group"]);
    if (!g) return 1;
    json body = {{"name", (*f)["name"]}, {"role", f->count("role") ? (*f)["role"] : ""}, {"minecraftName", f->count("mc") ? (*f)["mc"] : (*f)["name"]},
                 {"groupId", num(*g, "id")}};
    if (f->count("order")) {
        auto n = to_int((*f)["order"]);
        if (!n) {
            term::error("order= takes a number.");
            return 2;
        }
        body["sortOrder"] = *n;
    }
    json m = auth->client.call("POST", kAdmin + "/members", auth->session(), &body);
    term::success("Added " + term::bold(str(m, "name")) + " to " + str(*g, "name") + term::dim(" (member " + id_of(m) + ")"));
    return 0;
}

int kathars_edit_member(const Context& ctx) {
    if (!need_args(ctx, 2, "kathars edit-member <member> [name=…] [group=…] [role=…] [mc=…] [order=…]")) return 2;
    auto f = fields(ctx, 1, {"name", "group", "role", "mc", "order"});
    if (!f) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json groups = auth->client.get_authed(kAdmin, auth->session());
    auto m = find_member(groups, ctx.args[0]);
    if (!m) return 1;
    // The update replaces the whole member; start from what it is now.
    json body = {{"name", str(*m, "name")}, {"role", str(*m, "role")}, {"minecraftName", str(*m, "minecraftName")},
                 {"groupId", num(*m, "groupId")}, {"sortOrder", num(*m, "sortOrder")}};
    if (f->count("name")) body["name"] = (*f)["name"];
    if (f->count("role")) body["role"] = (*f)["role"];
    if (f->count("mc")) body["minecraftName"] = (*f)["mc"];
    if (f->count("group")) {
        auto g = find_group(groups, (*f)["group"]);
        if (!g) return 1;
        body["groupId"] = num(*g, "id");
    }
    if (f->count("order")) {
        auto n = to_int((*f)["order"]);
        if (!n) {
            term::error("order= takes a number.");
            return 2;
        }
        body["sortOrder"] = *n;
    }
    auth->client.call("PUT", kAdmin + "/members/" + id_of(*m), auth->session(), &body);
    term::success("Updated " + term::bold(body["name"].get<std::string>()) + ".");
    return 0;
}

int kathars_remove_member(const Context& ctx) {
    if (!need_args(ctx, 1, "kathars rm-member <member>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto m = find_member(auth->client.get_authed(kAdmin, auth->session()), ctx.args[0]);
    if (!m) return 1;
    if (!confirm(ctx, "Remove " + str(*m, "name") + " from the staff list?")) return 1;
    auth->client.call("DELETE", kAdmin + "/members/" + id_of(*m), auth->session());
    term::success("Removed " + str(*m, "name") + ".");
    return 0;
}

int kathars_add_group(const Context& ctx) {
    if (!need_args(ctx, 1, "kathars add-group name=<name> [color=#rrggbb] [order=<n>]")) return 2;
    auto f = fields(ctx, 0, {"name", "color", "order"});
    if (!f) return 2;
    if (!f->count("name")) {
        term::error("A group needs name=.");
        return 2;
    }
    json body = {{"name", (*f)["name"]}};
    if (f->count("color")) body["color"] = (*f)["color"];
    if (f->count("order")) {
        auto n = to_int((*f)["order"]);
        if (!n) {
            term::error("order= takes a number.");
            return 2;
        }
        body["sortOrder"] = *n;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json g = auth->client.call("POST", kAdmin + "/groups", auth->session(), &body);
    term::success("Added the group " + term::bold(str(g, "name")) + term::dim(" (group " + id_of(g) + ")"));
    return 0;
}

int kathars_edit_group(const Context& ctx) {
    if (!need_args(ctx, 2, "kathars edit-group <group> [name=…] [color=…] [order=…]")) return 2;
    auto f = fields(ctx, 1, {"name", "color", "order"});
    if (!f) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto g = find_group(auth->client.get_authed(kAdmin, auth->session()), ctx.args[0]);
    if (!g) return 1;
    json body = {{"name", str(*g, "name")}, {"color", str(*g, "color")}, {"sortOrder", num(*g, "sortOrder")}};
    if (f->count("name")) body["name"] = (*f)["name"];
    if (f->count("color")) body["color"] = (*f)["color"];
    if (f->count("order")) {
        auto n = to_int((*f)["order"]);
        if (!n) {
            term::error("order= takes a number.");
            return 2;
        }
        body["sortOrder"] = *n;
    }
    auth->client.call("PUT", kAdmin + "/groups/" + id_of(*g), auth->session(), &body);
    term::success("Updated the group " + term::bold(body["name"].get<std::string>()) + ".");
    return 0;
}

int kathars_remove_group(const Context& ctx) {
    if (!need_args(ctx, 1, "kathars rm-group <group>")) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    auto g = find_group(auth->client.get_authed(kAdmin, auth->session()), ctx.args[0]);
    if (!g) return 1;
    // The server refuses while the group has members (StaffService.deleteGroup).
    if (std::size_t members = g->value("members", json::array()).size(); members > 0) {
        term::error(str(*g, "name") + " still has " + std::to_string(members) + (members == 1 ? " member" : " members") +
                    "; move them (edit-member <member> group=…) or remove them first.");
        return 1;
    }
    if (!confirm(ctx, "Delete the empty group " + str(*g, "name") + "?")) return 1;
    auth->client.call("DELETE", kAdmin + "/groups/" + id_of(*g), auth->session());
    term::success("Deleted the group " + str(*g, "name") + ".");
    return 0;
}

}  // namespace commands

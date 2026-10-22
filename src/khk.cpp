// gn khk: membership applications to KHK Pardubice (khk AdminController), for khkAdmin accounts.
#include <iostream>

#include "commands.hpp"
#include "common.hpp"
#include "term.hpp"

namespace commands {
namespace {

const std::string kKhk = "/api/khk/admin";

// Some of these endpoints answer in plain text ("OK", "PENDING"), not JSON.
std::string get_text(Auth& a, const std::string& path, const http::Form& query) {
    std::string body;
    a.client.download(path, query, a.session(), [&](const char* d, std::size_t n) {
        body.append(d, n);
        return true;
    });
    return body;
}

std::optional<std::string> application_id(const Context& ctx) {
    const std::string& s = ctx.args[0];
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        term::error("An application is a number; gn khk ls lists them.");
        return std::nullopt;
    }
    return s;
}

std::string status_word(const std::string& s) {
    if (s == "APPROVED") return term::green("approved");
    if (s == "REJECTED") return term::red("rejected");
    if (s == "PENDING") return term::yellow("pending");
    return s;
}

// Labels for MembershipRequest.toMap(), in the order the form asks them.
const std::pair<const char*, const char*> kFields[] = {
    {"companyName", "Company"},
    {"ic", "IČ"},
    {"dic", "DIČ"},
    {"legalForm", "Legal form"},
    {"establishmentDate", "Established"},
    {"fileNumber", "File number"},
    {"dataBoxId", "Data box"},
    {"streetAndNumber", "Address"},
    {"city", "City"},
    {"postalCode", "Postal code"},
    {"okres", "District"},
    {"region", "Region"},
    {"phone", "Phone"},
    {"email", "Email"},
    {"website", "Website"},
    {"statutoryRepresentative", "Statutory rep."},
    {"statutoryRepresentativeRole", "  role"},
    {"statutoryRepresentativeEmail", "  email"},
    {"statutoryRepresentativePhone", "  phone"},
    {"communicationRepresentativeName", "Contact person"},
    {"communicationRepresentativeRole", "  role"},
    {"communicationRepresentativeEmail", "  email"},
    {"communicationRepresentativePhone", "  phone"},
    {"employeeCount", "Employees"},
    {"netTurnoverCzk", "Net turnover (CZK)"},
    {"fieldOfActivity", "Field of activity"},
    {"czNace", "CZ-NACE"},
};

}  // namespace

int khk_list(const Context& ctx) {
    std::string status = ctx.args.empty() ? "ALL" : ctx.args[0];
    for (char& c : status) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (status != "ALL" && status != "PENDING" && status != "APPROVED" && status != "REJECTED") {
        term::error("khk ls takes pending, approved, rejected or all.");
        return 2;
    }
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json apps = auth->client.get_authed(kKhk + "/list?" + http::encode_query({{"status", status}}), auth->session());
    if (ctx.json) {
        std::cout << apps.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Applications (" + std::to_string(apps.size()) + (status == "ALL" ? "" : ", " + term::dim(status)) + ")");
    if (apps.empty()) term::row("", term::dim("none"), 0);
    for (const auto& a : apps)
        std::cout << term::dim(term::g().vbar) << ' ' << pad_left(std::to_string(static_cast<long long>(num(a, "id"))), 5) << "  "
                  << pad_right(status_word(str(a, "status")), 10) << pad_right(truncate(str(a, "companyName"), 34), 36)
                  << pad_right(truncate(str(a, "email"), 28), 30) << term::dim(str(a, "submissionDate")) << '\n';
    term::section_end();
    return 0;
}

int khk_show(const Context& ctx) {
    if (!need_args(ctx, 1, "khk show <id>")) return 2;
    auto id = application_id(ctx);
    if (!id) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json data = json::parse(get_text(*auth, kKhk + "/data", {{"id", *id}}), nullptr, false);
    std::string status = trim(get_text(*auth, kKhk + "/status", {{"id", *id}}));
    if (!data.is_object()) {
        term::error("No application " + *id + ".");
        return 1;
    }
    if (ctx.json) {
        data["status"] = status;
        std::cout << data.dump(2) << '\n';
        return 0;
    }
    term::section_begin("Application " + *id + " " + status_word(status));
    for (const auto& [key, label] : kFields) {
        auto it = data.find(key);
        if (it == data.end() || it->is_null()) continue;
        std::string v = it->is_string() ? it->get<std::string>() : it->dump();
        if (!trim(v).empty()) term::row(label, v, 20);
    }
    std::string lat = str(data, "lat"), lon = str(data, "lon");
    if (!lat.empty() && !lon.empty())
        term::row("Map", term::cyan("https://mapy.cz/zakladni?q=" + lat + "%2C" + lon), 20);
    term::section_end();
    return 0;
}

int khk_pdf(const Context& ctx) {
    if (!need_args(ctx, 1, "khk pdf <id> [-o <path>]")) return 2;
    auto id = application_id(ctx);
    if (!id) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    fs::path dest = download_target(ctx, "application-" + *id + ".pdf");
    std::error_code ec;
    if (!dest.empty() && fs::exists(dest, ec) && !ctx.force) {
        term::error(utf8(dest) + " already exists; pass --force to overwrite it.");
        return 1;
    }
    fetch_into(dest, "application-" + *id + ".pdf", 0, "", [&](const http::Sink& sink, const http::Progress& progress) {
        auth->client.download(kKhk + "/pdf", {{"id", *id}}, auth->session(), sink, progress, true);
    });
    if (!dest.empty()) term::success("Saved " + term::bold(utf8(dest)));
    return 0;
}

// Approving publishes the member on the KHK website and emails the applicant; neither can be
// taken back. Run again on an approved application, it only republishes (no second email).
int khk_accept(const Context& ctx) {
    if (!need_args(ctx, 1, "khk accept <id>")) return 2;
    auto id = application_id(ctx);
    if (!id) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    json data = json::parse(get_text(*auth, kKhk + "/data", {{"id", *id}}), nullptr, false);
    std::string status = trim(get_text(*auth, kKhk + "/status", {{"id", *id}}));
    if (!data.is_object()) {
        term::error("No application " + *id + ".");
        return 1;
    }
    if (status == "REJECTED") {
        term::error("Application " + *id + " was rejected; set it back to pending first (gn khk pending " + *id + ").");
        return 1;
    }
    std::string company = str(data, "companyName");
    std::string question = status == "APPROVED"
                               ? "Republish " + company + " on the KHK website? (no new email)"
                               : "Approve " + company + "? This publishes them on the KHK website and emails " + str(data, "email") + ".";
    if (!confirm(ctx, question)) return 1;
    std::string ok = trim(get_text(*auth, kKhk + "/accept", {{"id", *id}}));
    if (ok != "true") {
        term::error("The server didn't approve application " + *id + ".");
        return 1;
    }
    // Company names often end in a dot already (s.r.o., a.s.).
    term::success((status == "APPROVED" ? "Republished " : "Approved ") + term::bold(company) +
                  (!company.empty() && company.back() == '.' ? "" : "."));
    return 0;
}

namespace {
int set_status(const Context& ctx, const char* status, const char* verb) {
    if (!need_args(ctx, 1, std::string("khk ") + verb + " <id>")) return 2;
    auto id = application_id(ctx);
    if (!id) return 2;
    auto auth = authenticate(ctx);
    if (!auth) return 1;
    std::string now = trim(get_text(*auth, kKhk + "/status", {{"id", *id}}));
    if (now == status) {
        term::info("Application " + *id + " is already " + status_word(status) + ".");
        return 0;
    }
    // Only the status changes: an approved member stays on the KHK website until removed there.
    std::string question = std::string("Set application ") + *id + " to " + status_word(status) + "?" +
                           (now == "APPROVED" ? " It stays published on the KHK website until you remove it there." : "");
    if ((std::string(status) == "REJECTED" || now == "APPROVED") && !confirm(ctx, question)) return 1;
    std::string r = trim(get_text(*auth, kKhk + "/set_status", {{"id", *id}, {"status", status}}));
    if (r != "OK") {
        term::error("No application " + *id + ".");
        return 1;
    }
    term::success("Application " + *id + " is now " + status_word(status) + ".");
    return 0;
}
}  // namespace

int khk_reject(const Context& ctx) { return set_status(ctx, "REJECTED", "reject"); }
int khk_pending(const Context& ctx) { return set_status(ctx, "PENDING", "pending"); }

}  // namespace commands

// The Terms of Service and the licence, accepted once: `gn-cli terms`, and the gate every other
// command goes through. The acceptance is a small file next to the credentials, holding a hash
// of the texts it was given for, so changing either one asks again.
#include "commands.hpp"

#include "config.hpp"
#include "notices.hpp"
#include "sha256.hpp"
#include "term.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace commands {
namespace {

std::string terms_text() { return reinterpret_cast<const char*>(kTerms); }
std::string license_text() { return reinterpret_cast<const char*>(kLicense); }

// Short id of the two texts. Any edit to either, a typo fix too, makes everyone accept again.
std::string current_version() {
    return sha256::of(terms_text() + '\0' + license_text()).substr(0, 16);
}

fs::path acceptance_path() { return config::dir() / "terms.json"; }

std::string now_utc() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

struct Acceptance {
    std::string version;
    std::string accepted_at;
    std::string cli_version;
};

std::optional<Acceptance> load_acceptance() {
    std::ifstream in(acceptance_path(), std::ios::binary);
    if (!in) return std::nullopt;
    try {
        json j = json::parse(in);
        Acceptance a;
        a.version = j.value("version", "");
        a.accepted_at = j.value("acceptedAt", "");
        a.cli_version = j.value("cliVersion", "");
        if (a.version.empty()) return std::nullopt;
        return a;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

// False (with a warning) when it can't be saved: this run is still accepted, the next one asks again.
bool save_acceptance() {
    json j = {{"version", current_version()}, {"acceptedAt", now_utc()}, {"cliVersion", GN_CLI_VERSION}};
    std::error_code ec;
    fs::create_directories(config::dir(), ec);
    std::ofstream out(acceptance_path(), std::ios::binary | std::ios::trunc);
    out << j.dump(2) << '\n';
    if (ec || !out) {
        term::warn("Could not remember the acceptance in " + acceptance_path().string() + "; you will be asked again.");
        return false;
    }
    return true;
}

void print_terms() {
    std::string text = terms_text();
    std::cout << text;
    if (text.empty() || text.back() != '\n') std::cout << '\n';
}

void print_summary() {
    std::cerr << '\n'
              << term::bold("Before you use gn-cli") << "\n"
              << "  gn-cli is used under the " << term::bold("Terms of Service") << " and the " << term::bold("licence")
              << " that come with it.\n"
              << "  Read them with " << term::bold("gn-cli terms") << " and " << term::bold("gn-cli license")
              << " (or type " << term::bold("v") << " below).\n"
              << "  Terms version " << term::dim(current_version()) << ".\n\n";
}

// The prompt: y accepts, v shows both texts and asks again, anything else (and EOF) declines.
bool ask_to_accept() {
    for (;;) {
        std::string answer;
        if (!term::prompt("Do you accept the Terms of Service and the licence? [y/N/v]", answer)) return false;
        if (answer == "v" || answer == "V" || answer == "view") {
            print_terms();
            std::cout << "\n" << term::dim("---") << "\n\n" << license_text();
            std::cout << '\n';
            continue;
        }
        return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
    }
}

bool already_accepted() {
    auto a = load_acceptance();
    return a && a->version == current_version();
}

}  // namespace

bool require_terms(const Context& ctx) {
    if (already_accepted()) return true;

    if (ctx.accept_terms) {
        save_acceptance();
        return true;
    }
    if (!term::interactive()) {
        term::error("You have to accept the Terms of Service and the licence before using gn-cli.");
        std::cerr << "  Read them with " << term::bold("gn-cli terms") << " and " << term::bold("gn-cli license")
                  << ", then pass " << term::bold("--accept-terms") << " or set " << term::bold("GN_ACCEPT_TERMS=1")
                  << " (can't ask: " << term::noninteractive_reason() << ").\n";
        return false;
    }
    print_summary();
    if (!ask_to_accept()) {
        term::error("Not accepted, so gn-cli will not run.");
        return false;
    }
    save_acceptance();
    term::success("Accepted. " + term::dim("gn-cli terms status shows when; gn-cli terms revoke undoes it."));
    return true;
}

int terms(const Context&) {
    print_terms();
    return 0;
}

int terms_accept(const Context& ctx) {
    if (already_accepted()) {
        term::info("Already accepted, nothing to do.");
        return 0;
    }
    return require_terms(ctx) ? 0 : 1;
}

int terms_revoke(const Context&) {
    std::error_code ec;
    if (!fs::remove(acceptance_path(), ec)) {
        term::info("There was no saved acceptance.");
        return 0;
    }
    term::success("Forgotten. gn-cli asks again on the next command.");
    return 0;
}

int terms_status(const Context& ctx) {
    auto a = load_acceptance();
    const bool current = a && a->version == current_version();
    if (ctx.json) {
        json j = {{"accepted", current}, {"termsVersion", current_version()}};
        if (a) {
            j["acceptedVersion"] = a->version;
            j["acceptedAt"] = a->accepted_at;
            j["acceptedWithCli"] = a->cli_version;
        }
        std::cout << j.dump(2) << '\n';
        return current ? 0 : 1;
    }
    term::section_begin("Terms of Service and licence");
    term::row("Current version", current_version());
    if (!a) {
        term::row("Accepted", "no");
    } else if (current) {
        term::row("Accepted", "yes, " + a->accepted_at + " (gn-cli " + a->cli_version + ")");
    } else {
        term::row("Accepted", "no: you accepted " + a->version + " on " + a->accepted_at + ", and the texts changed");
    }
    term::section_end();
    return current ? 0 : 1;
}

}  // namespace commands

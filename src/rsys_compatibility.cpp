#include "rsys_compatibility.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace rsys {
using nlohmann::json;
namespace {
[[noreturn]] void bad(const std::string& message) { throw std::invalid_argument(message); }

void members(const json& object, const std::set<std::string>& allowed, const std::string& label) {
    if (!object.is_object()) bad(label + " must be an object");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.count(it.key())) bad("Unknown " + label + " property: " + it.key());
}

json signature(const json& snapshot, const std::set<json>& removed) {
    json variants = json::object();
    for (const auto& variant : snapshot.at("variants")) {
        json item = {{"selection", variant.at("selection")}, {"automatic", variant.value("automatic", false)}};
        if (!variant.value("automatic", false)) {
            std::set<json> ids;
            for (const auto& file : variant.at("files")) if (!removed.count(file.at("id"))) ids.insert(file.at("id"));
            item["files"] = ids;
            const auto& id = variant.at("defaultFileId");
            item["defaultFileId"] = removed.count(id) ? json(nullptr) : id;
        }
        variants[variant.at("key").get<std::string>()] = std::move(item);
    }
    return {{"revision", snapshot.at("templateRevisionId")}, {"variants", variants}};
}
}  // namespace

json selection(const std::vector<std::string>& flags) {
    json result = json::object();
    static const std::regex field("[a-z][a-z0-9_]{0,47}"), choice("[a-z0-9][a-z0-9._+\\-]{0,63}");
    for (const auto& flag : flags) {
        auto split = flag.find('=');
        if (split == std::string::npos || !std::regex_match(flag.substr(0, split), field) || !std::regex_match(flag.substr(split + 1), choice))
            bad("--compat needs field=value using template field and choice keys");
        std::string key = flag.substr(0, split);
        if (result.contains(key)) bad("Duplicate --compat field: " + key + "; use --variants-file for multiple combinations");
        result[key] = flag.substr(split + 1);
    }
    return result;
}

std::vector<Variant> read_manifest(const std::string& path) {
    constexpr std::size_t maximum = 1024 * 1024;
    std::ifstream file;
    if (path != "-") {
        file.open(std::filesystem::path(path), std::ios::binary);
        if (!file) bad("Can't read variants manifest: " + path);
    }
    std::istream& stream = path == "-" ? std::cin : file;
    std::string text(maximum + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (text.size() > maximum) bad("Variants manifests can be at most 1 MiB");
    // JSON normally accepts duplicate object keys by silently keeping the last value.
    std::vector<std::set<std::string>> keys;
    json document = json::parse(text, [&](int, json::parse_event_t event, json& parsed) {
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        else if (event == json::parse_event_t::object_end) keys.pop_back();
        else if (event == json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            bad("Duplicate JSON property: " + parsed.get<std::string>());
        return true;
    });
    members(document, {"variants"}, "manifest");
    if (!document.contains("variants") || !document["variants"].is_array() || document["variants"].empty() || document["variants"].size() > 256)
        bad("A manifest needs 1–256 variants");
    std::vector<Variant> result;
    for (const auto& variant : document["variants"]) {
        members(variant, {"key", "selection", "files", "defaultFile"}, "variant");
        if (!variant.contains("key") || !variant["key"].is_string() || !variant.contains("selection") || !variant["selection"].is_object()
            || !variant.contains("files") || !variant["files"].is_array()) bad("Each variant needs key, selection and files");
        Variant item{variant["key"].get<std::string>(), variant["selection"], {}, ""};
        for (const auto& name : variant["files"]) {
            if (!name.is_string() || name.get<std::string>().empty()) bad("Variant files must be release file names");
            item.files.push_back(name.get<std::string>());
        }
        if (variant.contains("defaultFile") && !variant["defaultFile"].is_null()) {
            if (!variant["defaultFile"].is_string()) bad("defaultFile must be a release file name");
            item.default_file = variant["defaultFile"].get<std::string>();
        }
        result.push_back(std::move(item));
    }
    return result;
}

void validate(const std::vector<Variant>& variants, const json& fields, const std::set<std::string>& available,
              bool complete, bool allow_deprecated) {
    if (!fields.is_array()) bad("The server did not supply a template schema");
    if (variants.size() > 256 || (complete && variants.empty())) bad("Declare compatible builds with --compat or --variants-file before publishing");
    std::set<std::string> names, combinations;
    static const std::regex key("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");
    for (const auto& variant : variants) {
        if (!std::regex_match(variant.key, key) || !names.insert(variant.key).second) bad("Variant keys must be unique, using 1–64 letters, digits, dots, dashes or underscores");
        if (!variant.selection.is_object()) bad("Variant selection must be an object");
        if (!combinations.insert(variant.selection.dump()).second) bad("Two variants declare the same compatibility combination");
        for (auto it = variant.selection.begin(); it != variant.selection.end(); ++it) {
            auto field = std::find_if(fields.begin(), fields.end(), [&](const json& f) { return f.at("key") == it.key(); });
            if (field == fields.end()) bad("Unknown compatibility field: " + it.key());
            if (!it.value().is_string()) bad("Compatibility choices must be strings: " + it.key());
            const auto& options = field->at("options");
            auto option = std::find_if(options.begin(), options.end(), [&](const json& o) { return o.at("key") == it.value(); });
            if (option == options.end()) bad("Unknown choice for " + it.key() + ": " + it.value().get<std::string>());
            if (!allow_deprecated && option->value("deprecated", false)) bad("This choice is deprecated: " + it.value().get<std::string>());
        }
        if (complete) for (const auto& field : fields)
            if (field.value("required", false) && !variant.selection.contains(field.at("key").get<std::string>()))
                bad("Missing required compatibility field: " + field.at("key").get<std::string>());
        std::set<std::string> files;
        if (variant.files.size() > 50) bad("Each variant can attach at most 50 files");
        for (const auto& name : variant.files)
            if (!files.insert(name).second || !available.count(name)) bad("Duplicate or unavailable variant file: " + name);
        if (!variant.default_file.empty() && !files.count(variant.default_file)) bad("The default file must be attached to its variant: " + variant.default_file);
        if (complete && variant.default_file.empty()) bad("Each variant needs a ready defaultFile before publishing");
    }
}

json replacement(const std::vector<Variant>& variants, const std::map<std::string, json>& files, const json& version) {
    json body = {{"expectedVersion", version}, {"variants", json::array()}};
    for (const auto& variant : variants) {
        json ids = json::array();
        for (const auto& name : variant.files) ids.push_back(files.at(name));
        json primary = variant.default_file.empty() ? json(nullptr) : files.at(variant.default_file);
        body["variants"].push_back({{"key", variant.key}, {"selection", variant.selection}, {"fileIds", ids}, {"defaultFileId", primary}});
    }
    return body;
}

bool unchanged(const json& before, const json& after, const std::set<json>& removed, bool automatic_changed) {
    if (removed.empty() && !automatic_changed && before.at("version") != after.at("version")) return false;
    return signature(before, removed) == signature(after, removed);
}
}  // namespace rsys

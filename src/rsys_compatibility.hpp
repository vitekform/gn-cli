#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rsys {

struct Variant {
    std::string key;
    nlohmann::json selection;
    std::vector<std::string> files;
    std::string default_file;
};

nlohmann::json selection(const std::vector<std::string>& flags);
std::vector<Variant> read_manifest(const std::string& path);
void validate(const std::vector<Variant>& variants, const nlohmann::json& fields,
              const std::set<std::string>& available, bool complete, bool allow_deprecated = false);
nlohmann::json replacement(const std::vector<Variant>& variants, const std::map<std::string, nlohmann::json>& files,
                           const nlohmann::json& version);

// File operations can update automatic variants and remove deleted file links. Everything
// else must still match the snapshot read before upload, before a replacement can be saved.
bool unchanged(const nlohmann::json& before, const nlohmann::json& after, const std::set<nlohmann::json>& removed,
               bool automatic_changed);

}  // namespace rsys

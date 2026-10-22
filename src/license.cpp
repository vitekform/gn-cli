#include "commands.hpp"

#include <iostream>
#include <string>

#include "notices.hpp"
#include "term.hpp"

namespace commands {

// The texts as they were when this binary was built: CMakeLists.txt embeds LICENSE and
// THIRD-PARTY-NOTICES into notices.hpp.

int license(const Context& ctx) {
    if (!ctx.args.empty()) {
        term::error("unknown license topic '" + ctx.args[0] + "'; there is only third-party");
        return 2;
    }
    std::cout << reinterpret_cast<const char*>(kLicense) << '\n'
              << std::string(79, '=') << "\n\n"
              << reinterpret_cast<const char*>(kThirdPartyNotices);
    return 0;
}

int license_third_party(const Context&) {
    std::cout << reinterpret_cast<const char*>(kThirdPartyNotices);
    return 0;
}

}  // namespace commands

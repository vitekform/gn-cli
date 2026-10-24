// Stock commands are retained only to explain the sunset. No auth or HTTP calls.
#include <iostream>

#include "commands.hpp"

namespace commands {

int stock_sunset(const Context& ctx) {
    if (ctx.json) {
        std::cout << R"({"error":"stock_sunset","message":"Stock system has been sunset","url":"https://ganamaga.me/blog/stock/sunset"})" << '\n';
    } else {
        std::cout << "Stock system has been sunset\n"
                  << "https://ganamaga.me/blog/stock/sunset\n";
    }
    return 1;
}

}  // namespace commands

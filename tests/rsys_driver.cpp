// Focused Linux test entry point: exercise the production parser and RSYS handlers.
// Function-section linking discards unrelated command groups and the normal entry point.
#define main unused_gn_cli_entrypoint
#include "../src/main.cpp"
#undef main

int main(int argc, char** argv) {
    Invocation base;
    if (const char* value = std::getenv("GN_TOKEN")) base.ctx.token = value;
    Invocation inv = parse(std::vector<std::string>(argv + 1, argv + argc), base);
    inv.topts.interactive = false;
    inv.topts.color = false;
    term::init(inv.topts);
    http::global_init();
    int code = 2;
    try {
        if (!inv.bad.empty()) throw std::invalid_argument(inv.bad);
        if (inv.positional.size() < 2 || inv.positional[0] != "rsys") throw std::invalid_argument("Expected rsys command");
        inv.ctx.args.assign(inv.positional.begin() + 2, inv.positional.end());
        const auto& verb = inv.positional[1];
        if (verb == "publish") code = commands::rsys_publish(inv.ctx);
        else if (verb == "latest") code = commands::rsys_latest(inv.ctx);
        else if (verb == "download") code = commands::rsys_download(inv.ctx);
        else throw std::invalid_argument("Unsupported test command");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; code = 1; }
    http::global_cleanup();
    return code;
}

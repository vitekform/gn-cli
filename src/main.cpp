// gn-cli — command-line client for the nextgen internal API.
#include <cstddef>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "api.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "http.hpp"
#include "lineedit.hpp"
#include "term.hpp"

namespace fs = std::filesystem;

namespace {

void print_help() {
    using namespace term;
    const auto& G = g();
    std::cout << bold(cyan("gn-cli")) << ' ' << dim(std::string("v") + GN_CLI_VERSION) << ' ' << dim(G.dash) << ' '
              << "command-line client for the nextgen API\n\n"
              << bold("Usage") << "\n"
              << "  gn-cli [options] <command> [command options]\n"
              << "  gn-cli               With no command, in interactive mode: a shell for commands\n"
              << "  " << dim("gn and gncli are aliases of gn-cli.") << "\n\n"
              << bold("Commands") << "\n"
              << "  " << green("auth login") << "          Log in and save the session\n"
              << "      -e, --email <email>   Email address (prompted when omitted)\n"
              << "      --password-stdin      Read the password from standard input\n"
              << "  " << green("auth logout") << "         Forget the saved session\n"
              << "  " << green("auth register") << "       Create an account (then signs in, offers to verify the email)\n"
              << "  " << green("auth verify-email") << "   Email yourself a code and enter it\n"
              << "  " << green("info") << ", " << green("me") << "            Show everything about your account\n"
              << "      --json                Print the raw data as JSON\n"
              << "  " << green("token scopes") << "        List the scopes a CLI token can be given\n"
              << "  " << green("token list") << "          List your CLI tokens\n"
              << "  " << green("token create") << " <name>  Create a CLI token (shown once)\n"
              << "      -s, --scope <scope>   Grant a scope; repeat it or separate with commas\n"
              << "      --expires <when>      30d, 12w, 1y or never (default 90d)\n"
              << "  " << green("token revoke") << " <id>    Revoke a CLI token\n"
              << "  " << green("credits") << "             Balance and the last 30 requests (--watch refreshes)\n"
              << "  " << green("passkeys") << " ls|rm <id>  Your passkeys; registering one stays in the browser\n"
              << "  " << green("api-tokens") << " ls|create <name>|revoke <id>|models\n"
              << "                      OpenAI-compatible API keys (openAIAPI permission)\n\n"
              << bold("S3 storage") << " " << dim("(needs a login session and the s3 permission)") << "\n"
              << "  " << green("s3 ls") << " [provider [bucket [prefix]]]\n"
              << "                      Providers, a provider's buckets, or a folder's objects\n"
              << "      -r, --recursive       Every object below the prefix, not just one level\n"
              << "  " << green("s3 upload") << " <provider> <bucket> <bucket_path> <local_path>\n"
              << "                      Upload in parts; a bucket_path ending in / keeps the file name\n"
              << "      -r, --recursive       Upload a folder's contents under bucket_path\n"
              << "      --part-size <MiB>     Ignored: the server sizes the parts\n"
              << "      --content-type <t>    Override the type guessed from the extension\n"
              << "  " << green("s3 download") << " <provider> <bucket> <path> [local_path]\n"
              << "                      Download a file (- for stdout), or a folder with -r or a trailing /\n"
              << "      -o, --output <path>   Where to save (default: the name in the current folder)\n"
              << "      -f, --force           Overwrite local files\n"
              << "  " << green("s3 cat") << " <provider> <bucket> <path>          Print a file to stdout\n"
              << "  " << green("s3 delete") << " <provider> <bucket> <path>       Delete a file, or a folder (-r or /)\n"
              << "      -y, --yes             Don't ask before deleting a folder\n"
              << "  " << green("s3 rename") << " <provider> <bucket> <old> <new>  Rename/move a file, or a folder (-r or /)\n"
              << "  " << green("s3 mkdir") << " <provider> <bucket> <path>        Create an empty folder\n"
              << "  " << green("s3 mb") << " <provider> <bucket>                 Create a bucket\n"
              << "  " << green("vault use") << " <provider> <bucket>        Where the secure-storage vault is\n"
              << "  " << green("vault ls") << " / " << green("get") << " <file> / " << green("link") << " <file>   What the key opens; extract one; share it\n"
              << "  " << green("vault hide") << " <file> <cover>...        Encrypt a file into images (checks they hold it)\n"
              << "      --keyfile <path>      Lock with a key file instead of a password (--password-stdin too)\n"
              << "  " << green("s3 share") << " <provider> <bucket> <path>...  Public link to files or folders (/ or -r)\n"
              << "      --expires <time>      5m to 30d, like 90m, 12h, 7d (default 24h)\n"
              << "  " << green("s3 shares") << "                   Your share links\n"
              << "  " << green("s3 unshare") << " <id|link>...     Revoke share links\n"
              << "  " << green("share get") << " <link> [-o <dir>]  Download a share link's files (no login); --zip for one zip\n\n"
              << bold("Release system") << " " << dim("(rsys.ganamaga.me; needs the release_system permission, except")
              << "\n" << dim("                latest and download of public projects, which need no login)") << "\n"
              << "  " << green("rsys latest") << " <project>        Newest release: version, files, changelogs\n"
              << "      --prerelease          Count prereleases as newest too\n"
              << "      --since <version>     Changelogs of everything after the version you have\n"
              << "  " << green("rsys download") << " <project> [version]\n"
              << "                      Download the newest (or that) release's primary file; checks sha256\n"
              << "      --file <name>         Another file of the release\n"
              << "      -o, --output <path>   Where to save (default: its name in the current folder; - for stdout)\n"
              << "  " << green("rsys publish") << " <project> <version> <file>[#name]...\n"
              << "                      Create the release, upload its files, publish it (re-run to resume);\n"
              << "                      with no files, publish an existing draft\n"
              << "      --title <text>        Release title\n"
              << "      --changelog <text>    Release notes (Markdown); or --changelog-file <path|->\n"
              << "      --prerelease          Mark it a prerelease (never \"latest\" by default)\n"
              << "      --primary <name>      The file public/latest points at (default: the first)\n"
              << "      --draft               Upload but don't publish yet\n"
              << "      --append              Add files to a release that is already published\n"
              << "      --compat <field=value> Repeat per field; publish one combination, or resolve latest/download\n"
              << "      --variants-file <json> Publish explicit combinations and file/default mappings\n"
              << "  " << green("rsys projects") << "                   List projects\n"
              << "  " << green("rsys project") << " <project>         A project and its releases\n"
              << "  " << green("rsys releases") << " <project>        Its releases, drafts included\n"
              << "  " << green("rsys show") << " <project> [version]  One release with its files (default: latest)\n"
              << "  " << green("rsys edit") << " <project> <version>  Change --title, --changelog[-file], --prerelease/--stable\n"
              << "  " << green("rsys yank") << " / " << green("unyank") << " <project> <version>\n"
              << "                      Hide a published release from the public, or bring it back\n"
              << "  " << green("rsys delete") << " <project> <version>  Delete a release and its files (-y to skip asking)\n"
              << "  " << green("rsys new-project") << " <slug>        Create a project: --name, --description, --public\n"
              << "  " << green("rsys edit-project") << " <slug>       Change --name, --description, --public/--private\n"
              << "  " << green("rsys delete-project") << " <slug>     Delete it with every release (-y to skip asking)\n"
              << "  " << green("rsys hash") << " <file>...          SHA-256 of local files, like sha256sum\n\n"
              << bold("Temporary files") << " " << dim("(a public link that expires; get needs no login)") << "\n"
              << "  " << green("drop") << " <file>...               Upload, print the link (only the link when piped)\n"
              << "      --ttl <ttl>           How long it lives: 30m, 1h, 3h, 6h, 12h, 24h, 3d, 7d, 1mo\n"
              << "                            (default: the longest up to 24h your level allows)\n"
              << "      --max-downloads <n>   Expire after n downloads\n"
              << "  " << green("drop ls") << "                     Your temporary files and their links\n"
              << "  " << green("drop rm") << " <id|link>...        Delete them; the links stop working\n"
              << "  " << green("drop limits") << "                 Largest file and the TTLs your level may pick\n"
              << "  " << green("drop get") << " <link> [-o <path>]  Download one (spends a download)\n\n"
              << bold("Persistent files") << " " << dim("(a public link until you delete it; level 2+; get needs no login)") << "\n"
              << "  " << green("files") << " <file>...              Upload, print the link (only the link when piped)\n"
              << "  " << green("files ls") << "                    Your persistent files, their links and downloads\n"
              << "  " << green("files rm") << " <id|link>...       Delete them; the links stop working\n"
              << "  " << green("files limits") << "                Largest file your level may upload\n"
              << "  " << green("files get") << " <link> [-o <path>] Download one\n\n"
              << bold("AI chat") << " " << dim("(needs the aiChatBasic permission; paid models spend credits)") << "\n"
              << "  " << green("chat") << "                        A chat in the terminal: /model, /new, /exit; replies stream\n"
              << "  " << green("chat") << " <question>             Ask once; stdin is attached: git diff | gn chat \"review this\"\n"
              << "      -m, --model <name|id>  Which model (default: the last one used)\n"
              << "      --chat <id>           Continue that chat instead of starting one\n"
              << "      --reasoning           Show the model's reasoning (on stderr)\n"
              << "  " << green("chat ls") << " / " << green("show") << " <id> / " << green("rm") << " <id>   Your chats\n"
              << "  " << green("chat models") << "                 Models, their prices and context sizes\n\n"
              << bold("Roleplay") << " " << dim("(aiRoleplayBasic; characters are made in the web app)") << "\n"
              << "  " << green("rp new") << " <character>[,…]      Start a chat and talk (--as <persona>, -m <model>, --name)\n"
              << "  " << green("rp") << " <chat>                   Carry on in one: /regen, /history, /exit\n"
              << "  " << green("rp chats") << " / " << green("show") << " <chat> / " << green("rm") << " <chat> / " << green("characters") << " / " << green("personas") << "\n\n"
              << bold("Images and speech") << " " << dim("(the aiImagen / aiTTS permissions)") << "\n"
              << "  " << green("imagen") << " <prompt>             Generate an image, wait, save it (-o, default imagen-<id>.png)\n"
              << "      -m, --model <id>      gn imagen models lists them (default flux1-dev-fp8)\n"
              << "      --size <s>            square, portrait, landscape, tall, wide or WxH\n"
              << "      --negative <text>     What to keep out; --steps, --guidance to tune\n"
              << "      --no-wait             Start it and return; gn imagen get <id> fetches it\n"
              << "  " << green("imagen ls") << " / " << green("get") << " <id> / " << green("models") << "\n"
              << "  " << green("tts") << " <text>                Speak it with one of your voices into speech.wav (-o; - for stdout)\n"
              << "      --voice <name|id>     Which voice (needed when you have more than one)\n"
              << "  " << green("tts voices") << " / " << green("add") << " <name> <sample> / " << green("rm") << " <id>\n"
              << "  " << green("breeze") << " <text>             Speak it on the Breeze XPU server into speech.wav (-o; - streams)\n"
              << "      --voice <name>        One of your Breeze voices (clones it)\n"
              << "      --instruction <how>   The voice or delivery in words; with --voice, directs it\n"
              << "      --ref <clip> --ref-text <words>  Clone a one-off clip instead of a saved voice\n"
              << "      --format <f>          wav, flac or pcm; --seed, --guidance to tune\n"
              << "  " << green("breeze voices") << " / " << green("add") << " <name> <clip> <transcript> / " << green("rm") << " <name>\n"
              << "  " << green("breeze status") << " / " << green("load") << " / " << green("unload") << "   The model on the XPU (unload: admins)\n\n"
              << bold("Admin") << " " << dim("(aiAdmin or level 4; login session only; changes ask first unless -y)") << "\n"
              << "  " << green("admin users") << " [filter]          Everyone: credits and access\n"
              << "  " << green("admin user") << " <id|email>          One user with their credits and usage\n"
              << "  " << green("admin create-user") << " <email> <perm,...>  An account with only those permissions\n"
              << "  " << green("admin perms") << " <user> [set <perm,...> | role]  Show or change access\n"
              << "  " << green("admin password") << " <user>          Set a new one (root; --password-stdin)\n"
              << "  " << green("admin credits") << " <user> [set|add|sub <amount>]\n"
              << "  " << green("admin usage") << " / " << green("providers") << " / " << green("models") << " / " << green("oauth-clients") << "\n\n"
              << bold("Smart home") << " " << dim("(the smartHome permission)") << "\n"
              << "  " << green("home") << "                        Devices with their readings (also: home rooms)\n"
              << "  " << green("home room") << " <room> <°C> [±°C]  Set a room's target temperature\n"
              << "  " << green("home poll") << "                   Ask every device to report, then list them\n"
              << "  " << green("home config") << " <device> room=… task=… state=…   Reconfigure one (id or its start)\n"
              << "  " << green("home reset") << " <device>  /  " << green("home fun") << " <device> on|off\n\n"
              << bold("Booru and comics") << " " << dim("(the gelbooru permission)") << "\n"
              << "  " << green("booru") << " <tags>                Search the mirror (--page, --limit, --no-ai)\n"
              << "  " << green("booru tags") << " <prefix>  /  " << green("booru show") << " <id>\n"
              << "  " << green("booru get") << " <id>...  /  " << green("booru dl") << " <tags> [--limit N]   Download originals (-o dir)\n"
              << "  " << green("comics") << " [words]              Search comics (--tag, --author, --page, --limit)\n"
              << "  " << green("comics show") << " <uid>  /  " << green("comics get") << " <uid> [-o dir]   Details; every page (resumable)\n"
              << "  " << green("comics tags") << " / " << green("authors") << " [words]\n\n"
              << bold("KHK applications") << " " << dim("(the khkAdmin permission)") << "\n"
              << "  " << green("khk ls") << " [pending|approved|rejected]  Membership applications\n"
              << "  " << green("khk show") << " <id>  /  " << green("khk pdf") << " <id> [-o file]\n"
              << "  " << green("khk accept") << " <id>              Publish on the KHK website and email the applicant (asks)\n"
              << "  " << green("khk reject") << " <id>  /  " << green("khk pending") << " <id>   Change only the status\n\n"
              << bold("Kathars") << " " << dim("(the Minecraft network; staff and punishments are public)") << "\n"
              << "  " << green("kathars staff") << "               The staff list, by group\n"
              << "  " << green("kathars punishments") << " [player]  Bans, mutes and the rest, active first\n"
              << "  " << green("kathars add-member") << " name=… group=… [role=…] [mc=…] [order=…]\n"
              << "  " << green("kathars edit-member") << " <member> field=…  /  " << green("rm-member") << " <member>\n"
              << "  " << green("kathars add-group") << " name=… [color=#…] [order=…]  /  " << green("edit-group") << "  /  " << green("rm-group") << "\n"
              << "                      " << dim("Editing needs the katharsAdmin permission.") << "\n\n"
              << bold("BakaLink") << " " << dim("(needs the server's BAKALINK_TOKEN; a CLI token also bakalink:read / bakalink:manage)") << "\n"
              << "  " << green("bakalink accounts") << "            The Bakaláři accounts data is kept for\n"
              << "  " << green("bakalink account add") << " name=… [school=… user=… full-name=… class=…]  /  " << green("edit") << " <account> …  /  " << green("rm") << " <account>\n"
              << "  " << green("bakalink komens") << "              Received messages (--since <time>, --limit <n>)\n"
              << "  " << green("bakalink komens show") << " <id>  /  " << green("get") << " <id> [attachment] [-o path]  /  " << green("rm") << " <id>\n"
              << "  " << green("bakalink komens push") << " --file <json|->   Store or update messages by their Bakaláři id\n"
              << "  " << green("bakalink komens send") << " [files...]        One message from options:\n"
              << "      --title <t>, --content <text> | --content-file <path|->   At least one of them\n"
              << "      --id <Bakaláři id>, --author-id <id>, --author <name>, --kind received|noticeboard, --at <time>\n"
              << "  " << green("bakalink marks") << "               Marks by subject (--subject <id|abbrev|name>)\n"
              << "  " << green("bakalink marks push") << " --file <json|-> [--prune]   --prune deletes the marks the file leaves out\n"
              << "  " << green("bakalink marks add") << " --subject <id> --mark <m> [--weight n] [--caption t] [--teacher id] [--at time] [--id id]\n"
              << "  " << green("bakalink marks rm") << " <id>\n"
              << "  " << green("bakalink timetable") << "           The actual timetable (--from, --to as yyyy-mm-dd; this week by default)\n"
              << "  " << green("bakalink timetable push") << " --file <json|->   Replace the days between its from and to\n"
              << "  " << green("bakalink timetable add") << " --date <d> --hour <id> [--subject id] [--teacher id] [--room r] [--change type] [--description t]\n"
              << "  " << green("bakalink timetable rm") << " <lesson id>\n"
              << "  " << green("bakalink timetable permanent") << "  /  " << green("permanent push") << " --file <json|->   The permanent timetable\n"
              << "      --account <id|name>   The account, for everything but accounts (or $GN_BAKALINK_ACCOUNT)\n"
              << "      --bakalink-token <t>  The token (or $GN_BAKALINK_TOKEN)\n\n"
              << bold("Markets (sunset)") << "\n"
              << "  " << green("stock") << "                       Stock system has been sunset\n"
              << "      https://ganamaga.me/blog/stock/sunset\n\n"
              << bold("Updating") << "\n"
              << "  " << green("update") << "                      Replace this gn-cli with the newest release (sha256-checked)\n"
              << "      --check               Only say whether there is one (exit 10 if so)\n"
              << "      --prerelease          Take dev builds too\n"
              << "      -f, --force           Reinstall even when up to date\n\n"
              << bold("Licence and terms") << "\n"
              << "  " << green("license") << "                     gn-cli's licence and the third-party notices\n"
              << "  " << green("license third-party") << "         Only the third-party notices\n"
              << "  " << green("terms") << "                       The Terms of Service\n"
              << "  " << green("terms status") << "                Whether you have accepted the terms and licence, and when\n"
              << "  " << green("terms accept") << "                Accept them now (asks); needed once, then again when they change\n"
              << "  " << green("terms revoke") << "                Forget the acceptance; gn-cli asks again on the next command\n"
              << "      --accept-terms        Accept the terms and licence without asking (or GN_ACCEPT_TERMS=1)\n\n"
              << bold("Options") << "\n"
              << "  --no-unicode          Plain ASCII output, no box drawing or symbols\n"
              << "  --no-color            No colours (also: NO_COLOR=1)\n"
              << "  --token <token>       Authenticate with a CLI token instead of the saved login\n"
              << "                        (or $GN_TOKEN); it only reaches the calls its scopes allow\n"
              << "  --api-url <url>       API server (default " << api::kDefaultUrl << ", or $GN_API_URL)\n"
              << "  --rsys-url <url>      Release system server (default " << api::kDefaultRsysUrl
              << ",\n                        or $GN_RSYS_URL, or --api-url when that is given)\n"
              << "  --noninteractive      Never ask anything: no prompts for missing arguments, the\n"
              << "                        login or confirmations (also GN_NONINTERACTIVE=1,\n"
              << "                        DEBIAN_FRONTEND=noninteractive, CI=true, or a piped stdin)\n"
              << "  --debug               Log every request and response to stderr, credentials\n"
              << "                        redacted (or GN_DEBUG=1)\n"
              << "  -h, --help            Show this help\n"
              << "  -V, --version         Show the version\n";
}

int usage_error(const std::string& msg) {
    term::error(msg);
    std::cerr << "  Run " << term::bold("gn-cli --help") << " for usage.\n";
    return 2;
}

// One command line, parsed. Options may appear anywhere: `gn-cli --no-unicode me` and
// `gn-cli me --no-unicode` both work.
struct Invocation {
    term::Options topts;
    commands::Context ctx;
    bool help = false, version = false, debug = false;
    std::vector<std::string> positional;
    std::string bad;
};

Invocation parse(const std::vector<std::string>& args, Invocation inv) {
    commands::Context& ctx = inv.ctx;
    std::string& bad = inv.bad;
    std::string scope, compatibility;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::string_view name, std::string& out) -> bool {
            std::string prefix = std::string(name) + "=";
            if (a.rfind(prefix, 0) == 0) {
                out = a.substr(prefix.size());
                return true;
            }
            if (a == name) {
                if (i + 1 >= args.size()) {
                    bad = std::string(name) + " needs a value";
                    return true;
                }
                out = args[++i];
                return true;
            }
            return false;
        };

        if (a == "--no-unicode") inv.topts.unicode = false;
        else if (a == "--no-color" || a == "--no-colour") inv.topts.color = false;
        else if (a == "--noninteractive" || a == "--non-interactive") inv.topts.interactive = false;
        else if (a == "-h" || a == "--help") inv.help = true;
        else if (a == "-V" || a == "--version") inv.version = true;
        else if (a == "--json") ctx.json = true;
        else if (a == "--debug") inv.debug = true;
        else if (a == "--password-stdin") ctx.password_stdin = true;
        else if (a == "-r" || a == "--recursive") ctx.recursive = true;
        else if (a == "--accept-terms") ctx.accept_terms = true;
        else if (a == "-y" || a == "--yes") ctx.yes = true;
        else if (a == "-f" || a == "--force") ctx.force = true;
        else if (value("--output", ctx.output) || value("-o", ctx.output)) {}
        else if (value("--part-size", ctx.part_size)) {}
        else if (value("--content-type", ctx.content_type)) {}
        else if (value("--api-url", ctx.api_url)) {}
        else if (value("--rsys-url", ctx.rsys_url)) {}
        else if (a == "--public") ctx.set_public = true;
        else if (a == "--private") ctx.set_private = true;
        else if (a == "--prerelease") ctx.prerelease = true;
        else if (a == "--stable") ctx.stable = true;
        else if (a == "--draft") ctx.draft = true;
        else if (a == "--append") ctx.append = true;
        else if (a == "--zip") ctx.zip = true;
        else if (a == "--check") ctx.check = true;
        else if (a == "--reasoning") ctx.reasoning = true;
        else if (a == "--watch") ctx.watch = true;
        else if (a == "--no-wait") ctx.no_wait = true;
        else if (value("--size", ctx.size)) {}
        else if (value("--negative", ctx.negative)) {}
        else if (value("--steps", ctx.steps)) {}
        else if (value("--guidance", ctx.guidance)) {}
        else if (value("--voice", ctx.voice)) {}
        else if (value("--instruction", ctx.instruction)) {}
        else if (value("--ref-text", ctx.ref_text)) {}
        else if (value("--ref", ctx.ref_audio)) {}
        else if (value("--seed", ctx.seed)) {}
        else if (value("--format", ctx.format)) {}
        else if (value("--period", ctx.period)) {}
        else if (value("--interval", ctx.interval)) {}
        else if (value("--limit", ctx.limit)) {}
        else if (value("--page", ctx.page)) {}
        else if (value("--tag", ctx.tag)) {}
        else if (value("--author", ctx.author)) {}
        else if (a == "--no-ai") ctx.no_ai = true;
        else if (value("--as", ctx.persona)) {}
        else if (value("--keyfile", ctx.keyfile)) {}
        else if (value("--model", ctx.model) || value("-m", ctx.model)) {}
        else if (value("--chat", ctx.chat)) {}
        else if (value("--name", ctx.name)) {}
        else if (value("--description", ctx.description)) {}
        else if (value("--title", ctx.title)) {}
        else if (value("--changelog-file", ctx.changelog_file)) {}
        else if (value("--changelog", ctx.changelog)) {}
        else if (value("--primary", ctx.primary)) {}
        else if (value("--file", ctx.file)) {}
        else if (value("--since", ctx.since)) {}
        else if (value("--compat", compatibility)) ctx.compatibility.push_back(compatibility);
        else if (value("--variants-file", ctx.variants_file)) {
            if (ctx.variants_file.empty()) bad = "--variants-file needs a manifest path";
        }
        else if (value("--ttl", ctx.ttl)) {}
        else if (value("--max-downloads", ctx.max_downloads)) {}
        else if (value("--email", ctx.email) || value("-e", ctx.email)) {}
        else if (value("--token", ctx.token)) {}
        else if (value("--expires", ctx.expires)) {}
        else if (value("--scope", scope) || value("-s", scope)) ctx.scopes.push_back(scope);
        else if (value("--bakalink-token", ctx.bakalink_token)) {}
        else if (value("--account", ctx.account)) {}
        else if (value("--id", ctx.baka_id)) {}
        else if (value("--content-file", ctx.content_file)) {}
        else if (value("--content", ctx.content)) {}
        else if (value("--author-id", ctx.author_id)) {}
        else if (value("--kind", ctx.kind)) {}
        else if (value("--at", ctx.at)) {}
        else if (a == "--prune") ctx.prune = true;
        else if (value("--subject", ctx.subject)) {}
        else if (value("--mark", ctx.mark)) {}
        else if (value("--weight", ctx.weight)) {}
        else if (value("--caption", ctx.caption)) {}
        else if (value("--teacher", ctx.teacher)) {}
        else if (value("--from", ctx.from)) {}
        else if (value("--to", ctx.to)) {}
        else if (value("--date", ctx.date)) {}
        else if (value("--hour", ctx.hour)) {}
        else if (value("--room", ctx.room)) {}
        else if (value("--change", ctx.change)) {}
        else if (a == "--") {
            inv.positional.insert(inv.positional.end(), args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        } else if (a.size() > 1 && a[0] == '-') {
            if (bad.empty()) bad = "unknown option " + a;
        } else inv.positional.push_back(a);
    }
    return inv;
}

// ── the command table ─────────────────────────────────────────────────────
//
// Dispatch, "needs a subcommand" errors and completion all come from here; the help text is
// written by hand above. Every name in a list works; the first is the one shown.

using Run = int (*)(const commands::Context&);

struct Sub {
    std::vector<std::string> names;
    Run run;
};

struct Group {
    std::vector<std::string> names;
    std::vector<Sub> subs;  // empty: the group is a command by itself
    Run run = nullptr;      // without subs: the command; with subs: what any other word runs (drop <file>)
};

int help_command(const commands::Context&) {
    print_help();
    return 0;
}

int completion_command(const commands::Context& ctx);

const std::vector<Group>& groups() {
    using namespace commands;
    static const std::vector<Group> table = {
        {{"auth"},
         {{{"login"}, auth_login}, {{"logout"}, auth_logout}, {{"register", "signup"}, auth_register},
          {{"verify-email"}, auth_verify_email}}},
        {{"info", "me"}, {}, info},
        {{"token", "tokens"},
         {{{"scopes"}, token_scopes}, {{"list", "ls"}, token_list}, {{"create", "new"}, token_create},
          {{"revoke", "rm", "delete"}, token_revoke}}},
        {{"s3"},
         {{{"ls", "list"}, s3_list}, {{"providers"}, s3_providers}, {{"buckets"}, s3_buckets},
          {{"upload", "put"}, s3_upload}, {{"download", "get"}, s3_download}, {{"cat"}, s3_cat},
          {{"delete", "rm"}, s3_delete}, {{"rename", "mv"}, s3_rename}, {{"mkdir"}, s3_mkdir},
          {{"mb", "create-bucket"}, s3_make_bucket}, {{"share"}, s3_share}, {{"shares"}, s3_shares},
          {{"unshare"}, s3_unshare}}},
        {{"rsys", "release", "releases"},
         {{{"projects", "ls"}, rsys_projects}, {{"project"}, rsys_project}, {{"releases"}, rsys_releases},
          {{"show", "info"}, rsys_show}, {{"latest"}, rsys_latest}, {{"download", "get"}, rsys_download},
          {{"publish", "push"}, rsys_publish}, {{"edit"}, rsys_edit_release}, {{"yank"}, rsys_yank},
          {{"unyank"}, rsys_unyank}, {{"delete", "rm"}, rsys_delete_release},
          {{"new-project", "create-project"}, rsys_new_project}, {{"edit-project"}, rsys_edit_project},
          {{"delete-project"}, rsys_delete_project}, {{"hash", "sha256"}, rsys_hash}}},
        {{"drop", "tmp"},
         {{{"put", "upload"}, drop_put}, {{"ls", "list"}, drop_list}, {{"rm", "delete"}, drop_remove},
          {{"limits"}, drop_limits}, {{"get", "download"}, drop_get}},
         drop_put},
        {{"files", "pf"},
         {{{"put", "upload"}, files_put}, {{"ls", "list"}, files_list}, {{"rm", "delete"}, files_remove},
          {{"limits"}, files_limits}, {{"get", "download"}, files_get}},
         files_put},
        {{"share"}, {{{"get", "download"}, share_get}}},
        {{"api-tokens", "api-token"},
         {{{"ls", "list"}, api_tokens_list}, {{"create", "new"}, api_tokens_create},
          {{"revoke", "rm", "delete"}, api_tokens_revoke}, {{"models"}, api_tokens_models}}},
        {{"credits"}, {}, commands::credits},
        {{"vault"},
         {{{"use"}, vault_use}, {{"ls", "list"}, vault_list}, {{"get", "open"}, vault_get}, {{"hide"}, vault_hide},
          {{"link", "share"}, vault_link}}},
        {{"rp", "roleplay"},
         {{{"new", "start"}, rp_new}, {{"chats", "ls"}, rp_chats}, {{"show"}, rp_show}, {{"characters"}, rp_characters},
          {{"personas"}, rp_personas}, {{"rm", "delete"}, rp_remove}},
         rp_continue},
        {{"booru", "gelbooru"},
         {{{"search", "ls"}, booru_search}, {{"tags"}, booru_tags}, {{"show"}, booru_show}, {{"get"}, booru_get},
          {{"dl", "download"}, booru_download}},
         booru_search},
        {{"comics", "comic"},
         {{{"search", "ls"}, comics_search}, {{"show"}, comics_show}, {{"get", "download"}, comics_get},
          {{"tags"}, comics_tags}, {{"authors"}, comics_authors}},
         comics_search},
        {{"khk"},
         {{{"ls", "list"}, khk_list}, {{"show"}, khk_show}, {{"pdf"}, khk_pdf}, {{"accept", "approve"}, khk_accept},
          {{"reject"}, khk_reject}, {{"pending"}, khk_pending}}},
        {{"kathars"},
         {{{"staff"}, kathars_staff}, {{"punishments", "bans"}, kathars_punishments}, {{"add-member"}, kathars_add_member},
          {{"edit-member"}, kathars_edit_member}, {{"rm-member"}, kathars_remove_member}, {{"add-group"}, kathars_add_group},
          {{"edit-group"}, kathars_edit_group}, {{"rm-group"}, kathars_remove_group}}},
        {{"bakalink"},
         {{{"accounts", "account"}, bakalink_accounts}, {{"komens"}, bakalink_komens}, {{"marks", "znamky"}, bakalink_marks},
          {{"timetable", "rozvrh"}, bakalink_timetable}}},
        {{"home", "smarthome"},
         {{{"nodes", "devices", "ls"}, home_nodes}, {{"rooms"}, home_rooms}, {{"room"}, home_room},
          {{"poll"}, home_poll}, {{"config"}, home_config}, {{"reset"}, home_reset}, {{"fun"}, home_fun}},
         home_nodes},
        {{"stock", "stocks"}, {}, stock_sunset},
        {{"admin"},
         {{{"users"}, admin_users}, {{"user"}, admin_user}, {{"create-user"}, admin_create_user},
          {{"perms", "permissions"}, admin_perms}, {{"password"}, admin_password}, {{"credits"}, admin_credits},
          {{"usage"}, admin_usage}, {{"providers"}, admin_providers}, {{"models"}, admin_models},
          {{"oauth-clients"}, admin_oauth_clients}}},
        {{"imagen", "image"},
         {{{"generate", "gen"}, imagen_generate}, {{"get"}, imagen_get}, {{"ls", "list"}, imagen_list},
          {{"models"}, imagen_models}},
         imagen_generate},
        {{"tts", "say"},
         {{{"say"}, tts_say}, {{"voices"}, tts_voices}, {{"add"}, tts_add}, {{"rm", "delete"}, tts_remove}},
         tts_say},
        {{"breeze"},
         {{{"say"}, breeze_say}, {{"voices", "ls"}, breeze_voices}, {{"add"}, breeze_add}, {{"rm", "delete"}, breeze_remove},
          {{"status"}, breeze_status}, {{"load"}, breeze_load}, {{"unload"}, breeze_unload}},
         breeze_say},
        {{"passkeys", "passkey"}, {{{"ls", "list"}, passkeys_list}, {{"rm", "delete"}, passkeys_remove}}},
        {{"chat", "ai"},
         {{{"ask"}, chat}, {{"ls", "list"}, chat_list}, {{"show"}, chat_show}, {{"rm", "delete"}, chat_remove},
          {{"models"}, chat_models}},
         chat},
        {{"update", "self-update", "upgrade"}, {}, commands::update},
        {{"license", "licence", "licenses"}, {{{"third-party"}, license_third_party}}, license},
        {{"terms", "tos"},
         {{{"accept"}, terms_accept}, {{"revoke"}, terms_revoke}, {{"status"}, terms_status}},
         terms},
        {{"completion"}, {}, completion_command},
        {{"help"}, {}, help_command},
    };
    return table;
}

bool named(const std::vector<std::string>& names, const std::string& word) {
    return std::find(names.begin(), names.end(), word) != names.end();
}

const Group* find_group(const std::string& word) {
    for (const auto& g : groups())
        if (named(g.names, word)) return &g;
    return nullptr;
}

// Options as completion offers them; parse() is what actually reads them.
struct OptionSpec {
    const char* name;
    bool takes_value;
};
constexpr OptionSpec kOptions[] = {
    {"--help", false}, {"--version", false}, {"--accept-terms", false}, {"--json", false}, {"--debug", false}, {"--no-unicode", false},
    {"--no-color", false}, {"--noninteractive", false}, {"--token", true}, {"--api-url", true}, {"--rsys-url", true},
    {"--email", true}, {"--password-stdin", false}, {"--scope", true}, {"--expires", true}, {"--recursive", false},
    {"--yes", false}, {"--force", false}, {"--output", true}, {"--part-size", true}, {"--content-type", true},
    {"--name", true}, {"--description", true}, {"--public", false}, {"--private", false}, {"--title", true},
    {"--changelog", true}, {"--changelog-file", true}, {"--prerelease", false}, {"--stable", false},
    {"--compat", true}, {"--variants-file", true}, {"--draft", false}, {"--append", false}, {"--primary", true}, {"--file", true}, {"--since", true},
    {"--ttl", true}, {"--max-downloads", true}, {"--zip", false}, {"--check", false}, {"--model", true},
    {"--chat", true}, {"--reasoning", false}, {"--watch", false}, {"--size", true}, {"--negative", true},
    {"--steps", true}, {"--guidance", true}, {"--no-wait", false}, {"--voice", true}, {"--instruction", true},
    {"--ref", true}, {"--ref-text", true}, {"--seed", true}, {"--format", true}, {"--period", true},
    {"--interval", true}, {"--limit", true}, {"--page", true}, {"--tag", true}, {"--author", true}, {"--no-ai", false}, {"--as", true}, {"--keyfile", true},
    {"--bakalink-token", true}, {"--account", true}, {"--id", true}, {"--content", true}, {"--content-file", true},
    {"--author-id", true}, {"--kind", true}, {"--at", true}, {"--prune", false}, {"--subject", true}, {"--mark", true},
    {"--weight", true}, {"--caption", true}, {"--teacher", true}, {"--from", true}, {"--to", true}, {"--date", true},
    {"--hour", true}, {"--room", true}, {"--change", true},
};
constexpr const char* kShortValueOptions[] = {"-o", "-e", "-s", "-m"};

bool takes_value(const std::string& word) {
    for (const auto& o : kOptions)
        if (word == o.name) return o.takes_value;
    return std::find(std::begin(kShortValueOptions), std::end(kShortValueOptions), word) != std::end(kShortValueOptions);
}

// What can come next, given the words before the cursor and the word being typed. Empty means
// "nothing gn-cli knows of": shells then fall back to file names.
std::vector<std::string> complete(const std::vector<std::string>& before, const std::string& current) {
    std::vector<std::string> out;
    std::string prev = before.empty() ? "" : before.back();
    if (takes_value(prev)) {
        if (prev == "--ttl") out = {"30m", "1h", "3h", "6h", "12h", "24h", "3d", "7d", "1mo"};
        else if (prev == "--period") out = {"1d", "5d", "1m", "3m", "6m", "ytd", "1y", "5y", "max"};
        else if (prev == "--interval") out = {"auto", "1min", "5min", "15min", "1hour", "1day", "1week", "1month"};
        else if (prev == "--size") out = {"square", "portrait", "landscape", "tall", "wide"};
        else if (prev == "--kind") out = {"received", "noticeboard"};
        else if (prev == "--format") out = {"wav", "flac", "pcm"};
        else if (prev == "--expires") out = {"1h", "24h", "7d", "30d", "90d", "1y", "never"};
        else if (prev == "--scope" || prev == "-s")
            out = {"account:read", "credits:read", "passkeys:read", "api-tokens:read", "s3:read", "s3:write",
                   "s3:share", "tmp:read", "tmp:write", "files:read", "files:write", "chat:read", "chat:write", "api-tokens:write", "imagen:read", "imagen:write", "tts", "breeze:read", "breeze:write", "smarthome:read", "smarthome:write", "kathars:admin", "khk:admin", "booru:read", "comics:read", "roleplay:read", "roleplay:write", "vault", "rsys:read", "rsys:publish", "rsys:admin", "rsys:maven", "bakalink:read", "bakalink:manage"};
    } else if (!current.empty() && current[0] == '-') {
        for (const auto& o : kOptions) out.push_back(o.name);
    } else {
        std::vector<std::string> positional;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (before[i].size() > 1 && before[i][0] == '-') {
                if (takes_value(before[i]) && before[i].find('=') == std::string::npos) ++i;
                continue;
            }
            positional.push_back(before[i]);
        }
        if (positional.empty()) {
            for (const auto& g : groups()) out.insert(out.end(), g.names.begin(), g.names.end());
        } else if (positional.size() == 1) {
            if (const Group* g = find_group(positional[0]))
                for (const auto& sub : g->subs) out.insert(out.end(), sub.names.begin(), sub.names.end());
        }
    }
    std::vector<std::string> matching;
    for (auto& c : out)
        if (c.rfind(current, 0) == 0) matching.push_back(std::move(c));
    std::sort(matching.begin(), matching.end());
    matching.erase(std::unique(matching.begin(), matching.end()), matching.end());
    return matching;
}

// The scripts only call back into `<gn> __complete <words before the cursor> -- <current word>`,
// so they never go stale when commands change.
int completion_command(const commands::Context& ctx) {
    const std::string shell = ctx.args.empty() ? "" : ctx.args[0];
    if (shell == "bash") {
        std::cout << R"SH(# gn-cli completion for bash: eval "$(gn-cli completion bash)", or save it to
# /etc/bash_completion.d/gn-cli (Homebrew installs it for you).
_gn_cli() {
    local cur="${COMP_WORDS[COMP_CWORD]}" IFS=$'\n'
    COMPREPLY=($("${COMP_WORDS[0]}" __complete "${COMP_WORDS[@]:1:COMP_CWORD-1}" -- "$cur" 2>/dev/null))
}
complete -o default -o nosort -F _gn_cli gn-cli gn gncli
)SH";
    } else if (shell == "zsh") {
        std::cout << R"SH(#compdef gn-cli gn gncli
# gn-cli completion for zsh: eval "$(gn-cli completion zsh)", or save it as _gn-cli on your $fpath.
_gn_cli() {
    local -a candidates
    candidates=(${(f)"$(${words[1]} __complete ${words[2,CURRENT-1]} -- ${words[CURRENT]} 2>/dev/null)"})
    if (( ${#candidates} )); then compadd -a candidates; else _files; fi
}
if [[ $zsh_eval_context[-1] == loadautofunc ]]; then _gn_cli "$@"; else compdef _gn_cli gn-cli gn gncli; fi
)SH";
    } else if (shell == "fish") {
        std::cout << R"SH(# gn-cli completion for fish: gn-cli completion fish > ~/.config/fish/completions/gn-cli.fish
for cmd in gn-cli gn gncli
    complete -c $cmd -a '(set -l words (commandline -opc); command -q $words[1]; and $words[1] __complete $words[2..-1] -- (commandline -ct) 2>/dev/null)'
end
)SH";
    } else if (shell == "powershell" || shell == "pwsh") {
        std::cout << R"SH(# gn-cli completion for PowerShell: gn-cli completion powershell | Out-String | Invoke-Expression
# (add that line to your $PROFILE to keep it).
Register-ArgumentCompleter -Native -CommandName gn-cli, gn, gncli -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)
    $words = @($commandAst.CommandElements | Select-Object -Skip 1 | Where-Object { $_.Extent.EndOffset -le $cursorPosition } | ForEach-Object { $_.ToString() })
    if ($wordToComplete -and $words.Count -gt 0) { $words = @($words | Select-Object -SkipLast 1) }
    & $commandAst.CommandElements[0].ToString() __complete @words -- $wordToComplete 2>$null | ForEach-Object {
        [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)
    }
}
)SH";
    } else {
        term::error(shell.empty() ? "completion needs a shell: bash, zsh, fish or powershell"
                                  : "no completion for '" + shell + "'; there is bash, zsh, fish and powershell");
        return 2;
    }
    return 0;
}

std::string names_of(const std::vector<Sub>& subs) {
    std::string out;
    for (std::size_t i = 0; i < subs.size(); ++i)
        out += (i == 0 ? "" : i + 1 == subs.size() ? " or " : ", ") + subs[i].names[0];
    return out;
}

int dispatch(Invocation& inv) {
    if (inv.version) {
        std::cout << "gn-cli " << GN_CLI_VERSION << '\n';
        // Only for people: the updater and scripts read this output and expect one line.
        if (term::stdout_is_tty())
            std::cout << "Copyright (c) 2026 Vít Formánek. Licensed, not sold: see `gn-cli license`.\n";
        return 0;
    }
    if (inv.help || inv.positional.empty()) {
        print_help();
        return inv.help ? 0 : 2;
    }
    if (!inv.bad.empty()) return usage_error(inv.bad);

    commands::Context& ctx = inv.ctx;
    const std::vector<std::string>& positional = inv.positional;
    auto args_from = [&](std::size_t i) {
        ctx.args.assign(positional.begin() + static_cast<std::ptrdiff_t>(std::min(i, positional.size())), positional.end());
    };
    http::set_debug(inv.debug);
    if (const char* env = std::getenv("GN_ACCEPT_TERMS"); env && *env && std::string_view(env) != "0") ctx.accept_terms = true;
    int rc = 0;
    try {
        const std::string& cmd = positional[0];
        const Group* group = find_group(cmd);
        auto run = [&](int (*handler)(const commands::Context&)) {
            if ((!ctx.compatibility.empty() || !ctx.variants_file.empty()) && handler != commands::rsys_publish
                && handler != commands::rsys_latest && handler != commands::rsys_download)
                throw std::invalid_argument("Compatibility options are supported only by rsys publish, latest and download");
            return handler(ctx);
        };
        // Local notices, terms, licence, help and completions need no service-terms acceptance.
        static const std::vector<std::string> exempt = {"terms", "license", "completion", "help", "stock"};
        if (group && !named(exempt, group->names[0]) && !commands::require_terms(ctx)) return 1;
        if (!group) {
            rc = usage_error("unknown command '" + cmd + "'");
        } else if (group->subs.empty()) {
            args_from(1);
            rc = run(group->run);
        } else {
            const std::string sub = positional.size() > 1 ? positional[1] : "";
            auto it = std::find_if(group->subs.begin(), group->subs.end(), [&](const Sub& s) { return named(s.names, sub); });
            if (it != group->subs.end()) {
                args_from(2);
                rc = run(it->run);
            } else if (group->run) {
                args_from(1);
                rc = run(group->run);
            } else if (sub.empty()) {
                rc = usage_error(cmd + " needs a subcommand: " + names_of(group->subs));
            } else {
                rc = usage_error("unknown " + cmd + " subcommand '" + sub + "'");
            }
        }
    } catch (const api::Unauthorized& e) {
        if (!ctx.token.empty()) {
            term::error(std::string("The CLI token was refused: ") + e.what());
        } else {
            term::error(std::string("Not logged in anymore: ") + e.what() + ".");
            std::cerr << "  Run " << term::bold("gn-cli auth login") << " again.\n";
        }
        rc = 1;
    } catch (const http::TransportError& e) {
        term::error(std::string("Could not reach the server: ") + e.what());
        rc = 1;
    } catch (const api::ApiError& e) {
        term::error(std::string("The server said no: ") + e.what());
        rc = 1;
    } catch (const std::exception& e) {
        term::error(e.what());
        rc = 1;
    }
    return rc;
}

// Splits a shell line into words: whitespace separates, '…' and "…" quote, \ escapes outside '…'.
bool split_words(const std::string& line, std::vector<std::string>& out, std::string& err) {
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\'))
                word += line[++i];
            else word += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == '\\' && i + 1 < line.size()) {
            word += line[++i];
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (in_word) out.push_back(word);
            word.clear();
            in_word = false;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (quote) {
        err = std::string("unterminated ") + quote;
        return false;
    }
    if (in_word) out.push_back(word);
    return true;
}

// Local paths starting with `typed` ("src/ma" → "src/main.cpp", folders with a trailing /),
// spaces escaped the way split_words reads them back.
std::vector<std::string> complete_path(const std::string& typed) {
    std::string unescaped;
    for (std::size_t i = 0; i < typed.size(); ++i) {
        if (typed[i] == '\\' && i + 1 < typed.size()) ++i;
        unescaped += typed[i];
    }
    auto slash = unescaped.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : unescaped.substr(0, slash + 1);
    std::string prefix = slash == std::string::npos ? unescaped : unescaped.substr(slash + 1);
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir.empty() ? fs::path(".") : fs::path(dir), ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || (name[0] == '.' && (prefix.empty() || prefix[0] != '.'))) continue;
        std::string full = dir + name + (entry.is_directory(ec) ? "/" : "");
        std::string escaped;
        for (char c : full) {
            if (c == ' ' || c == '"' || c == '\'' || c == '\\') escaped += '\\';
            escaped += c;
        }
        out.push_back(escaped);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Tab in the shell: the same completion as the shells get, plus the shell's own words, and
// local files once gn-cli has nothing to offer.
std::vector<std::string> shell_complete(const std::string& before_cursor, std::size_t& word_start) {
    std::vector<std::string> words;
    std::string err;
    split_words(before_cursor, words, err);
    bool fresh_word = before_cursor.empty() || before_cursor.back() == ' ';
    std::string current = fresh_word || words.empty() ? "" : words.back();
    if (!fresh_word && !words.empty()) words.pop_back();
    word_start = fresh_word ? before_cursor.size() : before_cursor.rfind(' ') == std::string::npos ? 0 : before_cursor.rfind(' ') + 1;
    if (!words.empty() && (words[0] == "gn" || words[0] == "gn-cli" || words[0] == "gncli")) words.erase(words.begin());

    std::vector<std::string> out = complete(words, current);
    if (words.empty())
        for (const char* own : {"exit", "quit", "clear"})
            if (std::string(own).rfind(current, 0) == 0) out.push_back(own);
    if (out.empty() && !words.empty() && (current.empty() || current[0] != '-')) out = complete_path(current);
    std::sort(out.begin(), out.end());
    return out;
}

// Interactive mode with no command: read commands one per line until exit, quit or EOF. Global
// options given on the gn-cli command line (--token, --api-url, …) apply to every command.
int shell(const Invocation& base) {
    using namespace term;
    std::cout << bold(cyan("gn-cli")) << ' ' << dim(std::string("v") + GN_CLI_VERSION) << ' ' << dim(g().dash) << ' '
              << "interactive mode. Type " << bold("help") << " for commands, " << bold("clear") << " to clear the screen, "
              << bold("exit") << " to leave.\n";
    if (std::string notice = commands::update_notice(base.ctx); !notice.empty()) info(notice);
    const fs::path history = config::dir() / "history";
    lineedit::history_load(history);
    const std::string prompt = cyan("gn") + bold(">") + ' ';
    int rc = 0;
    for (;;) {
        std::string line;
        if (!lineedit::read_line(prompt, line, shell_complete)) {
            std::cout << '\n';
            break;
        }
        lineedit::history_add(line);
        lineedit::history_save(history);
        std::vector<std::string> words;
        std::string err;
        if (!split_words(line, words, err)) {
            error(err);
            continue;
        }
        if (words.empty()) continue;
        if (words[0] == "gn-cli" || words[0] == "gn" || words[0] == "gncli") words.erase(words.begin());
        if (words.size() == 1 && (words[0] == "exit" || words[0] == "quit" || words[0] == "q")) break;
        if (words.size() == 1 && (words[0] == "clear" || words[0] == "cls")) {
            clear_screen();
            continue;
        }

        Invocation inv = base;
        inv.positional.clear();
        inv.bad.clear();
        inv = parse(words, std::move(inv));
        if (inv.positional.empty() && !inv.help && !inv.version) {
            if (!inv.bad.empty()) usage_error(inv.bad);
            continue;  // just options, e.g. "--json": nothing to run
        }
        rc = dispatch(inv);
        if (rc != 0) std::cerr << dim("(exit " + std::to_string(rc) + ")") << '\n';
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // Shell completion calls back with the words before the cursor, "--", and the current word.
    if (argc > 1 && std::string_view(argv[1]) == "__complete") {
        std::vector<std::string> before;
        std::string current;
        int i = 2;
        for (; i < argc && std::string_view(argv[i]) != "--"; ++i) before.emplace_back(argv[i]);
        if (i + 1 < argc) current = argv[i + 1];
        for (const auto& c : complete(before, current)) std::cout << c << '\n';
        return 0;
    }

    Invocation base;
    if (const char* env = std::getenv("GN_API_URL"); env && *env) base.ctx.api_url = env;
    if (const char* env = std::getenv("GN_TOKEN"); env && *env) base.ctx.token = env;
    if (const char* env = std::getenv("GN_RSYS_URL"); env && *env) base.ctx.rsys_url = env;
    if (const char* env = std::getenv("GN_DEBUG"); env && *env && std::string_view(env) != "0") base.debug = true;

    Invocation inv = parse(std::vector<std::string>(argv + 1, argv + argc), base);
    term::init(inv.topts);

    http::global_init();
    commands::update_cleanup();
    int rc;
    if (inv.positional.empty() && !inv.help && !inv.version && inv.bad.empty() && term::interactive()) rc = shell(inv);
    else rc = dispatch(inv);
    http::global_cleanup();
    return rc;
}

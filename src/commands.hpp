#pragma once

#include <string>
#include <vector>

namespace commands {

struct Context {
    std::string api_url;  // from --api-url or $GN_API_URL; empty when neither is set
    bool json = false;    // info: print raw JSON
    std::string email;    // auth login: --email
    bool password_stdin = false;
    std::string token;                // --token or $GN_TOKEN: a CLI token instead of the saved session
    std::vector<std::string> scopes;  // token create: --scope (repeatable, or comma-separated)
    std::string expires;              // token create: --expires
    // Positional arguments after the (sub)command; mutable so interactive mode can ask for missing ones.
    mutable std::vector<std::string> args;
    bool recursive = false;           // s3: -r, --recursive
    bool yes = false;                 // s3 delete: -y, --yes (no confirmation)
    bool force = false;               // s3 download: -f, --force (overwrite local files)
    bool accept_terms = false;        // --accept-terms (or $GN_ACCEPT_TERMS): accept the ToS and licence unasked
    std::string output;               // s3 download: -o, --output
    std::string part_size;            // s3 upload: --part-size (MiB)
    std::string content_type;         // s3 upload: --content-type

    // rsys
    std::string rsys_url;         // --rsys-url or $GN_RSYS_URL: the release system's server
    std::string name;             // new-project / edit-project: --name
    std::string description;      // new-project / edit-project: --description
    bool set_public = false;      // new-project / edit-project: --public
    bool set_private = false;     // edit-project: --private
    std::string title;            // publish / edit: --title
    std::string changelog;        // publish / edit: --changelog <text>
    std::string changelog_file;   // publish / edit: --changelog-file <path> (- for stdin)
    bool prerelease = false;      // publish / edit: mark as prerelease; latest / download: include prereleases
    bool stable = false;          // edit: no longer a prerelease
    bool draft = false;           // publish: upload but don't publish
    bool append = false;          // publish: add files to an already published release
    std::string primary;          // publish: --primary <file name>
    std::string file;             // download: --file <file name>
    std::string since;            // latest: --since <version>
    std::vector<std::string> compatibility;  // --compat field=value (repeatable, one value per field)
    std::string variants_file;    // publish: --variants-file <manifest.json> (or - for stdin)

    // drop
    std::string ttl;              // --ttl <id>: how long the link lives (30m, 24h, 7d, …)
    std::string max_downloads;    // --max-downloads <n>

    // share
    bool zip = false;             // share get: --zip, everything as one archive

    // update
    bool check = false;           // --check: only say whether there is a newer version

    // chat
    std::string model;            // -m, --model <name|id>
    std::string chat;             // --chat <id>: continue that chat
    bool reasoning = false;       // --reasoning: show the model's reasoning (stderr)

    bool watch = false;           // credits: --watch, redraw every 10 s

    // imagen / tts
    std::string size;             // --size square|portrait|…|WxH
    std::string negative;         // --negative <prompt>
    std::string steps;            // --steps <n>
    std::string guidance;         // --guidance <x>
    bool no_wait = false;         // --no-wait: start the generation and return
    std::string voice;            // --voice <name|id>

    // breeze
    std::string instruction;      // --instruction <text>: the voice or delivery in words
    std::string ref_audio;        // --ref <clip>: a one-off reference clip
    std::string ref_text;         // --ref-text <text>: what the clip says
    std::string seed;             // --seed <n>
    std::string format;           // --format wav|flac|pcm

    // stock
    std::string period;           // --period 1d|5d|1m|6m|ytd|1y|max…
    std::string interval;         // --interval auto|1min|…|1month
    std::string limit;            // --limit <n>

    // booru / comics
    std::string page;             // --page <n> (from 1)
    std::string tag;              // comics: --tag <slug>
    std::string author;           // comics: --author <slug>
    bool no_ai = false;           // booru: --no-ai, leave out AI-generated posts

    std::string persona;          // rp new: --as <persona>
    std::string keyfile;          // vault: --keyfile <path>, instead of a password

    // bakalink
    std::string bakalink_token;         // --bakalink-token or $GN_BAKALINK_TOKEN: the server's BAKALINK_TOKEN
    std::string account;                // --account <id|name> or $GN_BAKALINK_ACCOUNT
    std::string baka_id;                // komens send / marks add: --id <Bakaláři id> (made up when absent)
    std::string content;                // komens send: --content <text>
    std::string content_file;           // komens send: --content-file <path> (- for stdin)
    std::string author_id;              // komens send: --author-id <Bakaláři id> (the name is --author)
    std::string kind;                   // komens send: --kind received|noticeboard
    std::string at;                     // komens send / marks add: --at <ISO-8601 instant> (default now)
    bool prune = false;                 // marks push: --prune, delete the marks the push doesn't carry
    std::string subject;                // marks, marks add, timetable add: --subject
    std::string mark;                   // marks add: --mark <text>
    std::string weight;                 // marks add: --weight <n>
    std::string caption;                // marks add: --caption <text>
    std::string teacher;                // marks add / timetable add: --teacher <Bakaláři id>
    std::string from;                   // timetable: --from <yyyy-mm-dd>
    std::string to;                     // timetable: --to <yyyy-mm-dd>
    std::string date;                   // timetable add: --date <yyyy-mm-dd>
    std::string hour;                   // timetable add: --hour <Bakaláři id>
    std::string room;                   // timetable add: --room <room>
    std::string change;                 // timetable add: --change <type> (its text is --description)
};

int auth_login(const Context& ctx);
int auth_logout(const Context& ctx);
int auth_register(const Context& ctx);
int auth_verify_email(const Context& ctx);
int info(const Context& ctx);
int token_scopes(const Context& ctx);
int token_list(const Context& ctx);
int token_create(const Context& ctx);
int token_revoke(const Context& ctx);

int s3_providers(const Context& ctx);
int s3_buckets(const Context& ctx);
int s3_make_bucket(const Context& ctx);
int s3_list(const Context& ctx);
int s3_mkdir(const Context& ctx);
int s3_upload(const Context& ctx);
int s3_download(const Context& ctx);
int s3_cat(const Context& ctx);
int s3_delete(const Context& ctx);
int s3_rename(const Context& ctx);

int rsys_projects(const Context& ctx);
int rsys_project(const Context& ctx);
int rsys_new_project(const Context& ctx);
int rsys_edit_project(const Context& ctx);
int rsys_delete_project(const Context& ctx);
int rsys_releases(const Context& ctx);
int rsys_show(const Context& ctx);
int rsys_publish(const Context& ctx);
int rsys_edit_release(const Context& ctx);
int rsys_yank(const Context& ctx);
int rsys_unyank(const Context& ctx);
int rsys_delete_release(const Context& ctx);
int rsys_latest(const Context& ctx);
int rsys_download(const Context& ctx);
int rsys_hash(const Context& ctx);

int drop_put(const Context& ctx);
int drop_list(const Context& ctx);
int drop_remove(const Context& ctx);
int drop_limits(const Context& ctx);
int drop_get(const Context& ctx);

int files_put(const Context& ctx);
int files_list(const Context& ctx);
int files_remove(const Context& ctx);
int files_limits(const Context& ctx);
int files_get(const Context& ctx);

int s3_share(const Context& ctx);
int s3_shares(const Context& ctx);
int s3_unshare(const Context& ctx);
int share_get(const Context& ctx);

int chat(const Context& ctx);
int chat_models(const Context& ctx);
int chat_list(const Context& ctx);
int chat_show(const Context& ctx);
int chat_remove(const Context& ctx);

int api_tokens_list(const Context& ctx);
int api_tokens_create(const Context& ctx);
int api_tokens_revoke(const Context& ctx);
int api_tokens_models(const Context& ctx);
int credits(const Context& ctx);
int passkeys_list(const Context& ctx);
int passkeys_remove(const Context& ctx);

int imagen_generate(const Context& ctx);
int imagen_get(const Context& ctx);
int imagen_list(const Context& ctx);
int imagen_models(const Context& ctx);
int tts_say(const Context& ctx);
int tts_voices(const Context& ctx);
int tts_add(const Context& ctx);
int tts_remove(const Context& ctx);
int breeze_say(const Context& ctx);
int breeze_voices(const Context& ctx);
int breeze_add(const Context& ctx);
int breeze_remove(const Context& ctx);
int breeze_status(const Context& ctx);
int breeze_load(const Context& ctx);
int breeze_unload(const Context& ctx);

int admin_users(const Context& ctx);
int admin_user(const Context& ctx);
int admin_create_user(const Context& ctx);
int admin_perms(const Context& ctx);
int admin_password(const Context& ctx);
int admin_credits(const Context& ctx);
int admin_usage(const Context& ctx);
int admin_providers(const Context& ctx);
int admin_models(const Context& ctx);
int admin_oauth_clients(const Context& ctx);

int stock_price(const Context& ctx);
int stock_tickers(const Context& ctx);
int stock_score(const Context& ctx);
int stock_news(const Context& ctx);
int stock_macro(const Context& ctx);
int stock_geo(const Context& ctx);

int home_nodes(const Context& ctx);
int home_rooms(const Context& ctx);
int home_room(const Context& ctx);
int home_poll(const Context& ctx);
int home_config(const Context& ctx);
int home_reset(const Context& ctx);
int home_fun(const Context& ctx);

int kathars_staff(const Context& ctx);
int kathars_punishments(const Context& ctx);
int kathars_add_member(const Context& ctx);
int kathars_edit_member(const Context& ctx);
int kathars_remove_member(const Context& ctx);
int kathars_add_group(const Context& ctx);
int kathars_edit_group(const Context& ctx);
int kathars_remove_group(const Context& ctx);

int khk_list(const Context& ctx);
int khk_show(const Context& ctx);
int khk_pdf(const Context& ctx);
int khk_accept(const Context& ctx);
int khk_reject(const Context& ctx);
int khk_pending(const Context& ctx);

int booru_search(const Context& ctx);
int booru_tags(const Context& ctx);
int booru_show(const Context& ctx);
int booru_get(const Context& ctx);
int booru_download(const Context& ctx);
int comics_search(const Context& ctx);
int comics_show(const Context& ctx);
int comics_get(const Context& ctx);
int comics_tags(const Context& ctx);
int comics_authors(const Context& ctx);

int rp_characters(const Context& ctx);
int rp_personas(const Context& ctx);
int rp_chats(const Context& ctx);
int rp_show(const Context& ctx);
int rp_continue(const Context& ctx);
int rp_new(const Context& ctx);
int rp_remove(const Context& ctx);

int vault_use(const Context& ctx);
int vault_list(const Context& ctx);
int vault_get(const Context& ctx);
int vault_hide(const Context& ctx);
int vault_link(const Context& ctx);

int bakalink_accounts(const Context& ctx);
int bakalink_komens(const Context& ctx);
int bakalink_marks(const Context& ctx);
int bakalink_timetable(const Context& ctx);

int update(const Context& ctx);
int license(const Context& ctx);
int license_third_party(const Context& ctx);
int terms(const Context& ctx);
int terms_accept(const Context& ctx);
int terms_revoke(const Context& ctx);
int terms_status(const Context& ctx);
// True when the Terms of Service and the licence (this version of them) are accepted: already
// saved, passed with --accept-terms / $GN_ACCEPT_TERMS, or agreed to at the prompt just now.
// Otherwise says why not on stderr and returns false.
bool require_terms(const Context& ctx);
// Removes what an update left behind (Windows: the renamed old .exe). Call once at start-up.
void update_cleanup();
// "gn-cli 1.2.0 is out (you have 1.1.0); run gn update." when a newer release exists, else "".
// Asks the release system at most once a day (3 s timeout, failures ignored); GN_NO_UPDATE_CHECK=1 turns it off.
std::string update_notice(const Context& ctx);

}  // namespace commands

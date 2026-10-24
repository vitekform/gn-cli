# gn-cli

Command-line client for the nextgen services at https://ganamaga.me.

Source, issues and contributions: https://github.com/vitekform/gn-cli.
This repository builds on its own; no backend or frontend checkout is needed.
`gn` and `gncli` are aliases of `gn-cli` (installed as symlinks next to it).

```
gn-cli auth login      # log in (asks for email, password, and a device code if needed)
gn-cli auth logout     # forget the saved session
gn-cli info            # everything about your account (alias: gn-cli me)
gn-cli me --json       # the same, as raw JSON

gn-cli token scopes                                  # what a CLI token can be granted
gn-cli token create ci --scope credits:read --expires 30d
gn-cli token list
gn-cli token revoke 3
```

Global options, accepted anywhere on the line:

| Option | |
|---|---|
| `--no-unicode` | ASCII only: `[ok]`, `+-`, `\|` instead of `✔`, `╭─`, `│` |
| `--no-color` | no colours; `NO_COLOR=1` does the same, and colour is off anyway when stdout is not a terminal |
| `--token <token>` | use a CLI token instead of the saved login; also `$GN_TOKEN` |
| `--api-url <url>` | another server, e.g. `http://localhost:8080`; also `$GN_API_URL` |
| `--noninteractive` | never ask anything (see below); also `GN_NONINTERACTIVE=1` |
| `--debug` | log every request and response (URL, status, IP, headers, error bodies) to stderr, credentials redacted; also `GN_DEBUG=1`. A 403 from Cloudflare shows its `cf-ray` and `cf-mitigated` even without it |

### Interactive mode

On by default. gn-cli then asks for what it is missing instead of failing: the
email, password and device code of `auth login`, missing arguments
(`gn-cli s3 upload` asks for provider, bucket, …), and a yes/no before deleting
a folder, release or project. Run with no command at all, it opens a shell:

```
$ gn
gn> s3 ls minio
gn> rsys latest gn-cli --since 1.0.0
gn> clear
gn> exit
```

`clear` (or `cls`) clears the screen, `exit` (`quit`, `q`, Ctrl-D) leaves.

The shell edits lines like bash: ←/→, Home/End (Ctrl-A/E), ↑/↓ through the
history (kept in the config folder), Ctrl-W/U/K to delete, Ctrl-L to clear, and
Tab completes commands, subcommands, options and local file names (twice lists
them). On Windows the console's own editing applies instead.

Options given to `gn` itself (`--token`, `--api-url`, `--no-color`, …) apply to
every command in the shell; quote arguments with spaces like in a shell.

It is off, and gn-cli never waits for input, when any of these holds:
`--noninteractive` (or `--non-interactive`), `GN_NONINTERACTIVE=1`,
`DEBIAN_FRONTEND=noninteractive`, `CI` set (every CI system sets it), or stdin
is not a terminal. Missing input is then an error that says what to pass
instead (`--email`, `--password-stdin`, `--yes`), and no command prints the help.

Account odds and ends:

```
gn credits                      # balance and the last 30 requests; --watch redraws every 10 s
gn passkeys ls                  # your passkeys; gn passkeys rm <id> removes one (login session only)
gn api-tokens ls                # OpenAI-compatible API keys (openAIAPI permission)
gn api-tokens create laptop     # prints the key once, with OPENAI_BASE_URL/OPENAI_API_KEY lines
key=$(gn api-tokens create ci)  # piped: just the key
gn api-tokens revoke 3
gn api-tokens models            # what the gateway (https://oai.ganamaga.me/v1) serves
```

New here? `gn auth register` creates an account (email, password twice), signs
in, and offers to verify the email address right away: it mails a fresh code to
type in. `gn auth verify-email` does that part on its own later.

`auth login` takes `-e/--email` and `--password-stdin` for scripts:

```
pass show ganamaga | gn-cli auth login -e me@example.com --password-stdin
```

## S3 storage

The same buckets as the web S3 browser. These need the `s3` permission; with a
login session `ls` and `download` also work with just `gallery`, inside the
gallery folders. A CLI token works too: `ls`, `cat` and `download` need
`s3:read`, everything else `s3:write`. `delete` and `rename` list what they
touch first, so on folders they need `s3:read` as well — grant both.

```
gn-cli s3 ls                                     # providers
gn-cli s3 ls minio                               # buckets on a provider
gn-cli s3 ls minio media photos/                 # one folder level
gn-cli s3 ls minio media photos -r               # everything below it (--json works too)

gn-cli s3 upload minio media photos/ cat.jpg     # → photos/cat.jpg
gn-cli s3 upload minio media photos/kitty.jpg cat.jpg
gn-cli s3 upload minio media backup/ ./site -r   # a folder's contents, under backup/

gn-cli s3 download minio media photos/cat.jpg              # → ./cat.jpg
gn-cli s3 download minio media photos/cat.jpg ~/Pictures/  # or -o <path>; - for stdout
gn-cli s3 download minio media photos/ -o pics             # a whole folder
gn-cli s3 cat minio media notes/todo.txt

gn-cli s3 rename minio media photos/cat.jpg photos/old/    # alias mv; keeps the name
gn-cli s3 rename minio media photos/ pictures/             # a folder, object by object
gn-cli s3 delete minio media photos/cat.jpg                # alias rm
gn-cli s3 delete minio media photos/old/ --yes             # a folder; asks without --yes
gn-cli s3 mkdir minio media photos/2026
gn-cli s3 mb minio new-bucket
```

Share links, like the web S3 browser's "Share" (a CLI token needs `s3:share`):

```
gn s3 share minio media photos/cat.jpg              # → https://ganamaga.me/share/<token>, for 24 h
gn s3 share minio media photos/ docs/a.pdf --expires 7d   # 5m … 30d; a folder ends in / (or -r)
gn s3 shares                                        # your live links (--json)
gn s3 unshare 4                                     # or the link
gn share get https://ganamaga.me/share/<token>      # no login: one file, or every file
gn share get <link> -o pics/                        #   into a folder, keeping the structure
gn share get <link> --zip                           # all of it as one zip
```

The secure-storage vault (files encrypted into PNG images under
`secure_storage/` in a bucket; the server does the cryptography):

```
gn vault use minio media                    # where the vault is (remembered)
gn vault ls                                 # what the password opens (or --keyfile key.bin)
gn vault get secret.txt -o out/             # extract one
gn vault hide secret.txt c1.png c2.jpg      # encrypt it into those covers; checks they hold it
gn vault link secret.txt                    # public share link (the server keeps the key with it; asks)
```

The password is asked for (`--password-stdin` for scripts). A cover holds
6 bits per pixel, about 732 KiB for a 1000x1000 image. Keep one kind of key per
bucket: the decoder takes the key type from the first image it reads, so a
bucket with both password- and key-file-locked files may open with neither.
CLI tokens need `vault`, and `s3:write` to hide (the upload goes through
`/api/v2/multipart`).

A path ending in `/` (or `-r`) means a folder for `download`, `delete` and
`rename`; a folder rename is one call, done on the server. Everything talks to
`/api/v3/storage`. Uploads go up in parts the server sizes (8 MiB or more), each
streamed straight into storage, retrying a failed part twice, with a progress bar
when stderr is a terminal; `--part-size` is accepted but ignored. The content type comes from the file extension
unless you pass `--content-type`. Downloads land in `<name>.gn-part` first and
refuse to overwrite an existing file without `--force`.

## AI chat

The web app's chat, in the terminal (the `aiChatBasic` permission; paid models
spend credits). Replies stream as the model writes them.

```
gn chat                                    # a chat session: /model, /new, /reasoning, /exit
gn chat "how do I undo a git rebase?"      # ask once, the answer on stdout
git diff | gn chat "review this"           # stdin is attached to the question
gn chat -m gpt-5-mini "…"                  # pick a model by name, a piece of it, or id
gn chat --chat 42 "and in Python?"         # carry on in an existing chat
gn chat ls / show 42 / rm 42               # your chats (--json everywhere)
gn chat models                             # models, prices per million tokens, context sizes
```

The model you last used is the default. In a session, Tab completes the slash
commands, a line ending in `\` continues on the next, and Ctrl-C stops a reply
without leaving. Tools the server runs (web search and the like) show as a
status line; when the model asks you something (`ask_user`), the questions come
up as a numbered list to answer right there. Thinking and tool lines go to
stderr and only on a terminal, so `gn chat "…" > answer.md` holds just the
answer; `--reasoning` prints the model's reasoning too. To ask about a word
that is also a subcommand (`ls`, `show`, `models`), use `gn chat ask …`.

## Admin

The admin panel, for accounts with `aiAdmin` (or level 4). Login session only:
no CLI token scope reaches `/api/v2/admin`. Every change shows what it does
and asks first; `--yes` skips that. A user is an id, an email, or a piece of one
that fits only one account.

```
gn admin users [filter]                    # everyone: credits and access
gn admin user bob                          # one account with its credits and usage
gn admin create-user khk@x.cz khkAdmin     # an account with only these permissions (asks for a password)
gn admin perms bob                         # show; then: set s3,aiChatBasic | role
gn admin password bob --password-stdin     # root only; signs their sessions out
gn admin credits bob add 5                 # or sub 1, set 10; no action shows the balance
gn admin usage | providers | models | oauth-clients
```

`perms set` switches the account to custom permissions, where it can do exactly
what the list says and nothing its role would grant; `perms … role` switches
back.

## Smart home

The smart home's devices and rooms (the `smartHome` permission; CLI tokens need
`smarthome:read`, and `smarthome:write` for anything that sends a command):

```
gn home                                   # devices: type, room, temperature, humidity, last seen
gn home rooms                             # each room's temperature against its target
gn home room living_room 21.5 0.5         # target and threshold, °C
gn home poll                              # ask every device to report, then list them
gn home config a1b2 room=kitchen task=HAC:kitchen:5,6 state=SLAVE
gn home reset a1b2                        # asks first
gn home fun a1b2 on
```

A device is its id or the start of it. `config` keeps whatever you don't
change, since the backend replaces room, tasks and state together.

## Booru and comics

The booru mirror and the crawled comics (the `gelbooru` permission; CLI tokens
need `booru:read` / `comics:read`, and `s3:read` for booru originals):

```
gn booru cat_girl solo                 # posts with all those tags (--page, --limit, --no-ai)
gn booru tags cat                      # tag suggestions with counts
gn booru show 1234                     # size, rating, creator, source, rule34 link, tags
gn booru get 1234 1235 -o pics/        # originals, as <id>.<ext>
gn booru dl cat_girl --limit 200       # a whole search into ./cat_girl/ (skips what's there)

gn comics space                        # search titles (--tag, --author, --page, --limit)
gn comics show c-77                    # authors, tags, characters, pages mirrored so far
gn comics get c-77                     # every mirrored page into ./<slug>/001.jpg… (sha256-checked, resumable)
gn comics tags sci / gn comics authors zed
```

Uploading, deleting and the live rule34 search stay in the web app.

## KHK applications

Membership applications to KHK Pardubice, for `khkAdmin` accounts (CLI token:
`khk:admin`):

```
gn khk ls                 # all; or ls pending / approved / rejected
gn khk show 12            # everything the applicant filled in, with a map link
gn khk pdf 12             # application-12.pdf (-o to name it)
gn khk accept 12          # publishes them on the KHK website and emails them; asks first
gn khk reject 12          # status only (asks); khk pending 12 puts it back
```

Accepting an approved application again only republishes it, without a second
email. Rejecting an approved one leaves the member on the website; remove the
post there.

## Kathars

The Kathars Minecraft network's public staff list and punishments (no login),
and editing the staff list with the `katharsAdmin` permission (CLI token:
`kathars:admin`):

```
gn kathars staff                                   # by group, with Minecraft names
gn kathars punishments griefer                     # bans, mutes…; active first
gn kathars add-member name=Matetrix group=Helpers role="Trial Helper" mc=mrkukloff
gn kathars edit-member matetrix role=Helper order=2   # by id or name; keeps what you don't change
gn kathars rm-member matetrix
gn kathars add-group name=Moderators color=#00aa00 order=2
gn kathars edit-group moderators name=Mods  /  gn kathars rm-group mods   # only when empty
```

## BakaLink

BakaLink keeps what Bakaláři accounts receive: Komens messages with their
attachments, marks, and the actual and permanent timetables. These commands are
how the Bakaláři integration feeds it, and how to look at what is stored. No
login is needed, but the server's `BAKALINK_TOKEN` is (as `--bakalink-token` or
`$GN_BAKALINK_TOKEN`); with no token configured on the server it refuses
everything. A CLI token also needs `bakalink:read` to look and `bakalink:manage`
to change anything.

Everything but `accounts` works on one account: `--account <id|name>` or
`$GN_BAKALINK_ACCOUNT`. `--json` prints the server's answer; times are instants,
shown in UTC.

```
export GN_BAKALINK_TOKEN=… GN_BAKALINK_ACCOUNT=honza
gn bakalink accounts
gn bakalink account add name=honza school=https://skola.cz/bakaweb user=UXYZ1 full-name="Jan Novák" class=3.B
gn bakalink account edit honza class=4.B  /  gn bakalink account rm honza   # with everything stored for it

gn bakalink komens --since 2026-10-01T00:00:00Z --limit 20
gn bakalink komens show 12  /  gn bakalink komens get 12 [attachment id] -o pozvanka.pdf  /  gn bakalink komens rm 12
gn bakalink komens push --file komens.json            # or --file - for stdin
gn bakalink komens send --title "Třídní schůzky" --content-file zprava.txt \
    --id MSG123 --author-id UXYZ1 --author "Mgr. Novák" --at 2026-10-02T08:00:00Z pozvanka.pdf

gn bakalink marks --subject M
gn bakalink marks push --file marks.json --prune
gn bakalink marks add --subject M --mark 2- --weight 3 --caption "Ústní zkoušení" --id MARK9  /  gn bakalink marks rm 40

gn bakalink timetable --from 2026-10-05 --to 2026-10-09   # this week when neither is given
gn bakalink timetable push --file week.json
gn bakalink timetable add --date 2026-10-07 --hour 3 --subject M --room 14 --change Added  /  gn bakalink timetable rm 77
gn bakalink timetable permanent  /  gn bakalink timetable permanent push --file permanent.json
```

Things are told apart by their Bakaláři id (`bakaId`), within the account: a
push creates what is new and updates what is already stored, so the same file can
be pushed again and again. Teachers and subjects are described inline as
`{"bakaId", "name", "abbrev"}` wherever they appear; they are created on first
sight, and later mentions need only the `bakaId`.

**Komens** — `komens.json` is `{"messages": […]}`, a bare array, or one message:

```json
[{"bakaId": "MSG123", "kind": "RECEIVED", "title": "Třídní schůzky", "text": "<p>Zveme vás…</p>",
  "sender": {"bakaId": "UXYZ1", "name": "Mgr. Novák", "abbrev": "No"}, "sentAt": "2026-10-02T08:00:00+02:00", "read": false,
  "attachments": [{"bakaId": "ATT1", "path": "pozvanka.pdf"}]}]
```

`kind` is `RECEIVED` (the default) or `NOTICEBOARD`; `senderName` names a sender
that is not a teacher. An attachment gives a local `path`, which gn-cli reads, or
base64 `data` with a `filename`; one the message already has (same `bakaId`,
the filename when there is none) is not uploaded again. `komens send` is the same
for a single message from options; without `--id` every send is a new message.

**Marks** — `marks.json` is `{"subjects": […]}` or a bare array of subjects:

```json
{"subjects": [{"bakaId": "M", "name": "Matematika", "abbrev": "M", "averageText": "1,85", "marks": [
  {"bakaId": "MARK1", "markText": "2-", "weight": 4, "caption": "Písemka", "theme": "Rovnice", "typeCode": "P",
   "markDate": "2026-09-12T00:00:00+02:00", "isPoints": false, "isNew": true, "teacher": {"bakaId": "UXYZ1"}}]}]}
```

`--prune` also deletes the account's marks the file does not carry, which is what
a push of everything Bakaláři shows wants (teachers take marks back); leave it
out when pushing only what is new. A marks push is all or nothing.

**Timetable** — the actual one replaces every stored day between `from` and `to`
(the first and last pushed day when left out), so push whole weeks:

```json
{"from": "2026-10-05", "to": "2026-10-09",
 "hours": [{"bakaId": "3", "caption": "3", "beginTime": "9:55", "endTime": "10:40"}],
 "days": [{"date": "2026-10-05", "dayType": "WorkDay", "lessons": [
   {"hour": "3", "subject": {"bakaId": "M"}, "teacher": {"bakaId": "UXYZ1"}, "room": "12", "group": "3.B",
    "theme": "Rovnice", "changeType": "Canceled", "changeDescription": "Odpadá"}]},
  {"date": "2026-10-06", "dayType": "Holiday", "description": "Státní svátek"}]}
```

A lesson's `hour` is the `bakaId` of one of the `hours`, which are kept per
account: once pushed they need not be repeated. The permanent timetable is
replaced whole by `{"hours": […], "lessons": [{"dayOfWeek": 1, "hour": "3", "cycle": "Lichý", "subject": …, "teacher": …, "room": "12", "group": "3.B"}]}`,
with `dayOfWeek` 1 (Monday) to 7 and `cycle` left out for lessons on every week.

## Markets (sunset)

The Stock system has been sunset. Every `gn stock` command (and the `stocks`
alias), including former price, score, news, macro, geo and ticker commands,
now prints:

```text
Stock system has been sunset
https://ganamaga.me/blog/stock/sunset
```

These commands exit with status 1 without authentication, service-terms prompts
or network requests. With `--json`, they return an object with `error` set to
`stock_sunset`, plus `message` and `url`. Read
[why we decided to sunset the system](https://ganamaga.me/blog/stock/sunset).

## Roleplay

Roleplay chats in the terminal (`aiRoleplayBasic`; paid models spend credits).
Characters, personas and background cards are made in the web app; here you
pick them and talk.

```
gn rp characters / gn rp personas         # what you can cast
gn rp new finn,mira --as aster            # start a chat (-m <model>, --name) and talk
gn rp 12                                  # carry on in chat 12: /regen, /history, /clear, /exit
gn rp chats / gn rp show 12 / gn rp rm 12
```

Each reply shows a "writing…" line while it streams and is printed speaker by
speaker once complete. CLI tokens need `roleplay:read` and `roleplay:write`.

## Images and speech

Image generation (the `aiImagen` permission) and text to speech (`aiTTS`), as in
the web app:

```
gn imagen "a cat in a spacesuit"                    # waits, then saves imagen-<id>.png here
gn imagen "…" -m animagine --size portrait -o cat.png   # gn imagen models lists the models
gn imagen "…" --negative "blurry" --steps 30 --guidance 7
gn imagen "…" --no-wait                             # start it; gn imagen get <id> fetches it later
gn imagen ls                                        # your generations and how they went

gn tts "Hello there"                                # speech.wav, in your only voice
gn tts "Ahoj" --voice Narrator -o ahoj.wav          # by name or id; -o - for stdout
cat notes.txt | gn tts -o notes.wav                 # text from stdin (up to 2000 bytes a go)
gn tts voices / add <name> <sample.wav> / rm <id>
```

Ctrl-C while an image is generating only stops the waiting; the generation
carries on and `gn imagen get` picks it up. A CLI token needs `imagen:read` and
`imagen:write`, plus `s3:read` to download the finished image; speech needs `tts`.

### Breeze TTS

`gn breeze` speaks through Breeze TTS 2 on the XPU server at home (also
`aiTTS`). The voice depends on what you give it: nothing is the stock voice,
`--instruction` designs one from words, a saved `--voice` (or a one-off `--ref`
clip with its transcript) clones one, and a voice plus an instruction directs
the clone:

```
gn breeze "Hello there."                                    # speech.wav, stock voice
gn breeze "Arr, the treasure is mine." --instruction "gravelly old pirate, amused" --format flac
gn breeze add me me.wav "Exact words spoken in me.wav."     # save a voice (5-20 s clip)
gn breeze "Quietly now." --voice me --instruction "whispering, nervous"
gn breeze "New sentence." --ref clip.wav --ref-text "What clip.wav says."
gn breeze "Streaming test." -o - | ffplay -nodisp -autoexit -    # plays while it's generated
gn breeze voices / rm <name>
gn breeze status / load / unload                            # unload frees the XPU (admins)
```

The server loads the model on the first request after ten idle minutes, which
takes about a minute, and speaks one request at a time, so a request may wait.
`--seed` repeats a result (the seed used is printed), `--guidance` is the CFG
scale (4 with an instruction, 1 without). A CLI token needs `breeze:read` for
`voices` and `status`, `breeze:write` for the rest.

## Temporary files

Like the web app's "drop": upload a file, get a public link that expires after a
time (and, if you like, a number of downloads). Any logged-in account can; the
TTLs on offer depend on your role level (`gn drop limits` lists them).

```
gn drop report.pdf                         # → https://ganamaga.me/f/<token>
gn drop build.zip --ttl 1h --max-downloads 1
url=$(gn drop notes.txt)                   # piped: prints only the link
gn drop ls                                 # your files, their links and downloads (--json)
gn drop rm 12                              # or the link; it stops working at once
gn drop get https://ganamaga.me/f/<token>  # no login needed; -o <path>, - for stdout
```

Without `--ttl` the link lives for the longest TTL up to 24 hours your level
allows. A download through `drop get` counts against `--max-downloads` like any
other.

## Persistent files

The web app's "Files": upload a file and get a public link that keeps working
until you delete it, with no TTL or download limit. Uploading needs role level 2
or higher, with the same size limits as temporary files (`gn files limits`).

```
gn files report.pdf                         # → https://ganamaga.me/p/<token>
url=$(gn files notes.txt)                   # piped: prints only the link
gn files ls                                 # your files, their links and downloads (--json)
gn files rm 12                              # or the link; it stops working at once
gn files get https://ganamaga.me/p/<token>  # no login needed; -o <path>, - for stdout
```

A CLI token needs `files:read` and `files:write`.

## Release system

Projects and their released versions on https://rsys.ganamaga.me. Reading a
public project (`latest`, `download`) needs no login at all; everything else
needs the `release_system` permission (level 3, or a custom grant).

```
gn-cli rsys latest gn-cli                        # newest version, files, changelog
gn-cli rsys latest gn-cli --since 0.1.0          # the changelogs of everything newer than yours
gn-cli rsys latest gn-cli --json                 # the raw latest.json
gn-cli rsys download gn-cli                      # its primary file, into the current folder
gn-cli rsys download gn-cli 1.0.2 --file gn-cli-windows-x64.exe -o ~/bin/

gn-cli rsys publish gn-cli 0.3.0 build/gn-cli-linux-x64 build/gn-cli-windows-x64.exe \
    --title "Faster uploads" --changelog-file CHANGES.md --primary gn-cli-linux-x64
gn-cli rsys publish app 2.0.0-rc.1 dist/app.zip --prerelease
gn-cli rsys publish app 2.0.0 out/app#app-linux-x64 # path#name renames it in the release
gn-cli rsys publish app 2.0.0 extra.zip --append   # add a file to a published release

gn-cli rsys projects                             # every project (--json works everywhere)
gn-cli rsys project gn-cli                       # one project and its releases
gn-cli rsys show gn-cli 1.0.2                    # one release with its files (default: latest)
gn-cli rsys edit gn-cli 1.0.2 --changelog-file CHANGES.md --stable
gn-cli rsys yank gn-cli 1.0.2                    # hide a broken release; unyank brings it back
gn-cli rsys delete gn-cli 1.0.2 --yes            # gone, files and all

gn-cli rsys new-project gn-cli --name "gn-cli" --description "The nextgen CLI" --public
gn-cli rsys edit-project gn-cli --private
gn-cli rsys delete-project gn-cli
gn-cli rsys hash some-file                       # sha256, like sha256sum
```

Templates add generic compatibility selectors to `publish`, `latest`, and `download`
(starting with gn-cli 1.11.0):

```sh
# One explicitly supported combination, using the template's stable field/choice keys.
gn-cli rsys publish my-plugin 2.4.0 build/plugin.jar \
  --compat platform=paper --compat minecraft_version=1.21.1

gn-cli rsys latest my-plugin --compat platform=paper --compat minecraft_version=1.21.1 --json
gn-cli rsys download my-plugin --compat platform=paper --compat minecraft_version=1.21.1 -o plugin.jar
# Include prereleases, or request compatible changelogs after a particular release.
gn-cli rsys latest my-plugin --compat platform=paper --compat minecraft_version=1.21.1 --prerelease --since 2.3.0

# Different builds or shared files: declare each exact combination in a JSON manifest.
gn-cli rsys publish my-plugin 2.4.0 build/paper.jar build/spigot.jar \
  --variants-file examples/rsys-variants.json
```

`--compat` is repeatable with **one value per field**. All supplied/existing ready
files belong to that one build; its default is `--primary`, the existing primary,
or the first supplied file. Use `--variants-file` for several combinations or
for different files/defaults. Each entry names a `key`, a single `selection`
object, `files` by their release names (after any `path#name` rename), and
`defaultFile`. The same file can serve several entries and is uploaded once.
A manifest replaces the release's entire compatibility set; it does not append
combinations. Review the complete manifest before using it on a published release.
See [the example manifest](examples/rsys-variants.json) and
[the CI workflow example](examples/rsys-publish.workflow.yml).

The CLI checks required fields, unknown/deprecated choices, duplicate combinations,
file references, and defaults **before hashing or uploading**. Drafts may be incomplete
with `--draft`. To complete an existing draft, pass its version and compatibility
flags/manifest again, with no local files needed if its files are ready. Resuming a
draft uses its pinned template revision, even after the project changes templates.
Compatibility edits made during an upload cause a conflict; uploaded files remain
available for review and retry. Existing metadata stays as it was until the atomic
replacement succeeds. `--append` is required when updating a published/yanked
release. An explicit published default cannot be force-replaced; publish a new
release, assign a different default, or yank and repair the old release first.

Compatible downloads use the selected build's default, and `--file` can choose
only another file attached to that build. They verify SHA-256 and support public
or private projects. `--compat` on `download` resolves latest and cannot be combined
with an explicit release version. No match never falls back to unfiltered latest.
`latest --compat ... --since ...` searches up to 200 compatible releases and reports
an error if the anchor is absent. Add `--prerelease` to opt into prereleases.
Omitted optional fields mean an exact omission, not a wildcard. `--variants-file -`
reads stdin; the changelog and manifest cannot both use stdin.

Flag-free commands retain the existing general-project behavior and work with older
servers. Projects requiring compatibility need flags/a manifest for new releases;
old complete variants can be reused on a resumed release. Template administration,
project assignment, migration previews, and deliberate backfills are available in
the Releases GUI. The `gn-cli` publisher now declares operating system and architecture
variants after all parallel uploads finish; the Android publisher builds each ABI
separately and declares one `architecture` variant per APK, each with the shared
update metadata. Legacy latest
URLs and file defaults remain available to existing downloaders and the updater.

Configured project selectors can also be used directly:

```sh
gn-cli rsys latest gn-cli --compat os=linux --compat architecture=x64
gn-cli rsys latest gnlib --compat loader=neoforge --compat minecraft_version=26.3
gn-cli rsys latest katharsis --prerelease --compat platform=velocity
gn-cli rsys latest nextgen --compat architecture=arm64
```

Minecraft mods use exact `loader` and `minecraft_version` pairs. Katharsis uses
`platform=velocity|paper|neoforge` and, for companions, a `minecraft_version`
line: Paper `1.21.x`, NeoForge `1.21.1-1.21.10` or `1.21.11`, and `26.x` for
both. Velocity omits the version field. Android choices are `arm64`, `armv7`,
`x64`, and `x86`; older universal APKs are mapped to each supported architecture,
and every current build has its own APK for each of them.

`publish` creates the release as a draft, uploads the files (8 MiB parts,
retried like S3 uploads), and publishes it last, so nobody ever sees half a
release. Each file's SHA-256 goes along, and `download` checks it. Run it again
after a failure and it carries on: files already there with the same checksum
are skipped, an unfinished upload is replaced. `--draft` stops before
publishing; `gn-cli rsys publish <project> <version>` with no files publishes
it later.

A version is letters, digits, dots, dashes and underscores (`v1.2.0` is stored
as `1.2.0`; no `+build` suffixes). A prerelease is never "latest" unless you
ask with `--prerelease`.

What the public sees, for a public project:

| URL | |
|---|---|
| `https://rsys.ganamaga.me/<project>/public/latest.json` | newest release: version, changelog, files with sha256, and the changelog history (`?since=<version>`, `?prerelease=true`) |
| `https://rsys.ganamaga.me/<project>/public/latest` | 302 to the primary file on the CDN |
| `https://rsys.ganamaga.me/<project>/public/latest/<file>` | 302 to another file of the newest release |
| `https://rsys.ganamaga.me/<project>/public/releases.json` | every published release |
| `https://rsys.ganamaga.me/<project>/public/v/<version>.json` | one release |
| `https://rsys.ganamaga.me/<project>/public/v/<version>/<file>` | 302 to one of its files |

Every project is also a Maven repository, for libraries other builds compile
against. Publish with Gradle's `maven-publish` and a token holding `rsys:maven`
(`gn-cli token create ci-maven -s rsys:maven --expires 1y`):

```groovy
publishing.repositories.maven {
    name = 'rsys'
    url = 'https://rsys.ganamaga.me/api/v2/rsys/maven/<project>'
    credentials(HttpHeaderCredentials) { name = 'X-CLI-Token'; value = System.getenv('RSYS_MAVEN_TOKEN') }
    authentication { header(HttpHeaderAuthentication) }
}
```

and consume a public project's with no credentials at all:
`maven { url = 'https://rsys.ganamaga.me/<project>/public/maven' }`.
A file published again replaces the old one; files are at most 90 MiB.

The release system lives on its own host; `--rsys-url` (or `$GN_RSYS_URL`)
points gn-cli elsewhere, and `--api-url` counts too when given.

## How the session works

The backend's session token is an HMAC over user ID, IP address, User-Agent and
the account's master token, so it dies when your IP changes. gn-cli stores the
master token too, and when a request comes back 401 it signs in again with
`/auth/login-with-master-token` and retries — moving between networks needs no
new login. It always sends the User-Agent it logged in with.

A first login from a new device, for accounts with 2FA, asks for the code the
server emails you (`/auth/verify-device`). That step needs a terminal.

The session lives in `credentials.json` (mode 0600) in:

- `$GN_CLI_CONFIG_DIR`, if set
- Windows: `%APPDATA%\gn-cli`
- elsewhere: `$XDG_CONFIG_HOME/gn-cli`, or `~/.config/gn-cli`

The backend has no logout endpoint, so `auth logout` deletes that file.

## CLI tokens

A CLI token is a fine-grained credential for scripts and CI. Unlike the login
session it isn't tied to your IP address, but it only works for the calls its
scopes list, and the server refuses everything else with `403 insufficient_scope`:

| Scope | Unlocks |
|---|---|
| `account:read` | `GET /api/v2/auth/me` |
| `credits:read` | `GET /api/v2/auth/me/credits` |
| `passkeys:read` | `GET /api/v2/auth/passkey/list` |
| `api-tokens:read` | `GET /api/v2/api-tokens` (needs the openAIAPI permission too) |
| `s3:read` | `GET /api/v3/storage/providers`, `GET …/{provider}/buckets`, `GET …/{provider}/{bucket}/objects` and `…/object`, plus the old `/api/v2/s3` reads (needs the s3 permission too) |
| `s3:write` | `/api/v3/storage`: `POST …/buckets`, `POST`/`DELETE …/folder`, `DELETE …/object`, `POST …/move`, `POST …/uploads`, `POST …/uploads/{token}/parts/{n}` and `…/complete`, `DELETE …/uploads/{token}`, plus the old `/api/v2/s3` writes and `/api/v2/multipart` (needs the s3 permission too) |
| `s3:share` | `GET`, `POST /api/v2/s3/shares`, `DELETE …/shares/{id}`: public share links (needs the s3 permission too) |
| `tmp:read` | `GET /api/v2/tmp-files`, `GET /api/v2/tmp-files/limits` |
| `tmp:write` | `POST /api/v2/tmp-files`, `…/{id}/parts/{n}`, `…/{id}/complete`, `DELETE …/{id}`: upload and delete temporary files |
| `files:read` | `GET /api/v2/persistent-files`, `GET /api/v2/persistent-files/limits` |
| `files:write` | `POST /api/v2/persistent-files`, `…/{id}/parts/{n}`, `…/{id}/complete`, `DELETE …/{id}`: upload and delete persistent files |
| `chat:read` | `GET /api/v2/ai/models`, `…/chats`, `…/chats/{id}` (needs the aiChatBasic permission too) |
| `chat:write` | create, switch the model of and delete chats, send messages, answer the model's questions (spends credits) |
| `api-tokens:write` | `POST /api/v2/api-tokens`, `DELETE …/{id}`, `GET …/models`: create and revoke API keys (they spend credits) |
| `imagen:read` | `GET /api/v2/imagen`, `…/imagen/{id}`: your generations (needs aiImagen) |
| `imagen:write` | `POST /api/v2/imagen/generate` |
| `tts` | `GET`/`POST /api/v2/tts/voices`, `DELETE …/voices/{id}`, `GET /api/v2/tts/generate` (needs aiTTS) |
| `breeze:read` | `GET /api/v2/tts/breeze/status`, `…/voices`, `…/voices/{name}`: the Breeze TTS server's state and your Breeze voices (needs aiTTS) |
| `breeze:write` | `POST /api/v2/tts/breeze/synthesize`, `…/stream`, `…/voices`, `…/load`, `…/unload`, `DELETE …/voices/{name}` (unload needs the admin panels too) |
| `stock:read` | Retired: the Stock system has been sunset. |
| `smarthome:read` | `GET /api/v2/smarthome/nodes`, `…/rooms` (needs smartHome) |
| `smarthome:write` | `POST /api/v2/smarthome/rooms`, `…/nodes/*`: commands to the devices |
| `vault` | `POST /api/v2/secure-storage/*`: list, extract, hide and share vault files (needs s3, or gallery to read) |
| `roleplay:read` | `GET` characters, personas, background cards, chats and `/api/v2/ai/models` (needs aiRoleplayBasic) |
| `roleplay:write` | start, delete and talk in roleplay chats, regenerate replies (spends credits) |
| `booru:read` | `GET /api/v2/gelbooru/posts`, `…/posts/{id}`, previews, tag suggestions (needs gelbooru) |
| `comics:read` | `GET /api/v2/comics/**`: search, details and page images (needs gelbooru) |
| `khk:admin` | `GET /api/khk/admin/*`: list, read, approve and reject KHK applications (needs khkAdmin) |
| `kathars:admin` | `/api/kathars/admin/staff/**`: edit the Kathars staff list (needs katharsAdmin) |
| `rsys:read` | every `GET /api/v2/rsys/…`: projects, releases, downloads (needs the release_system permission too) |
| `rsys:publish` | create, edit, publish and yank releases; upload, delete and pick files; replace variant compatibility — what a CI job needs, with `rsys:read` |
| `rsys:admin` | create, change and delete projects, and delete whole releases |
| `rsys:maven` | `GET`, `HEAD`, `PUT /api/v2/rsys/maven/**`: publish to the projects' Maven repositories (what a Gradle `maven-publish` job needs, and nothing else) |
| `bakalink:read` | every `GET /api/v2/bakalink/…`: accounts, Komens, marks, timetables (the BakaLink token is still needed) |
| `bakalink:manage` | every other call under `/api/v2/bakalink/`: accounts, pushing and deleting (the BakaLink token is still needed) |

```
gn-cli token create ci-credits --scope credits:read          # expires in 90 days
gn-cli token create laptop -s account:read,credits:read --expires never
GN_TOKEN=gnc_… gn-cli me       # shows only what the token's scopes reach
```

The token is printed once; the server keeps only its hash. Creating, listing and
revoking tokens needs a real login (`auth login`): a CLI token can't manage
tokens, so it can never grant itself more. `--expires` takes `30d`, `12w`, `1y`
or `never`; `--json` works on every `token` command.

Scopes live in `CliScope.java` on the backend; each names an exact list of
method + path pairs.

## Shell completion

Tab completion for commands, subcommands and options in bash, zsh, fish and
PowerShell (Homebrew sets up bash, zsh and fish by itself):

```
echo 'eval "$(gn-cli completion bash)"' >> ~/.bashrc
echo 'eval "$(gn-cli completion zsh)"' >> ~/.zshrc
gn-cli completion fish > ~/.config/fish/completions/gn-cli.fish
gn-cli completion powershell | Out-String | Invoke-Expression   # in $PROFILE
```

The scripts ask the binary what fits (`gn-cli __complete …`), so they never go
stale when commands change.

## Installing

With Homebrew (macOS on Apple Silicon, Linux x64):

```
brew install vitekform/tap/gn-cli
```

Otherwise grab a binary from https://rsys.ganamaga.me/gn-cli/public/latest.json,
or `gn-cli rsys download gn-cli --file gn-cli-<platform>` once you have one.

After that, `gn update` keeps it current: it downloads the newest release for
your platform from rsys, checks its sha256, makes sure it runs, and swaps it in
(through the `gn`/`gncli` links too). `gn update --check` only reports (exit
code 10 when there is a newer one), `--prerelease` takes dev builds. A Homebrew
install is left to `brew upgrade gn-cli`. The interactive shell mentions a new
release at most once a day; `GN_NO_UPDATE_CHECK=1` turns that off.

Every stable `gn-cli-v*` tag updates the tap's formula
(`homebrew/gn-cli.rb.in`, rendered by the `homebrew` job in
`.github/workflows/gn-cli.yml`).

## Building

Needs CMake ≥ 3.20, a C++20 compiler, and libcurl. nlohmann/json is used from
the system when present and downloaded otherwise.

```
# Debian/Ubuntu
sudo apt install cmake g++ libcurl4-openssl-dev nlohmann-json3-dev
git clone https://github.com/vitekform/gn-cli.git
cd gn-cli
cmake -S . -B build && cmake --build build -j
./build/gn-cli --help
```

On Windows, take curl from vcpkg (see `.github/workflows/gn-cli.yml`).

CI builds and tests Linux x64 and Windows x64 on pushes and pull requests.
macOS arm64 joins tagged releases, commits containing `[macos]`, and manual
runs with `macos: true`. Pull requests and manual runs without `publish: true`
do not publish. See [CONTRIBUTING.md](CONTRIBUTING.md) for local tests.

To release, bump `VERSION` in `CMakeLists.txt` and push to `main` or `master`:
the `gn-cli tag` workflow tags the commit `gn-cli-v<version>` and dispatches
the release workflow. Stable releases go to rsys, GitHub Releases and the
Homebrew tap. Pushes also publish development prereleases when `RSYS_TOKEN`
is configured. A version already tagged is not tagged again. See
[PUBLISHING.md](PUBLISHING.md) for repository setup and release recovery.

## Licence

gn-cli is licensed under the [MIT License with an additional User-Agent
condition](LICENSE). Modified builds must send a custom User-Agent that
identifies the modified client distinctly, or include the literal `_fork`
in their User-Agent. This is a modified MIT licence, not the unmodified MIT
licence. Hosted NextGen services remain governed by their own Terms of Service.

Set the request identity when building a modified client:

```sh
cmake -S . -B build -DGN_CLI_USER_AGENT_PRODUCT=gn-cli_fork
# Or identify your client with its own product name:
cmake -S . -B build -DGN_CLI_USER_AGENT_PRODUCT=my-client
```

The resulting User-Agent is `gn-cli_fork/<version> (<os>)` or
`my-client/<version> (<os>)`. Official builds retain `gn-cli/<version> (<os>)`.
Sessions are bound to the User-Agent used at login and keep that saved value
across updates. Use a separate config directory for a modified client, for
example `GN_CLI_CONFIG_DIR=~/.config/gn-cli_fork`, and log in there. If reusing
an existing config directory, run `gn-cli auth logout` and log in again after
changing the User-Agent product. Do not reuse an official build's saved
session unchanged in a modified build.

`gn-cli license` prints the licence and third-party notices;
`gn-cli license third-party` prints only the notices. CMake embeds `LICENSE`,
`THIRD-PARTY-NOTICES` and `TERMS.en.md` into the binary, and rebuilds when any
of them changes. The terms are a versioned local copy; builds require no
frontend checkout and never download current terms. When a dependency is
added or Windows vcpkg packages change, update `THIRD-PARTY-NOTICES`.
The `homebrew` job copies `homebrew/LICENSE` to the tap.

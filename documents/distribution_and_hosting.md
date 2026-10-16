# Distribution and hosting: the production stage

Status: **proposal, for the owner's decision** (2026-10-16). This is the design for the
production stage. Nothing here is built yet. The open questions are at the end.

The goal has two parts. A player can **start their own server quickly** or **join the
owner's server**. Before that, the owner wants to **run a test server at home for friends
this week**. This document looks at how comparable games package, store, install and run
their games. It then recommends a shape for EconSpace and splits the work into phases and
issue-sized slices.

## Where we are today (read from the code)

| Area | Today | Why it matters for production |
|---|---|---|
| World save | `world.json` beside `econserver.exe` (`GetApplicationDirectory()`), rewritten whole every 60 s (`WORLD_SAVE_INTERVAL`), 3 backups (`.bakN`), atomic write-then-rename (`Save::WriteFileAtomic`) | Sound for one file. A new build unpacked over the old folder lands on top of the save. |
| Sealed regions | `regions/<slot>.json`, written once when a region opens, about 40 KB each; archives in `regions/archive-<id>.json` | Write-once data is ideal for plain files. |
| Accounts | `account_<name>.json` beside the **executable**, not the world (#392); about 600 B each; saved on dock and at checkpoints | An account from another world logs into this one. There is no consistent snapshot across files. |
| Designs | `designs.json` beside the world | One more file outside any transaction. |
| Game data | The server reads `data/` from `SIM_DATA_DIR`, the **source tree path baked in at configure time**. The client does the same with `GAME_DATA_DIR`, and so does econagent. | **A server or client copied to another machine cannot find its data.** This blocks any distribution. |
| Runtime libraries | MinGW builds link libgcc/libstdc++/winpthread statically | A Windows exe already runs from a bare folder. |
| Client start | `econspace connect <host> <port> <account> <secret>`: no connect screen, and the secret sits in argv and in shell history | Friends need a shortcut or a `.bat` file now and a login screen later. |
| Version check | `PROTO_VERSION` (32) in every message, with a clear refusal | Data files (`archetypes.json`, `ships.json`, `factions.json`...) are not checked. A client whose data differs from the server's still connects and gets the wrong result without any error. |
| Admin | A stdin console: `tide`, `open`, `drift`, `pause`, `resume`, `help` | Under systemd or Docker there is no stdin. There is no kick, ban, say, save or shutdown (#352). |
| Login | Challenge-response (#106); `Host::Gate` limits connections, registrations and retries (#341) | `stored` crosses in clear on registration. The account file's `stored` is password-equivalent. Nonces come from `mt19937_64`. There is no encryption (#351). |
| Network | One TCP port (default 50800); `--public` binds to all interfaces | One TCP port is the easiest case for NAT and firewalls. |
| Logs | `printf` to stdout, with no timestamps or levels (#352) | Unusable after the fact. |
| CI | Windows, Linux, ASan and macOS builds; no release artefacts | Nothing to download yet. |

## 1. How comparable games do it

### Persistence

| Game | Storage | Notes |
|---|---|---|
| **Veloren** (open-source voxel MMO-lite, Rust) | **SQLite** for characters and accounts; terrain regenerated from the seed | Migrations run at server start. Its 2020 devblog chose SQLite with migrations "so that we don't have to wipe the database with each version". Same shape as ours: seed plus a database of what changed. [docs][veloren-persist], [devblog][veloren-blog] |
| **Luanti / Minetest** | Pluggable per kind: map, players, auth and mod storage each choose `sqlite3` (default), `leveldb`, `redis` or `postgresql` in `world.mt`; `--migrate` converts between them | SQLite "works on every build and needs no configuration... a good fit for singleplayer and small servers". PostgreSQL is for big servers. LevelDB has corrupted worlds after power loss. [backends][luanti-db] |
| **Factorio** | One save `.zip` per world, autosaved in rotating slots | No accounts on the server (factorio.com verifies users). [wiki][factorio-mp] |
| **Space Engineers** | A world folder (`Sandbox.sbc` + `SANDBOX_0_0_0_.sbs`) | Hosts say: stop the server or copy the whole folder, because single files do not make a working world. [guide][se-world] |
| **Minecraft** | Region files (`.mca`) plus JSON lists (`whitelist.json`, `ops.json`, `banned-players.json`) | Backups are file copies, with `save-off`/`save-all` around them. |
| **Ryzom Core** (open-source MMO, C++) | **MySQL** plus its own shard services | Running a server is a project in itself. [Ryzom Core][ryzom] |
| **EVE Online** | **Microsoft SQL Server** cluster behind one shard | The opposite end of the scale. [CCP 2011][eve-db], [High Scalability][eve-hs] |

**Pattern.** Small and self-hosted games use files or SQLite. Games with accounts and
persistent shared state (Veloren, Luanti) settle on **SQLite by default and a server
database only as an option for large hosts**. A dedicated database server (MySQL, MSSQL)
appears only when a studio runs the service itself, and it makes self-hosting much harder.

### Server distribution

| Game | Shape |
|---|---|
| **Factorio** | A headless Linux tarball: `--create` a save, `--start-server save.zip --server-settings server-settings.json`; UDP 34197; `server-adminlist.json`; visibility `public` / `lan` / hidden; clients must run the **exact same version and mods**, checked by checksum. [wiki][factorio-mp] The community Docker image `factoriotools/factorio` keeps config and saves in a volume and generates an RCON password into the config dir. [image][factorio-docker] |
| **Veloren** | An official `docker-compose.yml`. The server updates itself to the weekly release with Watchtower. Port 14004, with metrics and query ports optional. [book][veloren-docker] |
| **Valheim** | A Steam dedicated server; `-port 2456 -password ... -crossplay`; `-crossplay` relays through a backend and gives a join code, so no port forwarding is needed; `-backups N -backupshort -backuplong`. [guide][valheim] |
| **OpenTTD** | The **Game Coordinator**: a server registers and gets a short **invite code**. The client tries a direct connection, then STUN hole punching, then a TURN relay run by openttd.org. "Invite-only" servers are not listed publicly. [docs][openttd-gc], [news][openttd-news] |
| **Luanti** | `server_announce = true` puts a server on the public list (servers.luanti.org); `server_address` must resolve, or the list refuses it. Debian ships `luanti-server` with a systemd unit. [setup][luanti-setup] |
| **Minecraft** | One jar + `server.properties`; `white-list` / `enforce-whitelist`; `online-mode` checks identity with Mojang; RCON is plain text and should stay local or behind a VPN. [config][mc-props], [baseline][mc-sec] |
| **Space Engineers** | A Windows dedicated server; on Linux only through community Wine images. [image][se-docker] |

**Pattern.**
- **One binary plus one config file in a world directory.** CLI flags are overrides.
- **A Docker image for 24/7 hosting.** The save lives in a volume, so updating means pulling the image.
- **Strict version matching**, including data/mods by checksum (Factorio).
- **A short invite code or a public list** so a player never types an IP. Relays exist for players behind NAT (OpenTTD, Valheim).
- **Remote admin (RCON) is plain text** in Minecraft and Factorio, and the advice everywhere is to keep it off the internet.

### Client distribution

| Game | Shape |
|---|---|
| **Veloren** | The Airshipper launcher downloads and updates the game; plus a zip, Flatpak and distro packages |
| **Unvanquished** | A launcher that downloads game data over BitTorrent with web seeds and verifies it; re-running an install is a repair; a universal zip also exists. [launcher][unv-launcher], [distribution][unv-dist] |
| **Luanti** | A portable zip on Windows; Flatpak, PPA and distro packages on Linux |
| **itch.io games** | `butler push` uploads per-channel builds. The itch app applies binary patches. A direct download does **not** update itself, but the game can ask an endpoint for the latest version and tell the player. [butler][butler] |
| **Steam games** | Steam updates the client. Dedicated servers ship through SteamCMD, often with `login anonymous`. [SteamCMD][steamcmd] |

**Code signing on Windows.** An unsigned exe shows SmartScreen's "Windows protected your
PC" warning until the file builds up reputation.
- **Azure Artifact Signing** (formerly Trusted Signing) costs about $10/month. Individuals can use it only in the **USA and Canada**. Microsoft says the warning still fades only "once the file hash has sufficient download history". [Microsoft][ms-sign], [SmartScreen][ms-smartscreen]
- **OV certificates** cost $150–300 a year and **EV** $400+, now with the same SmartScreen behaviour. [Microsoft][ms-sign]
- **SignPath Foundation** signs **OSI-licensed open-source projects for free**. It requires an automated build in CI, a published signing policy and no proprietary parts. The certificate is in SignPath Foundation's name. [terms][signpath-terms], [about][signpath]

EconSpace is MIT-licensed, so it qualifies on licence.

**Linux.** An AppImage must be built on the **oldest** distro it supports, because glibc
runs forward but not backward. It must **not** bundle `libGL`/`libEGL`/`libdrm`.
[AppImage][appimage], [Mesa][mesa-appimage] raylib needs only GL and X11/Wayland from the
system, so a statically linked build on an old base is close to portable already.

**Installers.** NSIS (with NsisMultiUser) and Inno Setup (`PrivilegesRequired=lowest`)
can both install per user without admin rights. [comparison][installers]

### Security baselines elsewhere

- **Identity.** Minecraft calls `online-mode` "the single most important security setting"; a whitelist without verified identity "can easily be bypassed". [baseline][mc-sec] Factorio's `require_user_verification` does the same with factorio.com accounts. Both lean on a central account service. A self-hosted EconSpace server has none, so **the server's own login is the identity**.
- **RCON.** Use a long random password, keep it local or behind a VPN, and never expose it to the internet. [baseline][mc-sec]
- **Password protocols.** SRP is considered superseded. **OPAQUE** (RFC 9807, 2025) is an augmented PAKE that "hides the password from the server, even during password registration" and resists precomputation after a server compromise. [RFC 9807][opaque], [Green][srp-green]
- **Overlay networks for friends.** Tailscale can share one machine with another person's account without exposing the rest of the network, and shared devices do not count toward the device limit. [Tailscale][tailscale]
- **Carrier-grade NAT.** If the router's WAN address is in `100.64.0.0/10`, port forwarding cannot work at all. The fix is a public IP from the ISP, IPv6, or a tunnel or relay. [CGNAT][cgnat]

## 2. Persistence: recommendation

### Sizes we are designing for

From `documents/world_generation_v2.md` (scale table) and the files on disk:

| Data | At 1,000 systems | At 10,000 systems | Write pattern |
|---|---|---|---|
| `world.json` (seed, opening log, per-system changes, factions, markets) | ≈ 400 KB, 20 ms | ≈ 4 MB, 200 ms | Rewritten whole every 60 s |
| Sealed regions | ≈ 50 × 40 KB = 2 MB | ≈ 500 × 40 KB = 20 MB | Written once |
| Accounts | 600 B each | 10,000 accounts ≈ 6 MB | Per player, on dock and at checkpoint |
| Designs | small | small | At checkpoint |

These sizes are tiny for any storage engine. **Performance is not the reason to change
storage. Consistency is:**

1. **No consistent snapshot.** `world.json` and every account are written at different moments. A backup copied while the server runs, or a crash between two writes, can restore credits that were paid to a station but not yet taken from the account, or the reverse. As soon as players trade with each other (contracts, player markets, #44), a crash between the two account writes **duplicates or destroys goods**.
2. **Whole-file rewrites grow with the world.** A 4 MB rewrite every minute is fine on its own (#350 already moves it off the tick thread). But it rewrites everything to change a few systems.
3. **Many files on a home machine** are easy to lose or mix up between worlds (#392 is exactly this).

### Decision

- **P0 (this week): keep JSON files** and fix *where* they live.
  - Introduce one **world directory** holding `world.json`, `regions/`, `designs.json` and `accounts/` (#392), plus `server.json` and `logs/`.
  - Nothing else changes, and the files stay readable when something goes wrong in the first playtest.
- **P1 (first public release): one SQLite database per world, `world.db`, in WAL mode,** behind a small storage interface.
  - SQLite is one C file (the amalgamation), public domain, and builds with MinGW, GCC and Apple Clang like the rest of the tree. No external service to install.
  - It is the default in Veloren and Luanti for exactly this kind of server. SQLite's own guidance puts this well inside its range. [SQLite][sqlite-when]
  - **Rows hold the JSON we already write**, not a normalized schema. The existing serializers, `ACCOUNT_VERSION`/`WORLD_VERSION` checks and "refuse a newer file" rules (#20) stay as they are. What we gain is **transactions**: a checkpoint writes the world and every dirty account in one transaction, and a player-to-player trade commits both sides or neither.
  - **Online backups** use the SQLite backup API or `VACUUM INTO 'backups/world-<time>.db'` from a consistent snapshot without stopping the server. Copying the `.db` file alone misses whatever is still in the `-wal` file. [backup][sqlite-backup] Each backup is checked with `PRAGMA integrity_check`.
  - **Migrations** keep an ordered list of SQL steps in C++, keyed by `PRAGMA user_version` and run at start, the same way Veloren runs its migrations at startup. A database newer than the build is refused, as a save is today. Schema changes (tables) and blob changes (the JSON version fields) are versioned separately.
- **PostgreSQL: not planned.** It earns its cost only when **several server processes share one store**, for example one account service for many shards or a cluster like EVE's. Keeping storage behind an interface leaves that door open (Luanti offers both). The trigger to revisit is a decision to run more than one process per world.
- **LevelDB/RocksDB: no.** Luanti documents corruption on unclean shutdown, and a home server *will* lose power.

### Schema sketch (P1)

```
meta(key TEXT PRIMARY KEY, value TEXT)          -- seed, GENERATOR_VERSION, SKELETON_VERSION, WORLD_VERSION, opening log
regions(slot INTEGER PRIMARY KEY, version INTEGER, doc TEXT, sealed_at REAL)   -- write-once
archive(id INTEGER PRIMARY KEY, doc TEXT)
systems(id TEXT PRIMARY KEY, changes TEXT)      -- one row per changed system: the per-system part of world.json today
world(key TEXT PRIMARY KEY, doc TEXT)           -- factions, markets, sieges: what world.json holds besides systems
accounts(name TEXT PRIMARY KEY, version INTEGER, salt TEXT, verifier TEXT, doc TEXT, created REAL, last_login REAL)
designs(id TEXT PRIMARY KEY, doc TEXT)
bans(kind TEXT, value TEXT, until REAL, reason TEXT)   -- account or address
```

Splitting `world.json` into `systems` rows means a checkpoint writes only systems that
changed, which also removes most of #350's save cost.

### Migration path

1. **S-store-1:** `Save::Store` interface; `FileStore` is today's behaviour moved behind it, with no change to the files. Tests run against both stores later.
2. **S-store-2:** `SqliteStore`; `econserver import <world-dir>` converts a file world into `world.db`, and `econserver export <world.db> <dir>` writes the same JSON files back. JSON remains the **interchange format** for debugging, the editor, test fixtures and admin packs.
3. **S-store-3:** new worlds default to SQLite. A file world still loads, and `--store files` keeps the old behaviour for one release.
4. **S-store-4:** transactions used for real: checkpoint = one transaction; player-to-player transfers commit atomically.

## 3. Server distribution: recommendation

### What a hoster gets

```
econserver-<version>-windows-x64.zip   econserver.exe, data/, README-server.txt, start-server.bat
econserver-<version>-linux-x64.tar.gz  econserver, data/, README-server.txt, econserver.service
ghcr.io/hel3an/econserver:<version>    the same, Debian-slim, /world as a volume   (P1)
```

The game data travels with the binary and is read from beside it (`<exe dir>/data/`), or
from `--data <dir>`. Saves never live there.

### Layout of a world

```
<world dir>/            default: ./world beside the exe, or --world <dir>
  server.json           name, port, public/private, registration, limits, cap, backup policy
  world.json | world.db the world (P0: files, P1: SQLite)
  regions/              sealed regions (P0)
  accounts/             account files (P0; #392)
  designs.json
  allowlist.txt         one account per line (when registration is "allowlist")
  admins.txt            accounts allowed to use in-game admin commands (P1)
  bans.txt              (P0: file; P1: table)
  server.key            the server's identity key (P1, encryption)
  logs/econserver-<date>.log
  backups/
```

**Config: a file with CLI overrides.**
- `econserver init <dir>` writes a commented `server.json` and an empty world.
- `econserver host --world <dir>` reads it. A flag (`--port`, `--public`, `--centres`...) overrides the file for that run.
- Unknown keys are a load error, as in archetypes (#191).

This is Factorio's and Luanti's model, and it keeps a 24/7 server's settings in one place
that gets backed up with the world.

**Logs.**
- Timestamped, levelled lines to stderr and to `logs/` with daily rotation.
- A metrics line once a minute: tick mean and p99, clients, bytes per client, save ms.
- This is #352 as already written.

**Admin.**
- **P0:** extend the stdin console with `who`, `kick`, `ban`, `unban`, `allow`, `say`, `save`, `backup`, `shutdown [seconds]` (with a countdown broadcast), and `status`.
- **P1:** the same commands over a **local-only** channel, so they work under systemd and Docker where there is no stdin. Either a Unix socket / named pipe or `127.0.0.1` plus a token file. Usage: `econserver admin --world <dir> "kick bob"` (with Docker, `docker exec econserver econserver admin ...`).
- **P1:** in-game admin commands for accounts in `admins.txt`, carried by the game protocol once it is encrypted.
- **No internet-facing plain-text RCON**, for the reasons Minecraft's own guidance gives.

**Running as a service.**
- **Linux:** a shipped `econserver.service` with `DynamicUser=yes`, `StateDirectory=econspace`, `ProtectSystem=strict`, `NoNewPrivileges=yes`, `Restart=on-failure` and `KillSignal=SIGTERM`. The server already saves on `SIGTERM`. [systemd][systemd]
- **Windows:** the P0 answer is a console window (`start-server.bat`). A Windows service (NSSM or the SCM API) is P2.
- **Docker (P1):**
  - The image has `/world` as a volume, `EXPOSE 50800/tcp`, and a `HEALTHCHECK` that runs `econserver admin status`.
  - Updating means pulling the new tag. The world survives because it is in the volume. Factorio's image warns about exactly this.
  - Watchtower is optional and not recommended by default: an automatic update across a `PROTO_VERSION` bump locks every client out until it updates too.

**Ports and NAT.**
- One TCP port, default 50800. TCP needs a single forward and no hole punching.
- `README-server.txt` covers port forwarding, the Windows firewall prompt, and the CGNAT check: compare the router's WAN IP with what an external site shows, and look for a `100.64.x` address.
- **Fallbacks for a server behind CGNAT:**
  1. **Tailscale or ZeroTier**, sharing just the server machine with each friend. This also encrypts the traffic.
  2. **IPv6**, if both ends have it. Today the transport listens on IPv4 only (`INADDR_ANY` in `TcpConnection`), so dual-stack listening and connecting is slice P1-16.
  3. **A small VPS** running the server itself, not a relay. This is also what #352 recommends for a public server.

**Finding a server.**
- **P0:** the owner sends an address.
- **P1:** an **invite string**, `econ://host:port/<key-fingerprint>`, which the client accepts on the command line, by paste in the connect screen or as a registered URL handler. It names the server **and** pins its key (see §5).
- **P2:** a **master list**. Each public server POSTs a heartbeat (name, version, players, address, fingerprint) to a tiny HTTPS service, and the client reads the list. This is Luanti's `server_announce`. OpenTTD-style coordination (STUN, then a TURN relay) is out of scope until there are enough players behind CGNAT to justify running a relay.

**Version matching.**
- `PROTO_VERSION` stays the wire check.
- **Built (#400):** `Hello` also carries the **build version** (`Version::NUMBER`) and a **SHA-256 of each shared data file** (`Compat::SharedFiles`: archetypes, blueprints, factions, goods, ships; line endings ignored). Art only the client reads (materials, modules, look, shaders, themes) and files only the server reads (world_rules, pins) are left out: a difference there decides nothing on the client.
- A mismatch is refused with a `Bye` that names the server's build and the client's, or exactly which files differ, and where to download the matching build. Factorio does exactly this with mod checksums.
- Without this, a friend with last week's `ships.json` connects, predicts with the wrong stats and sees rubber-banding instead of an error.

### Releases

- **Naming (#186):** a semver for compatibility and tags (`v0.1.0`) and a codename per minor release from an alphabetical list (Anchor, Beacon, Current, Drift, Ebb, Fathom, Gale, Harbor...). The first release is **EconSpace 0.1 Anchor**; patch releases keep the codename.
- **One source of truth:** `project(EconSpace VERSION ...)` and `ECON_CODENAMES` in `CMakeLists.txt`, generated into `core/Version.h`. Every binary answers `--version`; the client shows it in its title bar and settings window, the server in its first log line.
- **Built (#400):** a tag `v*` runs `.github/workflows/release.yml`: Release builds on `windows-latest` (MSYS2) and `ubuntu-22.04` (the oldest glibc we support), the tests and smoke tests, a check that the tag matches the build's version, the `dist` packages `econspace-<version>-<codename>-windows.zip` / `-linux.tar.gz`, and a **draft** GitHub Release with generated notes. Publishing stays a person's decision. Run by hand, it builds and keeps the packages as artifacts.
- Later: macOS, SHA-256 sums beside the archives, itch.io with `butler`.

## 4. Client distribution: recommendation

- **P0: a portable zip** from the GitHub Release: `econspace.exe`, `econagent.exe`, `data/`, `README.txt`, `connect-example.bat`.
  - The client must read `data/` from beside the exe in a release build. `GAME_DATA_DIR` becomes a dev-only option (`-DECON_DEV_DATA=ON`, on by default only in Debug).
  - Player settings (UI layout, theme, saved servers) go to a per-user directory (`%APPDATA%\EconSpace`, `~/.local/share/econspace`), not beside the exe. Then an update is "unpack over the old folder".
  - SmartScreen will warn. The README explains "More info, then Run anyway".
- **P1: a connect screen** replaces argv secrets.
  - It has fields for server (or invite string), account and secret, a list of saved servers, and a "remember me" option.
  - "Remember me" stores the derived `stored` value, not the secret, in the per-user directory.
  - This is also where a version mismatch is shown with a download link.
- **P1: code signing through SignPath Foundation** (free for OSI projects; build must be automated in CI, which it is). Fallback: an OV certificate. Azure Artifact Signing is out unless the owner is in the US or Canada.
- **P1: itch.io** as the second channel.
  - CI runs `butler push` per channel (`windows`, `linux`, `mac`).
  - The itch app patches the game automatically. Direct downloads get the in-game "new version" notice.
  - It costs nothing and gives a store page without Steam's process.
- **P1: an in-game update notice.** At start the client asks the GitHub Releases API (or itch's version endpoint) for the latest version and says so. The server's version refusal says which version to get.
- **P1: Linux AppImage**, built on the oldest supported base (Ubuntu 20.04 container), with no GL libraries bundled. The tarball stays.
- **P2: an installer** (Inno Setup, per user, no admin) with Start-menu shortcuts and the `econ://` URL handler. It is optional, because the zip keeps working.
- **P2: Steam** (Steam Direct fee per app). This is a business decision rather than a technical one. A Steam build would use Steam only for distribution and updates. Our login stays ours, because self-hosted servers have no Steam to ask.
- **P2: macOS.** CI already builds with Apple Clang, but an unsigned, un-notarized app is blocked by Gatekeeper by default. Notarization needs a paid Apple developer account. Until then macOS is "build from source".
- **Not planned: our own launcher/updater** (Airshipper and Unvanquished style). itch, Steam and the update notice cover it for a fraction of the work. Revisit if neither store is used.

## 5. Security: recommendation

### What the current login does and does not protect (from `Auth.h`, #351)

- **Protected:** a listener who copies a login proof cannot replay it, because the nonce changes each time.
- **Not protected:**
  - `stored = H(salt ‖ secret)` (stretched) **crosses in clear at registration**, and the proof is `H(nonce ‖ stored)`. So anyone who sees a registration, or reads an account file or a backup, **can log in as that player** without the secret.
  - All traffic is readable and can be modified, so a session can be taken over after login by anyone on the path.
  - Nonces come from `mt19937_64` seeded by `random_device`, which is deterministic on older MinGW.
- **Already done:** `Host::Gate` (#341) caps connections, registrations per address and hour, and backs off after wrong secrets.

### Friends-only server (P0)

1. **Registration is closed or an allowlist.** `server.json: "registration": "allowlist"`, plus `allow <name>` on the console. A stranger who finds the port cannot create an account, so the open registration path that leaks `stored` is not exposed.
2. **Tell friends to use a unique, throwaway secret.** It is readable on the wire and the account file is password-equivalent. Put this in the README and in the connect example. #351 already says so.
3. **Expose one TCP port and nothing else.** The server runs as an ordinary user, never as admin or root.
4. **Prefer Tailscale or ZeroTier when practical.** No open port, encryption for free, and the owner's home IP is not handed out. Port forwarding is fine for a weekend with friends who know the risks.
5. **Back up the world directory before every update.** `econserver backup` (P0) copies the world directory to `backups/<time>/` after a forced save, with the world paused for the copy.
6. **Seed the nonce from the OS** (`BCryptGenRandom` / `getrandom`). It is small and closes the weakest part of #351. **This one is in P0.**

### Public server (P1)

1. **An encrypted transport, with the server's key pinned.** Use **libsodium** (ISC licence, a C library that builds with MinGW, GCC and Apple Clang, widely audited):
   - The server generates an X25519 identity key at `init` (`server.key`).
   - Each connection runs `crypto_kx` against an ephemeral client key, so every session has its own keys (forward secrecy).
   - Each frame is sealed with `crypto_secretstream` or `crypto_aead_xchacha20poly1305`, keyed per direction.
   - **Key trust:** the client pins the server key by its fingerprint from the invite string. Without one, it trusts the first key it sees and warns if it changes, like SSH.
   - **Why not TLS:** TLS needs certificates, and a home server has no domain and no CA certificate, so it would be self-signed and pinned anyway. That gives the same trust model with a much larger library (OpenSSL or mbedTLS).
   - The framing (#14) stays. Encryption is a layer inside `TcpConnection`, so `LocalTransport` (tests) is untouched.
   - Bump `PROTO_VERSION`.
2. **Registration and login inside the encrypted channel, with no password-equivalent files.**
   - Inside the channel the client sends `stored` (still derived client-side, so the raw secret never leaves the machine). The server keeps only `crypto_pwhash(stored, salt2)` (Argon2id).
   - A stolen account file or backup then no longer logs anyone in.
   - The challenge-response round goes away; the channel already gives freshness.
   - `ACCOUNT_VERSION` bump; old accounts are converted on their next successful login.
3. **Admin over the same encrypted channel** for accounts in `admins.txt`. The local admin socket for the host is P1 too.
4. **Run public servers on a VPS, not a home machine.** The project review already found that a home uplink cannot carry 50 players; a public address also draws scans and DoS that a home connection should not absorb.
5. **Abuse handling:** bans by account and address, a `report` command that writes to the log, and rate limits on chat once chat exists.
6. **Dependency hygiene:** pin libsodium, SQLite and nlohmann/json by version and hash in `FetchContent`, and turn on CodeQL on Linux (#395).

### Later (P2)

- **OPAQUE (RFC 9807)** in place of the "send `stored` over the channel" step. The server never learns anything secret-equivalent, even at registration. Worth it once there are many public servers run by people the players do not know: today a malicious server owner could collect `stored` values and, if a player reuses the same secret elsewhere, use them against other EconSpace servers.
- Fuzzing the decoders in CI (already proposed in the review).

## 6. Plan

Each line below is meant to be one issue and one PR. Existing issues are named where they
already cover the work.

### P0: a friends' test server this week

Goal: the owner runs a Release `econserver` at home and sends friends a zip. They connect
with a `.bat` file, play for an evening, and nothing is lost on a crash or an update.

| # | Slice | Size | Notes |
|---|---|---|---|
| P0-1 | **Data from beside the exe.** `SIM_DATA_DIR`, `GAME_DATA_DIR` and `AGENT_DATA_DIR` become dev-only defaults (ON in Debug, OFF in Release, `-DECON_DEV_DATA`); every binary takes `--data <dir>` and falls back to `<exe dir>/data/`; the server's build copies `data/` like the client's | S | **Blocking.** A Release server copied to another folder cannot find its data today |
| P0-2 | **World directory.** `--world <dir>` (default `./world` beside the exe), holding `world.json`, `regions/`, `designs.json`, `accounts/`, `logs/`; an account from another world gets the start choice | S | Is **#392**; a one-time move of existing files |
| P0-3 | **`server.json`** written by `econserver init <dir>`; read by `host`; CLI overrides; strict keys | S | Port, public, registration (`open` / `allowlist` / `closed`), limits, centres, cap, dev |
| P0-4 | **Allowlist and bans.** `allowlist.txt`, `bans.txt`; console `allow`, `ban`, `unban`; a refused login gets a `Bye` with the reason | S | Builds on `Host::Gate` |
| P0-5 | **Console: `who`, `kick`, `say`, `save`, `backup`, `shutdown [s]`, `status`.** `backup` = forced save plus a copy of the world dir to `backups/<time>/`, keeping N copies | S | Part of **#352** |
| P0-6 | **Timestamped logs to file** plus the per-minute metrics line | S | Part of **#352** |
| P0-7 | **Nonces and salts from the OS RNG** (`BCryptGenRandom` / `getrandom` / `arc4random`) | XS | Part of **#351** |
| P0-8 | **Version and data hash in `Hello`.** `--version` everywhere; a mismatch is refused with a `Bye` that names both builds | S | `PROTO_VERSION` bump |
| P0-9 | **Release packaging in CI.** A tag builds Release on Windows and Linux; attaches `econspace-<v>-windows-x64.zip`, `econserver-<v>-windows-x64.zip`, Linux tarballs, SHA-256 sums; READMEs: port forwarding, CGNAT check, Tailscale option, "use a throwaway secret", SmartScreen | M | Also the place for `connect-example.bat` and `start-server.bat` |
| P0-10 | **Host checklist** (`docs/hosting.md`): Release build, world directory, firewall, Tailscale or port forward, backup before update, how to update | XS | Docs only |

Not in P0, on purpose:
- **Encryption.** Friends plus an allowlist plus throwaway secrets is an acceptable risk for a weekend. Tailscale removes it entirely.
- **SQLite.** Files with a backup command are enough for one evening's data.
- **A connect screen.** A `.bat` file is enough for people the owner knows.

### P1: first public release

| # | Slice | Size | Notes |
|---|---|---|---|
| P1-1 | **Encrypted transport** (libsodium: `crypto_kx` + AEAD per frame; `server.key`; key pinning by fingerprint, else trust on first use) | M | Rest of **#351**; `PROTO_VERSION` bump |
| P1-2 | **Login inside the channel; Argon2id on the server; no password-equivalent files** | S | `ACCOUNT_VERSION` bump; convert on login |
| P1-3 | **Invite strings** `econ://host:port/<fingerprint>`; `econserver invite` prints one | S | |
| P1-4 | **Connect screen** in the client: invite or address, account, secret, saved servers, remember me; version-mismatch message with a download link | M | Uses `Ui::Desk`; no secret in argv any more |
| P1-5 | **`Save::Store` interface; `FileStore`** (no behaviour change) | M | S-store-1 |
| P1-6 | **`SqliteStore`, `import` / `export`** | M | S-store-2 |
| P1-7 | **SQLite by default; online backups (`VACUUM INTO` + integrity check) on a schedule; migrations by `user_version`** | S | S-store-3 |
| P1-8 | **Checkpoint in one transaction; atomic player-to-player transfers** | S | S-store-4; before any player trading lands |
| P1-9 | **Off-thread saves** | M | Is **#350**; easier after P1-5 |
| P1-10 | **Local admin channel** (`econserver admin "<cmd>"`) and **in-game admin** for `admins.txt` | S | Rest of **#352** |
| P1-11 | **Docker image** on GHCR, `docker-compose.yml` example, healthcheck; **systemd unit** in the tarball | S | |
| P1-12 | **Code signing** with SignPath Foundation in the release job | S | Apply first; approval takes time |
| P1-13 | **itch.io channels** via `butler push` from CI; **in-game update notice** | S | |
| P1-14 | **Linux AppImage** built on an old base | S | |
| P1-15 | **Per-user settings directory** for the client (`%APPDATA%`, XDG) | XS | Can move into P0 if the zip-over-zip update loses UI layouts |
| P1-16 | **IPv6**: dual-stack listen (`::` with `IPV6_V6ONLY` off) and connect by name (`getaddrinfo`) | S | A way round CGNAT for hosts whose ISP gives IPv6 |

### P2: later

| # | Slice | Notes |
|---|---|---|
| P2-1 | **Master server list**: heartbeat from public servers, list in the client | A tiny HTTPS service; the owner hosts it |
| P2-2 | **OPAQUE** login | Once strangers run servers for strangers |
| P2-3 | **Installer** (Inno Setup, per user) with the `econ://` handler | Optional |
| P2-4 | **Steam** release | A business decision |
| P2-5 | **macOS** signed and notarized | Needs an Apple developer account |
| P2-6 | **Windows service** for the server | Or document NSSM |
| P2-7 | **NAT relay / coordinator** (OpenTTD-style) | Only if many hosts are behind CGNAT |
| P2-8 | **PostgreSQL store** | Only if several processes share one world or one account service |

## Questions for the owner

1. **Where does the friends' server run this week**: the laptop the owner plays on, or another machine? Running the server and the client on the same Intel UHD 620 laptop is possible but competes for CPU (see the "one machine" note). Is the home connection behind CGNAT? A quick check of the router's WAN IP answers that.
2. **Port forwarding or Tailscale/ZeroTier for the friends' test?** Tailscale needs each friend to install it but gives encryption and hides the home IP. Port forwarding needs nothing on their side.
3. **Registration on the friends' server: allowlist (recommended) or open?**
4. **Is SQLite in P1 acceptable**, given that the world becomes one `world.db` file and JSON becomes an export format rather than the live save?
5. **Encryption: libsodium with pinned keys (recommended) or TLS?** TLS would need a domain and certificates for every public server, or self-signed certificates pinned the same way.
6. **Distribution channels for P1: GitHub Releases plus itch.io?** Is Steam a goal at all, and when?
7. **Code signing:** apply to SignPath Foundation (free, certificate in their name, project rules) or buy an OV certificate (≈ $150–300 a year, in the owner's name)?
8. **Will the owner host an official public server**, and on what: a VPS (recommended) or home? This decides whether P1's Docker image and admin channel are needed right away.
9. **Is a public server list (P2-1) wanted**, and is the owner willing to run the small service behind it?
10. **Release cadence and version numbers:** `v0.x.y` tags, and how often? Every release that changes `PROTO_VERSION` locks out older clients, so the cadence is also how often friends must update.

## Sources

[veloren-persist]: https://docs.veloren.net/veloren_server/persistence/index.html
[veloren-blog]: https://veloren.net/devblog-56
[veloren-docker]: https://book.veloren.net/players/server-hosting/on-docker.html
[luanti-db]: https://docs.luanti.org/for-server-hosts/database-backends/
[luanti-setup]: https://docs.luanti.org/for-server-hosts/setup/
[factorio-mp]: https://wiki.factorio.com/Multiplayer
[factorio-docker]: https://github.com/factoriotools/factorio-docker
[se-world]: https://docs.emeraldhost.de/en/gameserver/space-engineers/download-world
[se-docker]: https://github.com/Lothsahn/space-engineers-dedicated-docker-linux
[ryzom]: https://wiki.ryzom.dev/ryzomcore/Creating-Your-Own-Game-Using-Ryzom-Core_884852.html
[eve-db]: https://www.eveonline.com/news/view/full-suite-of-upgrades-to-the-tranquility-db-yippie
[eve-hs]: https://highscalability.com/eve-online-architecture/
[valheim]: https://hub.tcno.co/games/valheim/dedicated_server/
[openttd-gc]: https://github.com/OpenTTD/OpenTTD/blob/60e1362477a4644e8a910397d26921a23487e11d/docs/game_coordinator.md
[openttd-news]: https://www.openttd.org/news/2021/09/26/new-multiplayer-experience
[mc-props]: https://minecraft.wiki/w/Server.properties
[mc-sec]: https://terabit.io/kb/secure-minecraft-server-access-baseline
[unv-launcher]: https://wiki.unvanquished.net/wiki/Launcher
[unv-dist]: https://wiki.unvanquished.net/wiki/Infrastructure/Distribution
[butler]: https://itch.io/docs/butler/pushing
[steamcmd]: https://developer.valvesoftware.com/wiki/SteamCMD
[ms-sign]: https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options
[ms-smartscreen]: https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation
[signpath-terms]: https://signpath.org/terms
[signpath]: https://about.signpath.io/product/open-source
[appimage]: https://docs.appimage.org/reference/best-practices.html
[mesa-appimage]: https://gitlab.freedesktop.org/mesa/mesa/-/issues/5788
[installers]: https://kinook.com/blog2/inno-setup.html
[opaque]: https://www.rfc-editor.org/rfc/rfc9807.html
[srp-green]: https://blog.cryptographyengineering.com/should-you-use-srp/
[tailscale]: https://tailscale.com/blog/tailscale-sharing-friends-family
[cgnat]: https://docs.ipcheck.ing/knowledge-base/diagnose/am-i-behind-cgnat
[sqlite-when]: https://sqlite.org/whentouse.html
[sqlite-backup]: https://oneuptime.com/blog/post/2026-09-08-back-up-wal-mode-sqlite-safely/view
[systemd]: https://www.ssdnodes.com/learn/lang/ja/least-privilege-users-vps
[libsodium]: https://doc.libsodium.org/secret-key_cryptography/secretstream

- Veloren: [persistence module][veloren-persist], [devblog 56][veloren-blog], [Docker hosting][veloren-docker]
- Luanti: [database backends][luanti-db], [server setup][luanti-setup]
- Factorio: [Multiplayer wiki][factorio-mp], [factoriotools/factorio-docker][factorio-docker]
- Space Engineers: [world files][se-world], [Linux Docker image][se-docker]
- Ryzom Core: [developer guide][ryzom]
- EVE Online: [Tranquility DB upgrade][eve-db], [architecture][eve-hs]
- Valheim: [dedicated server][valheim]
- OpenTTD: [Game Coordinator][openttd-gc], [new multiplayer experience][openttd-news]
- Minecraft: [server.properties][mc-props], [access baseline][mc-sec]
- Unvanquished: [launcher][unv-launcher], [distribution][unv-dist]
- itch.io: [butler pushing][butler]; Valve: [SteamCMD][steamcmd]
- Microsoft: [code signing options][ms-sign], [SmartScreen reputation][ms-smartscreen]
- SignPath: [Foundation terms][signpath-terms], [open source][signpath]
- AppImage: [best practices][appimage], [Mesa issue on bundling GL][mesa-appimage]
- Installers: [non-admin Inno Setup][installers]
- Passwords: [RFC 9807 OPAQUE][opaque], [Should you use SRP?][srp-green]
- Networking: [Tailscale sharing][tailscale], [CGNAT check][cgnat]
- SQLite: [appropriate uses][sqlite-when], [backing up WAL databases][sqlite-backup]
- libsodium: [secretstream][libsodium]
- systemd: [least-privilege services][systemd]

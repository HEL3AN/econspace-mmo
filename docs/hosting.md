# Hosting a server for friends

This guide sets up an EconSpace server on a headless Ubuntu machine at home, reachable from the
internet through a port forward, with only the people you name allowed in. It then covers the
friend's side: a Windows client that connects to it.

It assumes a machine you can reach over SSH, a public ("white") IPv4 address on your router,
and access to the router's port forwarding page. Commands are for Ubuntu 22.04 or 24.04.

The connection is **not encrypted yet** (#351). Keep registration on the allowlist and tell
your friends to use a secret they use nowhere else.

## 1. Get the server

You need `econserver` with `data/` beside it. Either unpack a release, or build one.

**From a release.** Download `econspace-<version>-<codename>-linux.tar.gz` (for 0.1, `econspace-0.1.0-anchor-linux.tar.gz`) and unpack it into `/opt/econspace`:

```sh
sudo mkdir -p /opt/econspace
sudo tar xzf econspace-0.1.0-anchor-linux.tar.gz -C /opt/econspace --strip-components=1
ls /opt/econspace          # econserver, econspace, econagent, data/, README.txt
```

**From source.** The server links the same engine as the client, so the build needs the X11
and OpenGL headers even though the server never opens a window:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake git libasound2-dev libgl1-mesa-dev \
    libglu1-mesa-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev \
    libxkbcommon-dev libxrandr-dev libwayland-dev
git clone https://github.com/HEL3AN/econspace-mmo.git
cd econspace-mmo
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j "$(nproc)" --target dist   # -> build-release/econspace-<version>-<codename>-linux.tar.gz
sudo mkdir -p /opt/econspace
sudo tar xzf build-release/econspace-*-linux.tar.gz -C /opt/econspace --strip-components=1
```

Always a **Release** build for a server people play on: a Debug build is several times slower.

## 2. A user and a world

The server runs as a user of its own, which owns the world and nothing else:

```sh
sudo useradd --system --create-home --home-dir /var/lib/econspace --shell /usr/sbin/nologin econspace
sudo -u econspace /opt/econspace/econserver init /var/lib/econspace/world
```

`init` makes the **world directory**. Everything the server keeps lives in it:

| In the world directory | What it is |
|---|---|
| `server.json` | the settings (below); written by `init`, then yours to edit |
| `allowlist.txt` | who may play: one account name per line |
| `bans.txt` | who may not, with the reason they are told |
| `world.json`, `regions/` | the galaxy (made the first time the server starts) |
| `accounts/` | one file per player |
| `designs.json`, `corps.json` | ship designs players fly, and their corporations |
| `logs/server-YYYY-MM-DD.log` | the server's log, one file a day (UTC) |
| `backups/<time>/` | copies made by the `backup` command |
| `admin/` | the local command channel (below); owner-only |

`init` writes a server that other machines may reach, on port 50800, open only to the names on
the allowlist. Flags change that: `init <dir> --port 51000`, `--private` (this machine only),
`--open` (anyone may register), `--centres 4`, `--max-regions 60`, `--seed 12`.

### server.json

```jsonc
{
  "port": 50800,               // the TCP port players connect to
  "public": true,              // false: this machine only
  "registration": "allowlist", // or "open": anyone may create an account
  "centres": 3,                // a new world's civilisation centres, 1 to 6
  "randomStarts": false,       // true: new pilots are dealt a start
  "maxRegions": 0,             // the cap on growth; 0: the defaults
  "maxSystems": 0,
  "seed": 0,                   // a new world's seed; 0: random
  "dev": false,                // debug commands for every client -- never for friends
  "backupsKept": 10            // how many copies `backup` keeps; 0: all
}
```

Comments are allowed. An unknown key, or a value of the wrong kind, stops the server with a
message naming it -- a misspelt `"pubilc"` would otherwise run a server nobody can reach.
`centres` and `seed` matter only when the world is made; a saved world keeps its own.

A flag given to `host` overrides the file for that run and is not written back: `--port N`,
`--public`, `--private`, `--open`, `--allowlist`, `--centres N`, `--seed N`,
`--max-regions N`, `--max-systems N`, `--random-starts`, `--dev`.

## 3. Who may play

Add your friends' account names -- the name each of them will type when connecting:

```sh
sudo -u econspace nano /var/lib/econspace/world/allowlist.txt
```

The server reads the file again when it changes, so this works while it runs. So do the
console's `allow <name>` and `deny <name>` (section 6). The first login with a name on the list
creates that account and sets its secret; nobody else can take the name after that.

## 4. Try it in the foreground

```sh
sudo -u econspace /opt/econspace/econserver host --world /var/lib/econspace/world
```

It prints the galaxy it made, who may come in, and `EconSpace server on port 50800, open to
other machines`. Type `status` and Enter: this is the console. `Ctrl+C` stops it -- it saves
everything first. Then set it up as a service (section 5).

## 5. The network

**Firewall.** If `ufw` is on (or you turn it on), allow SSH *first*, then the game port:

```sh
sudo ufw allow OpenSSH
sudo ufw allow 50800/tcp
sudo ufw enable
sudo ufw status
```

**Router.**
1. Give the server a fixed address on your network: a DHCP reservation for its MAC address in
   the router, so the forward does not point at an empty address after a reboot. `ip -4 addr`
   on the server shows its current one (for example `192.168.1.20`).
2. Forward **TCP 50800** from the router's WAN side to that address, port 50800. Nothing else
   needs to be forwarded: the game uses this one TCP port.
3. Your public address is what the router's status page shows as its WAN address, or what
   `curl -4 ifconfig.me` prints on the server. If it starts with `10.`, `100.64`-`100.127`,
   `172.16`-`172.31` or `192.168.`, your provider puts you behind its own NAT and a port forward
   cannot work; ask them for a public address.

**Check from outside.** From a phone on mobile data (not your Wi-Fi), or ask a friend:
`nc -vz <public address> 50800` should say it connected. Many routers do not loop a connection
to their own public address back inside, so from your own network connect to the server's LAN
address instead.

## 6. Run it as a service

```sh
sudo cp /opt/econspace/econserver.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now econserver
systemctl status econserver
```

(The unit comes in the Linux package; in the source it is `tools/dist/econserver.service`.
If you chose other folders than `/opt/econspace` and `/var/lib/econspace`, edit its paths.)
It starts at boot, is restarted if it crashes, and on `systemctl stop` saves the
world and every account, tells each player the server is shutting down, and exits.

### The console without a console

A service has no terminal to type into. `econserver admin` gives the running server the same
commands from the same machine: it puts the command in the world's `admin/` folder, the server
picks it up within a second and writes back what it said.

```sh
sudo -u econspace /opt/econspace/econserver admin --world /var/lib/econspace/world status
```

A shorter name helps -- add to your `~/.bashrc`:

```sh
alias econ='sudo -u econspace /opt/econspace/econserver admin --world /var/lib/econspace/world'
```

and then `econ who`, `econ allow alice`, `econ backup`. Only someone who may write to the
world directory -- the `econspace` user or root -- can give a command; nothing listens on the
network for it.

| Command | What it does |
|---|---|
| `status` | uptime, players, tick time, regions, when the world was last saved |
| `who` | everyone on the server: in which system, from which address, for how long |
| `say <text>` | a line to everyone online |
| `kick <name> [reason]` | off the server now, told why; they can come back (`ban` keeps them out) |
| `allow <name>` / `deny <name>` | add to / remove from the allowlist |
| `ban <name> [reason]` / `unban <name>` | bans.txt; a banned player is removed at once |
| `mute <name> [minutes]` / `unmute <name>` | chat only |
| `access` / `reload` | show who may come in / read allowlist.txt and bans.txt again |
| `save` | write the world and every account now |
| `backup` | save, then copy the world to `backups/<time>/` |
| `shutdown [seconds]` | save, tell everyone, exit; with seconds, everyone is warned first and again 10 s before; `shutdown cancel` takes it back |
| `pause` / `resume` | stop and restart the world's growth |
| `help` | everything else (the tide, regions, roaming holes) |

`shutdown` exits cleanly, so systemd does not restart the server; `sudo systemctl start
econserver` brings it back.

## 7. Logs

```sh
journalctl -u econserver -f                            # live
less /var/lib/econspace/world/logs/server-2026-10-17.log
```

Every line has its time in UTC. Once a minute a `metrics:` line says how many are playing, what
a world tick costs and how much goes out on the wire. Logs are small; to keep a month of them:

```sh
sudo crontab -u econspace -e
# then add:
30 4 * * * find /var/lib/econspace/world/logs -name 'server-*.log' -mtime +30 -delete
```

## 8. Backups

`backup` copies the world while the server runs: it saves first, and nothing in the world moves
while the files are copied, so the copy is one moment of the world. It keeps the newest ten
(`backupsKept` in server.json). Every night at five:

```sh
sudo crontab -u econspace -e
# then add:
0 5 * * * /opt/econspace/econserver admin --world /var/lib/econspace/world backup >/dev/null
```

A copy on the same disk does not survive the disk. Copy `backups/` somewhere else now and then,
for example with `rsync -a` to another machine.

**Restoring** a backup:

```sh
sudo systemctl stop econserver
cd /var/lib/econspace
sudo -u econspace mv world/world.json world/regions world/accounts /tmp/   # out of the way
sudo -u econspace cp -a world/backups/2026-10-17T05-00-00Z/. world/
sudo systemctl start econserver
```

## 9. Updating

1. Back up: `econ backup`.
2. Warn the players: `econ shutdown 300` (five minutes), or stop it now.
3. `sudo systemctl stop econserver`
4. Unpack the new build over `/opt/econspace` (as in section 1). The world is in
   `/var/lib/econspace/world`, so replacing the program does not touch it.
5. `sudo systemctl start econserver` and check `journalctl -u econserver -n 50`.

A newer server may speak a newer protocol. A friend with an older client is then refused with
a message that names both versions; they need the new client too.

## 10. The friend's side (Windows)

1. Ask the owner to add your account name to the allowlist.
2. Download `econspace-<version>-<codename>-windows.zip` and unzip it anywhere (no installer, no admin rights).
   Windows may warn that the program is unrecognised: *More info* -> *Run anyway*.
3. Edit `connect-example.bat` (right click -> *Edit*): put in the server's address, your
   account name and a secret of your own, and save it -- or make a copy, such as
   `play-on-bobs-server.bat`:

   ```bat
   start "" econspace.exe connect 203.0.113.7 50800 alice my-own-secret
   ```

4. Double-click it. The first login creates your account with that secret; use the same one
   every time after. If you are told the server *is by invitation* and you are *not on its
   list*, the owner has not added your name yet (or it is spelled differently).

The same from a terminal: `econspace.exe connect <address> <port> <name> <secret>`.

## Hosting on Windows instead

Unzip the Windows package, then in a terminal in that folder:

```bat
econserver.exe init world
econserver.exe host --world world
```

Forward the port on the router as in section 5, and allow `econserver.exe` through the
Windows firewall when it asks. The console is the window: type commands there; `Ctrl+C`, or
closing the window, saves and stops. `start-server.bat` starts a server whose world is the
folder itself, as before `--world` existed.

## A world from before --world

Without `--world` the world is the folder `econserver` is in, as it always was. Nothing moves
on its own: a server started with `--world` on a new folder while an old `world.json` sits
beside the program says so in its log and makes a new world. To carry the old one over, stop
the server and copy `world.json`, `regions/`, `accounts/`, `designs.json`, `corps.json`,
`allowlist.txt` and `bans.txt` into the new world directory before starting it.

EconSpace -- portable build
===========================

This folder runs from anywhere: the programs read the game data from data/ beside them
(or from the folder given with --data <dir>).

  econspace   the game client
  econserver  the server -- the world lives here; every player connects to one
  econagent   an MCP server, so an AI agent can play (see docs/agents in the repository)
  worldeditor the world editor; `worldeditor gallery` shows every kind of object at once

Host a server
-------------
HOSTING.md is the whole guide: a headless Linux box with systemd, the router, backups,
updates, and what your friends do. In short:

  econserver init world                 a world folder with server.json and allowlist.txt
  econserver host --world world         run it (open TCP 50800 on the router)
  econserver admin world status         console commands for a server running elsewhere

Without --world the world is kept in this folder, as before:

  econserver host 50800             this machine only
  econserver host 50800 --public    reachable from other machines (open TCP 50800)

Either way the world (world.json, regions/, corps.json, designs.json), its accounts
(accounts/) and who may come in (allowlist.txt, bans.txt) are in the world folder. Keep it out of the program's folder, or
copy it somewhere safe before replacing the folder with a newer build. The console's
`backup` command copies it to backups/ while the server runs.

Connect
-------
  econspace connect <server address> 50800 <name> <secret>

A client plays only on a server of the same release with the same data/: anything else is
refused with a message that says which release to get. `econspace --version` (and
`econserver --version`) say which one this is.

The first login with a name creates that account and sets its secret. There is no
encryption on the connection: use a secret you use nowhere else.

On Windows, start-server.bat and connect-example.bat do the same with a double-click.

macOS
-----
The build is not signed yet, so macOS refuses to open a downloaded copy ("cannot be opened
because the developer cannot be verified"). Either right-click (Control-click) a program in
Finder, choose Open and confirm once, or clear the quarantine flag on the whole folder in
Terminal:

  xattr -dr com.apple.quarantine econspace-<version>-<codename>-macos

Then run the programs from Terminal in this folder, as above: ./econspace connect ...
The build is universal (Apple silicon and Intel) and needs macOS 11 or later.
